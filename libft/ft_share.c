/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_share.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ft_plan.h"

#define MM_PER_INCH 25.4
/* the allowed target range, pixels */
#define MIN_TARGET_PIXELS 100
#define MAX_TARGET_PIXELS 20000
/* the largest margin, mm */
#define MAX_MARGIN_MM 10
/* slack when comparing times, positions and scales */
#define TOLERANCE 1e-9
#define SCALE_TOLERANCE 1e-6
/* the lowest ppi a scan can have, and the largest bin */
#define MIN_PPI 525
#define MAX_BIN 12
/* the highest frame number a list may name */
#define MAX_LIST_NUMBER 1000000

/* The bins the scanner reads with, largest first. */
static const int32_t BINS_LARGEST_FIRST[] = {12, 6, 4, 3, 2, 1};
#define BIN_COUNT (sizeof BINS_LARGEST_FIRST / sizeof BINS_LARGEST_FIRST[0])

void ft_init_share_options(ft_share_options* options, double scan_length_mm) {
  memset(options, 0, sizeof *options);
  options->target_pixels = FT_SHARE_TARGET_PIXELS;
  options->margin_mm = FT_SHARE_MARGIN_MM;
  options->scan_length_mm = scan_length_mm;
}

static double get_own_long_side_mm(const ft_share_frame* frame) {
  return frame->length_mm > frame->width_mm ? frame->length_mm
                                            : frame->width_mm;
}

double ft_get_share_long_side_mm(const ft_share_frame* frame,
                                 bool half_frames_at_full_frame_ppi) {
  if(half_frames_at_full_frame_ppi && frame->type == FT_FRAME_HALF) {
    return FT_SHARE_FULL_FRAME_LONG_MM;
  }
  return get_own_long_side_mm(frame);
}

double ft_get_share_target_pixels(const ft_share_frame* frame,
                                  const ft_share_options* options) {
  if(options->half_frames_at_full_frame_ppi && frame->type == FT_FRAME_HALF) {
    return options->target_pixels *
           get_own_long_side_mm(frame) /
           FT_SHARE_FULL_FRAME_LONG_MM;
  }
  return options->target_pixels;
}

double ft_get_share_ppi_needed(const ft_share_frame* frames,
                               uint32_t frame_count,
                               const ft_share_options* options) {
  double needed = 0;
  for(uint32_t index = 0; index < frame_count; index++) {
    double long_mm = ft_get_share_long_side_mm(
      &frames[index], options->half_frames_at_full_frame_ppi);
    double ppi =
      long_mm > 0 ? options->target_pixels / (long_mm / MM_PER_INCH) : 0;
    if(ppi > needed) needed = ppi;
  }
  return ceil(needed - TOLERANCE);
}

/* One window over frames[first..last]: margins out, clipped to the
 * holder. */
static void make_window(const ft_share_frame* frames,
                        const ft_share_options* options, uint32_t first,
                        uint32_t last, ft_share_window* window) {
  double start = frames[first].along_start_mm - options->margin_mm;
  double end =
    frames[last].along_start_mm + frames[last].length_mm + options->margin_mm;
  if(start < 0) start = 0;
  if(end > options->scan_length_mm) end = options->scan_length_mm;
  window->start_mm = start;
  window->length_mm = end - start;
  window->first_frame = first;
  window->last_frame = last;
}

/* Check the options and frames. Returns 0, or -1 with error filled. */
static int32_t check_share_input(const ft_share_frame* frames,
                                 uint32_t frame_count,
                                 const ft_share_options* options,
                                 ft_err* error) {
  if(frame_count < 1 || frame_count > FT_MAX_FRAMES) {
    return ft_fail(error, "share: %u frames selected (1..%d)",
                   (unsigned)frame_count, FT_MAX_FRAMES);
  }
  /* written as "not inside" so that NaN is refused too */
  bool are_options_valid = options->target_pixels >= MIN_TARGET_PIXELS &&
                           options->target_pixels <= MAX_TARGET_PIXELS &&
                           options->margin_mm >= 0 &&
                           options->margin_mm <= MAX_MARGIN_MM &&
                           options->scan_length_mm > 0;
  if(are_options_valid == false) return ft_fail(error, "share: bad options");
  for(uint32_t index = 0; index < frame_count; index++) {
    const ft_share_frame* frame = &frames[index];
    bool is_inside =
      frame->length_mm > 0 &&
      frame->width_mm > 0 &&
      frame->along_start_mm >= 0 &&
      frame->along_start_mm + frame->length_mm <= options->scan_length_mm;
    if(is_inside == false) {
      return ft_fail(error, "share: frame %u outside the holder",
                     (unsigned)frame->index);
    }
    if(index == 0) continue;
    const ft_share_frame* previous = &frames[index - 1];
    if(!(frame->along_start_mm >=
         previous->along_start_mm + previous->length_mm - TOLERANCE)) {
      return ft_fail(error, "share: frame %u overlaps or precedes frame %u",
                     (unsigned)frame->index, (unsigned)previous->index);
    }
  }
  return 0;
}

int32_t ft_estimate_share(const ft_share_frame* frames, uint32_t frame_count,
                          const ft_share_options* options, double ppi,
                          int32_t bin, const ft_lens* lens,
                          ft_share_estimate* estimate, ft_err* error) {
  if(check_share_input(frames, frame_count, options, error) != 0) return -1;
  double scale_ppi = ppi * bin;
  /* the bin divides 12, and the optics are in focus range */
  bool is_usable = bin >= 1 &&
                   bin <= MAX_BIN &&
                   MAX_BIN % bin == 0 &&
                   scale_ppi >= FT_SCALE_MIN_PPI - SCALE_TOLERANCE &&
                   scale_ppi <= FT_SCALE_MAX_PPI + SCALE_TOLERANCE;
  if(is_usable == false) {
    return ft_fail(
      error, "share: %g ppi x bin %d = optics %g, outside %.0f..%.0f", ppi,
      (int)bin, scale_ppi, FT_SCALE_MIN_PPI, FT_SCALE_MAX_PPI);
  }
  memset(estimate, 0, sizeof *estimate);
  estimate->ppi = ppi;
  estimate->bin = bin;
  estimate->scale_ppi = scale_ppi;
  estimate->relative_light = ft_estimate_relative_light(lens, scale_ppi);
  estimate->dmax = ft_estimate_dmax_for_light(estimate->relative_light);

  /* The dynamic program. best_seconds[end]: the cheapest scan of frames
   * 0..end-1; last_window_first[end]: its last window's first frame. */
  double best_seconds[FT_MAX_FRAMES + 1];
  uint32_t last_window_first[FT_MAX_FRAMES + 1];
  best_seconds[0] = 0;
  for(uint32_t end = 1; end <= frame_count; end++) {
    best_seconds[end] = HUGE_VAL;
    for(uint32_t start = 0; start < end; start++) {
      ft_share_window window;
      make_window(frames, options, start, end - 1, &window);
      double seconds =
        best_seconds[start] +
        ft_estimate_scan_seconds(ppi, (uint32_t)bin, window.length_mm);
      if(seconds < best_seconds[end] - TOLERANCE) {
        best_seconds[end] = seconds;
        last_window_first[end] = start;
      }
    }
  }
  estimate->scan_seconds = best_seconds[frame_count];
  /* walk back from the last frame, then reverse into strip order */
  uint32_t window_count = 0;
  for(uint32_t end = frame_count; end > 0; end = last_window_first[end]) {
    make_window(frames, options, last_window_first[end], end - 1,
                &estimate->windows[window_count]);
    window_count++;
  }
  estimate->window_count = window_count;
  /* one focus bracket for the job, before its first window */
  estimate->focus_seconds =
    window_count > 0 ? FT_SHARE_FOCUS_CROPS * FT_FOCUS_SAMPLE_SECONDS : 0;
  estimate->seconds = estimate->scan_seconds + estimate->focus_seconds;
  for(uint32_t index = 0; index < window_count / 2; index++) {
    ft_share_window swap = estimate->windows[index];
    estimate->windows[index] = estimate->windows[window_count - 1 - index];
    estimate->windows[window_count - 1 - index] = swap;
  }
  return 0;
}

int32_t ft_find_share_candidates(const ft_share_frame* frames,
                                 uint32_t frame_count,
                                 const ft_share_options* options,
                                 const ft_lens* lens,
                                 ft_share_estimate candidates[6],
                                 ft_err* error) {
  if(frame_count < 1) return ft_fail(error, "share: no frames selected");
  double needed = ft_get_share_ppi_needed(frames, frame_count, options);
  if(!(needed > 0)) return ft_fail(error, "share: frames have no size");
  int32_t candidate_count = 0;
  for(size_t index = 0; index < BIN_COUNT; index++) {
    int32_t bin = BINS_LARGEST_FIRST[index];
    /* raise the ppi where the bin's optics would be too low */
    double ppi = needed;
    double lowest_ppi = ceil(FT_SCALE_MIN_PPI / bin - TOLERANCE);
    if(ppi < lowest_ppi) ppi = lowest_ppi;
    if(ppi < MIN_PPI || ppi * bin > FT_SCALE_MAX_PPI + SCALE_TOLERANCE) {
      continue;
    }
    if(ft_estimate_share(frames, frame_count, options, ppi, bin, lens,
                         &candidates[candidate_count], error) != 0) {
      return -1;
    }
    candidate_count++;
  }
  if(candidate_count == 0) {
    return ft_fail(error, "share: %g ppi is beyond the optics (max %.0f)",
                   needed, FT_SCALE_MAX_PPI);
  }
  return candidate_count;
}

uint32_t ft_pick_most_light(const ft_share_estimate* candidates,
                            uint32_t candidate_count) {
  uint32_t best = 0;
  for(uint32_t index = 1; index < candidate_count; index++) {
    const ft_share_estimate* candidate = &candidates[index];
    bool has_more_light = candidate->dmax > candidates[best].dmax + TOLERANCE;
    bool is_tie_but_faster =
      fabs(candidate->dmax - candidates[best].dmax) <= TOLERANCE &&
      candidate->seconds < candidates[best].seconds;
    if(has_more_light || is_tie_but_faster) best = index;
  }
  return best;
}

/* Read a number at *cursor (digits only). Returns it, or -1 when there
 * is none or it is too big. */
static int64_t read_number(const char** cursor) {
  if(**cursor < '0' || **cursor > '9') return -1;
  int64_t value = 0;
  while(**cursor >= '0' && **cursor <= '9') {
    value = value * 10 + (**cursor - '0');
    if(value > MAX_LIST_NUMBER) return -1;
    (*cursor)++;
  }
  return value;
}

static bool does_job_have(const ft_job* job, int64_t frame_number) {
  for(uint32_t index = 0; index < job->frame_count; index++) {
    if(job->frames[index].index == frame_number) return true;
  }
  return false;
}

/* Mark the job frames the list names. Returns 0, or -1 with error
 * filled. */
static int32_t mark_listed_frames(const ft_job* job, const char* list,
                                  bool* is_wanted, ft_err* error) {
  const char* cursor = list;
  while(true) {
    int64_t first = read_number(&cursor);
    if(first < 0) return ft_fail(error, "share: bad frame list '%s'", list);
    int64_t last = first;
    if(*cursor == '-') {
      cursor++;
      last = read_number(&cursor);
      if(last < first) {
        return ft_fail(error, "share: bad frame list '%s'", list);
      }
    }
    for(int64_t number = first; number <= last; number++) {
      if(does_job_have(job, number) == false) {
        return ft_fail(error, "share: the job has no frame %d", (int)number);
      }
      for(uint32_t index = 0; index < job->frame_count; index++) {
        if(job->frames[index].index == number) is_wanted[index] = true;
      }
    }
    if(*cursor == '\0') return 0;
    if(*cursor != ',') {
      return ft_fail(error, "share: bad frame list '%s'", list);
    }
    cursor++;
  }
}

/* the planner's view of a job frame */
static ft_share_frame make_share_frame(const ft_job_frame* frame) {
  ft_share_frame share = {.index = frame->index,
                          .type = frame->type,
                          .along_start_mm = frame->along_start_mm,
                          .length_mm = frame->length_mm,
                          .across_start_mm = frame->across_start_mm,
                          .width_mm = frame->width_mm};
  return share;
}

int32_t ft_select_share_frames(const ft_job* job, const char* list,
                               ft_share_frame* frames, uint32_t* frame_count,
                               ft_err* error) {
  bool is_wanted[FT_MAX_FRAMES];
  memset(is_wanted, 0, sizeof is_wanted);
  if(list == NULL || list[0] == '\0') {
    for(uint32_t index = 0; index < job->frame_count; index++) {
      is_wanted[index] = job->frames[index].type != FT_FRAME_PARTIAL;
    }
  } else if(mark_listed_frames(job, list, is_wanted, error) != 0) {
    return -1;
  }
  uint32_t count = 0;
  for(uint32_t index = 0; index < job->frame_count; index++) {
    if(is_wanted[index] == false) continue;
    ft_share_frame share = make_share_frame(&job->frames[index]);
    /* insertion sort by along_start_mm: detection writes strip order
     * already */
    uint32_t position = count;
    while(position > 0 &&
          frames[position - 1].along_start_mm > share.along_start_mm) {
      frames[position] = frames[position - 1];
      position--;
    }
    frames[position] = share;
    count++;
  }
  if(count == 0) return ft_fail(error, "share: no frames selected");
  *frame_count = count;
  return 0;
}

/* the window (from 0) that holds selected frame number selected_index */
static uint32_t find_window(const ft_share_estimate* estimate,
                            uint32_t selected_index) {
  uint32_t window = 0;
  while(window < estimate->window_count &&
        selected_index > estimate->windows[window].last_frame) {
    window++;
  }
  return window;
}

void ft_apply_share_plan(ft_job* job, const ft_share_frame* selected,
                         uint32_t selected_count,
                         const ft_share_options* options,
                         const ft_share_estimate* estimate) {
  job->share_pixels = options->target_pixels;
  job->half_frames_at_full_frame_ppi = options->half_frames_at_full_frame_ppi;
  for(uint32_t index = 0; index < job->frame_count; index++) {
    ft_job_frame* frame = &job->frames[index];
    frame->is_included = false;
    frame->group = 0;
    uint32_t selected_index = 0;
    while(selected_index < selected_count &&
          selected[selected_index].index != frame->index) {
      selected_index++;
    }
    if(selected_index == selected_count) continue;
    frame->is_included = true;
    frame->ppi = estimate->ppi;
    frame->bin = estimate->bin;
    frame->group = (int32_t)find_window(estimate, selected_index) + 1;
  }
}

int32_t ft_plan_share_sizes(ft_job* job, const double* target_pixels,
                            bool half_frames_at_full_frame_ppi,
                            const ft_lens* lens, double scan_length_mm,
                            ft_err* error) {
  /* the chosen frames in strip order, and their targets */
  ft_share_frame selected[FT_MAX_FRAMES];
  double targets[FT_MAX_FRAMES];
  double smallest_target = 0;
  uint32_t selected_count = 0;
  for(uint32_t index = 0; index < job->frame_count; index++) {
    const ft_job_frame* frame = &job->frames[index];
    double target = target_pixels[index];
    if(target == 0) continue;
    if(!(target >= MIN_TARGET_PIXELS && target <= MAX_TARGET_PIXELS)) {
      return ft_fail(error,
                     "share: frame %u: %g px is out of range (100..20000)",
                     (unsigned)frame->index, target);
    }
    ft_share_frame share = make_share_frame(frame);
    uint32_t position = selected_count;
    while(position > 0 &&
          selected[position - 1].along_start_mm > share.along_start_mm) {
      selected[position] = selected[position - 1];
      targets[position] = targets[position - 1];
      position--;
    }
    selected[position] = share;
    targets[position] = target;
    selected_count++;
    if(smallest_target == 0 || target < smallest_target) {
      smallest_target = target;
    }
  }
  if(selected_count == 0) return ft_fail(error, "share: no frames selected");
  for(uint32_t index = 0; index < job->frame_count; index++) {
    job->frames[index].is_included = false;
    job->frames[index].group = 0;
    job->frames[index].share_pixels = 0;
  }

  /* each run of one size, priced on its own */
  int32_t group_count = 0;
  ft_share_estimate candidates[6];
  uint32_t run_end;
  for(uint32_t run_start = 0; run_start < selected_count;
      run_start = run_end + 1) {
    run_end = run_start;
    while(run_end + 1 < selected_count &&
          targets[run_end + 1] == targets[run_start]) {
      run_end++;
    }
    ft_share_options options;
    ft_init_share_options(&options, scan_length_mm);
    options.target_pixels = targets[run_start];
    options.half_frames_at_full_frame_ppi = half_frames_at_full_frame_ppi;
    int32_t candidate_count =
      ft_find_share_candidates(selected + run_start, run_end - run_start + 1,
                               &options, lens, candidates, error);
    if(candidate_count < 1) return -1;
    const ft_share_estimate* estimate =
      &candidates[ft_pick_most_light(candidates, (uint32_t)candidate_count)];
    for(uint32_t chosen = run_start; chosen <= run_end; chosen++) {
      uint32_t window = find_window(estimate, chosen - run_start);
      for(uint32_t index = 0; index < job->frame_count; index++) {
        ft_job_frame* frame = &job->frames[index];
        if(frame->index != selected[chosen].index) continue;
        frame->is_included = true;
        frame->ppi = estimate->ppi;
        frame->bin = estimate->bin;
        frame->group = group_count + (int32_t)window + 1;
        /* only frames of another size than the strip's carry their own */
        frame->share_pixels =
          targets[run_start] == smallest_target ? 0 : targets[run_start];
      }
    }
    group_count += (int32_t)estimate->window_count;
  }
  job->share_pixels = smallest_target;
  job->half_frames_at_full_frame_ppi = half_frames_at_full_frame_ppi;
  /* measured on the first window */
  job->focus_mode = FT_FOCUS_PER_FRAME;
  job->focus_offset_steps = 0;
  return group_count;
}

int32_t ft_get_share_group_window(const ft_job* job, int32_t group,
                                  const ft_share_options* options,
                                  double* start_mm, double* length_mm) {
  double start = HUGE_VAL;
  double end = -HUGE_VAL;
  for(uint32_t index = 0; index < job->frame_count; index++) {
    const ft_job_frame* frame = &job->frames[index];
    if(frame->is_included == false || frame->group != group) continue;
    if(frame->along_start_mm < start) start = frame->along_start_mm;
    if(frame->along_start_mm + frame->length_mm > end) {
      end = frame->along_start_mm + frame->length_mm;
    }
  }
  if(start > end) return -1;
  start -= options->margin_mm;
  end += options->margin_mm;
  if(start < 0) start = 0;
  if(end > options->scan_length_mm) end = options->scan_length_mm;
  *start_mm = start;
  *length_mm = end - start;
  return 0;
}

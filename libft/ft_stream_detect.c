/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Streaming frame detection (ft_stream_detect.h). */
#include "ft_stream_detect.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* all channels above: open beam (as ft_detect) */
#define OPEN_TRANSMITTANCE 0.85f
/* all channels below: holder metal (as ft_detect) */
#define DARK_TRANSMITTANCE 0.005f
/* the band this much open: no film on the line */
#define NO_FILM_OPEN_SHARE 0.50
/* the band this much dark: metal (negatives) */
#define METAL_SHARE 0.80
/* a flat line: 3rd..97th percentile per channel within this density */
#define FLAT_DENSITY 0.12
/* a slide gap: 97 % of the band denser than this */
#define SLIDE_GAP_DENSITY 2.0
/* base samples kept: ~10 mm of gap lines */
#define BASE_MM 10.0
/* a run shorter than this is dropped */
#define SLIVER_MM 3.0
/* frame lengths, mm: full 33..39.5, half 15.5..21 */
#define FULL_MIN_MM 33.0
#define FULL_MAX_MM 39.5
#define HALF_MIN_MM 15.5
#define HALF_MAX_MM 21.0
#define FULL_LENGTH_MM 36.0
#define HALF_LENGTH_MM 18.0
/* at the film's ends: this close to 36 / 18 */
#define END_TOLERANCE_MM 1.5
/* where the holder grid cuts a run with no gap */
#define GRID_MM 36.0
/* a column belongs to the image: off the base on this share of lines */
#define EXPOSED_SHARE 0.30
/* across: the most unexposed columns crossed while growing */
#define GROW_GAP_MM 0.4
/* density histogram: BINS bins of STEP density, 0..4 */
#define BINS 80
#define STEP 0.05
/* pixels averaged for the line statistics */
#define BLOCK 4
/* the least ring of base samples */
#define MIN_RING 8
/* the densest a pixel reads: transmittance 1e-4 */
#define MIN_TRANSMITTANCE 1e-4

/* What a line shows. */
typedef enum line_kind { LINE_NO_FILM, LINE_GAP, LINE_FRAME } line_kind;

/* density from transmittance, floored at MIN_TRANSMITTANCE */
static double get_density(double transmittance) {
  if(!(transmittance > MIN_TRANSMITTANCE)) transmittance = MIN_TRANSMITTANCE;
  return -log10(transmittance);
}

static bool is_open(const float* pixel) {
  return pixel[0] > OPEN_TRANSMITTANCE &&
         pixel[1] > OPEN_TRANSMITTANCE &&
         pixel[2] > OPEN_TRANSMITTANCE;
}

static bool is_dark(const float* pixel) {
  return pixel[0] < DARK_TRANSMITTANCE &&
         pixel[1] < DARK_TRANSMITTANCE &&
         pixel[2] < DARK_TRANSMITTANCE;
}

/* The k-th smallest of values[0..count), by quickselect (Hoare
 * partition). Reorders values. */
static float select_kth(float* values, int32_t count, int32_t k) {
  int32_t low = 0;
  int32_t high = count - 1;
  while(low < high) {
    float pivot = values[(low + high) / 2];
    int32_t left = low;
    int32_t right = high;
    while(left <= right) {
      while(values[left] < pivot) left++;
      while(values[right] > pivot) right--;
      if(left <= right) {
        float swap = values[left];
        values[left] = values[right];
        values[right] = swap;
        left++;
        right--;
      }
    }
    if(k <= right) {
      high = right;
    } else if(k >= left) {
      low = left;
    } else {
      break;
    }
  }
  return values[k];
}

/* the column at mm across, rounded up or down */
static int32_t get_column(double mm, const ft_axes* axes, bool round_up) {
  double column = (mm - axes->first_pixel_mm) / axes->mm_per_pixel;
  if(round_up) return (int32_t)ceil(column - 1e-9);
  return (int32_t)floor(column + 1e-9);
}

int32_t ft_init_stream_detector(ft_stream_detector* detector,
                                const ft_axes* axes, uint32_t pixels,
                                ft_film film, const ft_holder* holder,
                                ft_err* error) {
  ft_stream_detector* d = detector;
  memset(d, 0, sizeof *d);
  double mm_per_line = axes != NULL ? axes->mm_per_line : 0;
  if(holder == NULL ||
     axes == NULL ||
     pixels < 1 ||
     pixels > INT32_MAX ||
     !(mm_per_line > 0) ||
     !(axes->mm_per_pixel > 0)) {
    return ft_fail(error, "stream detect: bad geometry");
  }
  d->axes = *axes;
  d->pixels = (int32_t)pixels;
  d->film = film;
  d->holder = holder;
  d->band_start = get_column(holder->band_start_mm, axes, true);
  d->band_end = get_column(holder->band_end_mm, axes, false);
  d->search_start = get_column(holder->image_start_mm, axes, false);
  d->search_end = get_column(holder->image_end_mm, axes, true);
  if(d->band_start < 0) d->band_start = 0;
  if(d->band_end > d->pixels) d->band_end = d->pixels;
  if(d->search_start < 0) d->search_start = 0;
  if(d->search_end > d->pixels) d->search_end = d->pixels;
  if(d->band_end - d->band_start < 8 * BLOCK) {
    return ft_fail(error,
                   "stream detect: the holder's image band is not on the "
                   "line");
  }
  /* the search covers at least the band */
  if(d->search_start > d->band_start) d->search_start = d->band_start;
  if(d->search_end < d->band_end) d->search_end = d->band_end;
  d->gap_lines = (int32_t)ceil(FT_STREAM_DETECT_GAP_MM / mm_per_line - 1e-9);
  d->weak_lines = d->gap_lines / 2 < 3 ? 3 : d->gap_lines / 2;
  d->grid_lines = (int32_t)floor(FULL_MAX_MM / mm_per_line + 1e-9);
  d->cut_lines = (int32_t)floor(GRID_MM / mm_per_line + 0.5);
  d->ring_capacity = (int32_t)floor(BASE_MM / mm_per_line + 0.5);
  if(d->ring_capacity < MIN_RING) d->ring_capacity = MIN_RING;
  if(d->ring_capacity > FT_STREAM_DETECT_RING_SIZE) {
    d->ring_capacity = FT_STREAM_DETECT_RING_SIZE;
  }
  d->run_start = -1;
  d->weak_start = -1;
  d->weak_end = -1;
  d->histogram = calloc((size_t)pixels * 3 * BINS, sizeof *d->histogram);
  d->valid_lines = calloc(pixels, sizeof *d->valid_lines);
  d->scratch =
    malloc((size_t)(d->search_end - d->search_start) * 3 * sizeof *d->scratch);
  if(d->histogram == NULL || d->valid_lines == NULL || d->scratch == NULL) {
    ft_free_stream_detector(d);
    return ft_fail(error, "stream detect: out of memory");
  }
  return 0;
}

void ft_free_stream_detector(ft_stream_detector* detector) {
  free(detector->histogram);
  free(detector->valid_lines);
  free(detector->scratch);
  detector->histogram = NULL;
  detector->valid_lines = NULL;
  detector->scratch = NULL;
}

bool ft_get_stream_base(const ft_stream_detector* detector, float base[3]) {
  if(detector->has_base == false) return false;
  for(uint32_t channel = 0; channel < 3; channel++) {
    base[channel] = (float)pow(10, -detector->base_density[channel]);
  }
  return true;
}

/* the base: the median of the ring per channel */
static void update_base(ft_stream_detector* d) {
  float values[FT_STREAM_DETECT_RING_SIZE];
  for(uint32_t channel = 0; channel < 3; channel++) {
    for(int32_t index = 0; index < d->ring_count; index++) {
      values[index] = d->ring[index][channel];
    }
    d->base_density[channel] =
      select_kth(values, d->ring_count, d->ring_count / 2);
  }
  d->has_base = true;
}

/* add a base sample (3 densities); should_reset empties the ring first */
static void push_base_sample(ft_stream_detector* d, const float* sample,
                             bool should_reset) {
  if(should_reset) {
    d->ring_count = 0;
    d->ring_position = 0;
  }
  memcpy(d->ring[d->ring_position], sample, sizeof d->ring[0]);
  d->ring_position = (d->ring_position + 1) % d->ring_capacity;
  if(d->ring_count < d->ring_capacity) d->ring_count++;
  update_base(d);
}

/* start the histograms of a new run */
static void reset_run(ft_stream_detector* d) {
  memset(d->histogram, 0, (size_t)d->pixels * 3 * BINS * sizeof *d->histogram);
  memset(d->valid_lines, 0, (size_t)d->pixels * sizeof *d->valid_lines);
}

/* Is column of the open run off the base on >= EXPOSED_SHARE of its
 * lines, in any channel? */
static bool is_column_exposed(const ft_stream_detector* d, int32_t column) {
  const uint16_t* histogram = d->histogram + (size_t)column * 3 * BINS;
  int64_t valid = d->valid_lines[column];
  if(valid == 0) return false;
  for(uint32_t channel = 0; channel < 3; channel++) {
    const uint16_t* bins = histogram + channel * BINS;
    int64_t exposed_count = 0;
    if(d->film == FT_FILM_SLIDE) {
      /* a slide's image is thinner than its dark gaps */
      int32_t end_bin = (int32_t)(SLIDE_GAP_DENSITY / STEP + 0.5);
      for(int32_t bin = 0; bin < end_bin; bin++) exposed_count += bins[bin];
    } else {
      /* a negative's image is denser than its base */
      double base = 0;
      if(d->has_base) {
        base = d->base_density[channel];
      } else if(d->has_min_density) {
        base = d->min_density[channel];
      }
      int32_t first_bin =
        (int32_t)ceil((base + FT_STREAM_DETECT_NEAR_DENSITY) / STEP - 1e-9);
      for(int32_t bin = first_bin < 0 ? 0 : first_bin; bin < BINS; bin++) {
        exposed_count += bins[bin];
      }
    }
    if(exposed_count >= EXPOSED_SHARE * valid) return true;
  }
  return false;
}

static double clamp(double value, double low, double high) {
  if(value < low) return low;
  if(value > high) return high;
  return value;
}

/* the type of a run of length_mm; is_bounded: a gap (or the grid) at
 * both ends */
static ft_frame_type classify_run(double length_mm, bool is_bounded) {
  if(is_bounded) {
    if(length_mm >= FULL_MIN_MM && length_mm <= FULL_MAX_MM) {
      return FT_FRAME_FULL;
    }
    if(length_mm >= HALF_MIN_MM && length_mm <= HALF_MAX_MM) {
      return FT_FRAME_HALF;
    }
    return FT_FRAME_PARTIAL;
  }
  if(fabs(length_mm - FULL_LENGTH_MM) <= END_TOLERANCE_MM) return FT_FRAME_FULL;
  if(fabs(length_mm - HALF_LENGTH_MM) <= END_TOLERANCE_MM) return FT_FRAME_HALF;
  return FT_FRAME_PARTIAL;
}

/* Seal the run [start, end) as a frame. starts_at_gap / ends_at_gap: its
 * ends are at a gap (or the grid); is_grid: cut by the holder grid.
 * Returns 1 when a frame was added, else 0. */
static uint32_t seal_run(ft_stream_detector* d, int64_t start, int64_t end,
                         bool starts_at_gap, bool ends_at_gap, bool is_grid) {
  const ft_axes* axes = &d->axes;
  double mm_per_line = axes->mm_per_line;
  double length_mm = (end - start) * mm_per_line;

  /* the film's end just after a short gap: the frame ended there */
  if(ends_at_gap == false &&
     is_grid == false &&
     d->weak_start > start &&
     (end - d->weak_end) * mm_per_line < SLIVER_MM) {
    end = d->weak_start;
    ends_at_gap = true;
    length_mm = (end - start) * mm_per_line;
  }
  if(end <= start) return 0;
  if(is_grid == false && length_mm < SLIVER_MM) return 0;
  if(d->frame_count == FT_MAX_FRAMES) return 0;

  ft_frame_type type = FT_FRAME_FULL;
  if(is_grid == false) {
    type = classify_run(length_mm, starts_at_gap && ends_at_gap);
  }

  /* across: the band, grown outward over the run's exposed columns,
   * crossing at most max_gap unexposed ones */
  int32_t max_gap = (int32_t)ceil(GROW_GAP_MM / axes->mm_per_pixel - 1e-9);
  int32_t first_column = d->band_start;
  int32_t end_column = d->band_end;
  int32_t unexposed_run = 0;
  for(int32_t column = d->band_start - 1;
      column >= d->search_start && unexposed_run <= max_gap; column--) {
    if(is_column_exposed(d, column)) {
      first_column = column;
      unexposed_run = 0;
    } else {
      unexposed_run++;
    }
  }
  unexposed_run = 0;
  for(int32_t column = d->band_end;
      column < d->search_end && unexposed_run <= max_gap; column++) {
    if(is_column_exposed(d, column)) {
      end_column = column + 1;
      unexposed_run = 0;
    } else {
      unexposed_run++;
    }
  }

  ft_stream_frame* frame = &d->frames[d->frame_count];
  memset(frame, 0, sizeof *frame);
  d->frame_count++;
  frame->index = d->frame_count;
  frame->type = type;
  frame->is_from_grid = is_grid;
  frame->first_line = start;
  frame->end_line = end;
  double scan_length_mm = d->holder->scan_length_mm;
  double scan_width_mm = d->holder->scan_width_mm;
  frame->start_mm =
    clamp(axes->first_line_mm + start * mm_per_line - FT_STREAM_DETECT_PAD_MM,
          0, scan_length_mm);
  frame->end_mm =
    clamp(axes->first_line_mm + end * mm_per_line + FT_STREAM_DETECT_PAD_MM, 0,
          scan_length_mm);
  frame->across_start_mm = clamp(axes->first_pixel_mm +
                                   first_column * axes->mm_per_pixel -
                                   FT_STREAM_DETECT_PAD_MM,
                                 0, scan_width_mm);
  frame->across_end_mm = clamp(axes->first_pixel_mm +
                                 end_column * axes->mm_per_pixel +
                                 FT_STREAM_DETECT_PAD_MM,
                               0, scan_width_mm);
  d->used_grid |= is_grid;
  return 1;
}

/* the open run takes a line: its columns' densities into the
 * histograms */
static void accumulate_line(ft_stream_detector* d, const float* line) {
  for(int32_t column = d->search_start; column < d->search_end; column++) {
    const float* pixel = line + (size_t)column * 3;
    uint16_t* histogram = d->histogram + (size_t)column * 3 * BINS;
    /* open beam never counts; dark (metal) does not on negatives */
    if(is_open(pixel)) continue;
    if(d->film != FT_FILM_SLIDE && is_dark(pixel)) continue;
    /* the count is 16 bits: stop at its top */
    if(d->valid_lines[column] == 0xffff) continue;
    d->valid_lines[column]++;
    for(uint32_t channel = 0; channel < 3; channel++) {
      int32_t bin = (int32_t)(get_density(pixel[channel]) / STEP);
      if(bin < 0) bin = 0;
      if(bin >= BINS) bin = BINS - 1;
      histogram[channel * BINS + bin]++;
    }
  }
}

/* Classify a line; the base and the thinnest density are updated. */
static line_kind classify_line(ft_stream_detector* d, const float* line) {
  int32_t band_pixels = d->band_end - d->band_start;
  int32_t open_count = 0;
  int32_t dark_count = 0;
  for(int32_t column = d->band_start; column < d->band_end; column++) {
    const float* pixel = line + (size_t)column * 3;
    open_count += is_open(pixel);
    dark_count += is_dark(pixel);
  }
  if(open_count >= NO_FILM_OPEN_SHARE * band_pixels) return LINE_NO_FILM;
  if(d->film != FT_FILM_SLIDE && dark_count >= METAL_SHARE * band_pixels) {
    return LINE_NO_FILM;
  }

  /* The band in blocks of BLOCK pixels: sensor noise on a dense base
   * (blue T ~0.05) would otherwise spread a gap line past FLAT_DENSITY.
   * Then the 3rd, 50th and 97th percentile per channel. */
  int32_t block_count = band_pixels / BLOCK;
  int32_t low = block_count * 3 / 100;
  int32_t high = block_count - 1 - block_count * 3 / 100;
  float percentile3[3];
  float percentile50[3];
  float percentile97[3];
  bool is_flat = true;
  for(uint32_t channel = 0; channel < 3; channel++) {
    float* densities = d->scratch + (size_t)channel * block_count;
    for(int32_t block = 0; block < block_count; block++) {
      const float* first =
        line + (size_t)(d->band_start + block * BLOCK) * 3 + channel;
      float sum = 0;
      for(int32_t index = 0; index < BLOCK; index++) sum += first[index * 3];
      densities[block] = (float)get_density(sum / BLOCK);
    }
    percentile3[channel] = select_kth(densities, block_count, low);
    percentile97[channel] = select_kth(densities, block_count, high);
    percentile50[channel] = select_kth(densities, block_count, block_count / 2);
    if(percentile97[channel] - percentile3[channel] > FLAT_DENSITY) {
      is_flat = false;
    }
  }

  if(d->film == FT_FILM_SLIDE) {
    bool is_gap = true;
    for(uint32_t channel = 0; channel < 3; channel++) {
      if(!(percentile3[channel] >= SLIDE_GAP_DENSITY)) is_gap = false;
    }
    if(is_gap) push_base_sample(d, percentile50, false);
    return is_gap ? LINE_GAP : LINE_FRAME;
  }

  /* a line without open beam: track the thinnest film density */
  if(open_count == 0) {
    for(uint32_t channel = 0; channel < 3; channel++) {
      if(d->has_min_density == false ||
         percentile3[channel] < d->min_density[channel]) {
        d->min_density[channel] = percentile3[channel];
      }
    }
    d->has_min_density = true;
  }
  if(is_flat) {
    /* the mean difference from the base, and is every channel near it */
    double mean_difference = 0;
    bool is_near = true;
    for(uint32_t channel = 0; channel < 3 && d->has_base; channel++) {
      double difference = percentile50[channel] - d->base_density[channel];
      mean_difference += difference / 3;
      if(!(fabs(difference) <= FT_STREAM_DETECT_NEAR_DENSITY)) is_near = false;
    }
    if(d->has_base == false ||
       mean_difference < -FT_STREAM_DETECT_NEAR_DENSITY) {
      /* thinner than the base: that is the base */
      push_base_sample(d, percentile50, true);
    } else if(is_near) {
      push_base_sample(d, percentile50, false);
    }
  }
  bool is_gap = d->has_base;
  for(uint32_t channel = 0; channel < 3 && is_gap; channel++) {
    double base = d->base_density[channel];
    is_gap = percentile3[channel] >= base - FT_STREAM_DETECT_NEAR_DENSITY &&
             percentile97[channel] <= base + FT_STREAM_DETECT_NEAR_DENSITY;
  }
  return is_gap ? LINE_GAP : LINE_FRAME;
}

uint32_t ft_detect_stream_line(ft_stream_detector* detector,
                               const float* line) {
  ft_stream_detector* d = detector;
  line_kind kind = classify_line(d, line);
  int64_t line_number = d->lines_fed;
  d->lines_fed++;
  uint32_t sealed = 0;

  switch(kind) {
    case LINE_NO_FILM:
      /* no film: the open run ends, untrusted at this end */
      if(d->run_start >= 0) {
        int64_t end = d->gap_length > 0 ? d->gap_start : line_number;
        sealed +=
          seal_run(d, d->run_start, end, d->is_run_trusted, false, false);
      }
      d->run_start = -1;
      d->gap_length = 0;
      d->is_after_gap = false;
      return sealed;
    case LINE_GAP:
      if(d->gap_length == 0) d->gap_start = line_number;
      d->gap_length++;
      /* a full gap: the open run ends at it */
      if(d->gap_length == d->gap_lines) {
        if(d->run_start >= 0) {
          sealed += seal_run(d, d->run_start, d->gap_start, d->is_run_trusted,
                             true, false);
        }
        d->run_start = -1;
        d->is_after_gap = true;
      }
      return sealed;
    case LINE_FRAME:
      break;
  }

  /* a short gap just ended inside the run */
  if(d->run_start >= 0 && d->gap_length >= d->weak_lines) {
    if(d->is_run_trusted == false &&
       (d->gap_start - d->run_start) * d->axes.mm_per_line < SLIVER_MM) {
      /* the film's start: the short gap is the frame's edge */
      d->run_start = -1;
    } else {
      d->weak_start = d->gap_start;
      d->weak_end = line_number;
    }
  }
  if(d->run_start < 0) {
    d->is_run_trusted = d->is_after_gap || d->gap_length >= d->weak_lines;
    d->run_start = line_number;
    d->weak_start = -1;
    d->weak_end = -1;
    reset_run(d);
  }
  d->gap_length = 0;
  accumulate_line(d, line);
  /* no gap for too long: the holder grid cuts the run */
  if(line_number + 1 - d->run_start > d->grid_lines) {
    int64_t cut = d->run_start + d->cut_lines;
    sealed += seal_run(d, d->run_start, cut, true, true, true);
    d->run_start = cut;
    d->is_run_trusted = true;
    reset_run(d);
  }
  return sealed;
}

uint32_t ft_detect_stream_lines(ft_stream_detector* detector,
                                const float* lines, uint32_t line_count) {
  uint32_t sealed = 0;
  for(uint32_t index = 0; index < line_count; index++) {
    sealed += ft_detect_stream_line(
      detector, lines + (size_t)index * detector->pixels * 3);
  }
  return sealed;
}

uint32_t ft_end_stream_detection(ft_stream_detector* detector) {
  ft_stream_detector* d = detector;
  uint32_t sealed = 0;
  if(d->run_start >= 0) {
    int64_t end = d->gap_length > 0 ? d->gap_start : d->lines_fed;
    sealed = seal_run(d, d->run_start, end, d->is_run_trusted, false, false);
  }
  d->run_start = -1;
  d->gap_length = 0;
  return sealed;
}

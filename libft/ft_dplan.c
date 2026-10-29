/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_dplan.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "ft_geom.h"
#include "ft_plan.h"

#define MM_PER_INCH 25.4
/* a frame list's numbers have at most this many digits */
#define MAX_LIST_DIGITS 6

/* The scan window along the strip for frames[first..last], margins
 * included and clipped to the holder. Returns its length;
 * *start_mm receives its start. */
static double get_pass_window(const ft_detail_frame* frames, uint32_t first,
                              uint32_t last, double scan_length_mm,
                              double* start_mm) {
  double start = frames[first].start_mm - FT_DETAIL_MARGIN_MM;
  double end =
    frames[last].start_mm + frames[last].length_mm + FT_DETAIL_MARGIN_MM;
  if(start < 0) start = 0;
  if(end > scan_length_mm) end = scan_length_mm;
  *start_mm = start;
  return end - start;
}

/* Check the frames are inside the holder, in order and not overlapping.
 * Returns 0, or -1 with error filled. */
static int32_t check_frames(const ft_detail_frame* frames, uint32_t frame_count,
                            double scan_length_mm, ft_err* error) {
  for(uint32_t index = 0; index < frame_count; index++) {
    const ft_detail_frame* frame = &frames[index];
    /* written as "not inside" so that NaN is refused too */
    bool is_inside = frame->length_mm > 0 &&
                     frame->start_mm >= 0 &&
                     frame->start_mm + frame->length_mm <= scan_length_mm;
    if(is_inside == false) {
      return ft_fail(error, "detail plan: frame %u is outside the holder",
                     (unsigned)frame->index);
    }
    if(index == 0) continue;
    const ft_detail_frame* previous = &frames[index - 1];
    if(!(frame->start_mm >= previous->start_mm + previous->length_mm)) {
      return ft_fail(error,
                     "detail plan: frame %u overlaps or precedes frame %u",
                     (unsigned)frame->index, (unsigned)previous->index);
    }
  }
  return 0;
}

int32_t ft_make_detail_plan(const ft_detail_frame* frames, uint32_t frame_count,
                            double ppi, double scan_length_mm,
                            int64_t focus_span_steps, ft_detail_plan* plan,
                            ft_err* error) {
  if(frame_count < 1 || frame_count > FT_MAX_FRAMES) {
    return ft_fail(error, "detail plan: %u frames (1..%d)",
                   (unsigned)frame_count, FT_MAX_FRAMES);
  }
  if(ft_is_bin_usable(ppi, FT_DETAIL_BIN) == false) {
    return ft_fail(error,
                   "detail plan: %g ppi cannot be scanned at bin 1 "
                   "(optics %.0f..%.0f)",
                   ppi, FT_SCALE_MIN_PPI, FT_SCALE_MAX_PPI);
  }
  if(focus_span_steps < 0 || !(scan_length_mm > 0)) {
    return ft_fail(error, "detail plan: bad focus span or holder");
  }
  if(check_frames(frames, frame_count, scan_length_mm, error) != 0) return -1;

  /* The dynamic program. best_seconds[end]: the fastest plan for frames
   * 0..end-1. last_pass_first[end]: the first frame of that plan's last
   * pass. Each end tries every last pass that stays in focus. */
  double best_seconds[FT_MAX_FRAMES + 1];
  int32_t last_pass_first[FT_MAX_FRAMES + 1];
  best_seconds[0] = 0;
  for(uint32_t end = 1; end <= frame_count; end++) {
    int64_t lowest = frames[end - 1].focus_offset_steps;
    int64_t highest = lowest;
    best_seconds[end] = HUGE_VAL;
    last_pass_first[end] = -1;
    /* the last pass covers frames[start - 1 .. end - 1] */
    for(uint32_t start = end; start >= 1; start--) {
      int64_t focus = frames[start - 1].focus_offset_steps;
      if(focus < lowest) lowest = focus;
      if(focus > highest) highest = focus;
      /* adding more frames only widens the spread */
      if(highest - lowest > focus_span_steps) break;
      double window_start_mm;
      double window_length_mm = get_pass_window(
        frames, start - 1, end - 1, scan_length_mm, &window_start_mm);
      double seconds =
        best_seconds[start - 1] +
        ft_estimate_scan_seconds(ppi, FT_DETAIL_BIN, window_length_mm);
      if(seconds < best_seconds[end]) {
        best_seconds[end] = seconds;
        last_pass_first[end] = (int32_t)(start - 1);
      }
    }
  }

  /* Walk back from the last frame, one pass at a time. This gives the
   * passes from last to first; they are reversed below. A single frame
   * always fits a pass, so last_pass_first is never -1 here. */
  uint32_t pass_count = 0;
  for(uint32_t end = frame_count; end > 0;
      end = (uint32_t)last_pass_first[end]) {
    ft_detail_pass* pass = &plan->passes[pass_count];
    pass_count++;
    pass->first_frame = (uint32_t)last_pass_first[end];
    pass->last_frame = end - 1;
    int64_t lowest = frames[pass->first_frame].focus_offset_steps;
    int64_t highest = lowest;
    for(uint32_t index = pass->first_frame; index <= pass->last_frame;
        index++) {
      int64_t focus = frames[index].focus_offset_steps;
      if(focus < lowest) lowest = focus;
      if(focus > highest) highest = focus;
    }
    pass->focus_offset_steps = ft_get_focus_middle(lowest, highest);
    pass->length_mm =
      get_pass_window(frames, pass->first_frame, pass->last_frame,
                      scan_length_mm, &pass->start_mm);
    pass->seconds =
      ft_estimate_scan_seconds(ppi, FT_DETAIL_BIN, pass->length_mm);
  }
  for(uint32_t index = 0; index < pass_count / 2; index++) {
    ft_detail_pass swap = plan->passes[index];
    plan->passes[index] = plan->passes[pass_count - 1 - index];
    plan->passes[pass_count - 1 - index] = swap;
  }
  plan->pass_count = pass_count;
  plan->seconds = best_seconds[frame_count];
  return 0;
}

int64_t ft_get_focus_middle(int64_t low, int64_t high) {
  int64_t sum = low + high;
  /* halve, rounding half away from zero */
  if(sum >= 0) return (sum + 1) / 2;
  return -((-sum + 1) / 2);
}

/* Read up to MAX_LIST_DIGITS decimal digits at *cursor.
 * Returns how many digits were read; *value receives the number. */
static uint32_t read_frame_number(const char** cursor, int64_t* value) {
  uint32_t digit_count = 0;
  *value = 0;
  while(**cursor >= '0' && **cursor <= '9' && digit_count < MAX_LIST_DIGITS) {
    *value = *value * 10 + (**cursor - '0');
    (*cursor)++;
    digit_count++;
  }
  return digit_count;
}

int32_t ft_is_frame_selected(const char* list, uint32_t index, ft_err* error) {
  if(list == NULL || list[0] == '\0') {
    return ft_fail(error, "select: empty frame list");
  }
  const char* cursor = list;
  bool is_selected = false;
  while(true) {
    int64_t first;
    if(read_frame_number(&cursor, &first) == 0 || first < 1) {
      return ft_fail(error, "select: expected a frame number in '%s'", list);
    }
    int64_t last = first;
    if(*cursor == '-') {
      cursor++;
      if(read_frame_number(&cursor, &last) == 0 || last < first) {
        return ft_fail(error, "select: bad range in '%s'", list);
      }
    }
    if(index >= first && index <= last) is_selected = true;
    if(*cursor == '\0') return is_selected ? 1 : 0;
    if(*cursor != ',') {
      return ft_fail(error, "select: unexpected '%c' in '%s'", *cursor, list);
    }
    cursor++;
  }
}

/* round to the nearest integer, halves up */
static int64_t round_to_int64(double value) {
  return (int64_t)floor(value + 0.5);
}

void ft_make_detail_scan(double ppi, int32_t bin, double across_start_mm,
                         double width_mm, double along_start_mm,
                         double length_mm, ft_detail_scan* scan) {
  double scale_ppi = ppi * bin;
  double first_ccd_pixel = ft_get_ccd_pixel_from_mm(across_start_mm, scale_ppi);
  scan->ppi = ppi;
  scan->bin = bin;
  /* + 0.5 then truncate: Sancho's crop rounding, kept as it is */
  scan->first_pixel =
    (int64_t)((first_ccd_pixel - FT_CCD_FIRST_PIXEL) / bin + 0.5);
  scan->first_line = (int64_t)(along_start_mm / MM_PER_INCH * ppi + 0.5);
  scan->pixel_count = (int64_t)(width_mm / MM_PER_INCH * ppi + 0.5);
  scan->line_count = (int64_t)(length_mm / MM_PER_INCH * ppi + 0.5);
}

void ft_get_ccd_reach_mm(double ppi, int32_t bin, double* first_mm,
                         double* last_mm) {
  double scale_ppi = ppi * bin;
  *first_mm = ft_get_mm_from_ccd_pixel(FT_CCD_FIRST_PIXEL, scale_ppi);
  *last_mm = ft_get_mm_from_ccd_pixel(FT_CCD_FIRST_PIXEL + FT_CCD_PIXEL_COUNT,
                                      scale_ppi);
}

int64_t ft_get_scan_line(const ft_detail_scan* scan, double along_mm) {
  /* aligned line l is green row l + row spacing of the scan (Sancho's
   * write_crop) */
  int64_t row_spacing = FT_CCD_ROW_SPACING_LINES / scan->bin;
  return round_to_int64(along_mm * scan->ppi / MM_PER_INCH) -
         scan->first_line -
         row_spacing;
}

int64_t ft_get_scan_pixel(const ft_detail_scan* scan, double across_mm) {
  double ccd_pixel = ft_get_ccd_pixel_from_mm(across_mm, scan->ppi * scan->bin);
  return round_to_int64((ccd_pixel - FT_CCD_FIRST_PIXEL) / scan->bin) -
         scan->first_pixel;
}

int32_t ft_find_scan_ranges(const ft_detail_scan* scan, double along_start_mm,
                            double along_end_mm, double across_start_mm,
                            double across_end_mm, int32_t ranges[4]) {
  int64_t row_spacing = FT_CCD_ROW_SPACING_LINES / scan->bin;
  int64_t aligned_lines = scan->line_count - 2 * row_spacing;
  int64_t first_line = ft_get_scan_line(scan, along_start_mm);
  int64_t end_line = ft_get_scan_line(scan, along_end_mm);
  int64_t first_pixel = ft_get_scan_pixel(scan, across_start_mm);
  int64_t end_pixel = ft_get_scan_pixel(scan, across_end_mm);
  if(first_line < 0) first_line = 0;
  if(first_pixel < 0) first_pixel = 0;
  if(end_line > aligned_lines) end_line = aligned_lines;
  if(end_pixel > scan->pixel_count) end_pixel = scan->pixel_count;
  if(end_line <= first_line || end_pixel <= first_pixel) return -1;
  /* safe: all four are within the scan, far below 2^31 */
  ranges[0] = (int32_t)first_line;
  ranges[1] = (int32_t)end_line;
  ranges[2] = (int32_t)first_pixel;
  ranges[3] = (int32_t)end_pixel;
  return 0;
}

int32_t ft_get_thumbnail_factor(const ft_detail_scan* scan) {
  int64_t factor = round_to_int64(scan->ppi / FT_DETAIL_THUMBNAIL_PPI);
  return factor < 1 ? 1 : (int32_t)factor;
}

void ft_get_detail_axes(const ft_detail_scan* scan, int32_t factor,
                        ft_axes* axes) {
  double scale_ppi = scan->ppi * scan->bin;
  int32_t row_spacing = FT_CCD_ROW_SPACING_LINES / scan->bin;
  /* the centre of a box: factor x bin CCD pixels wide */
  double box_centre = (factor * scan->bin - 1) / 2.0;
  axes->mm_per_line = MM_PER_INCH / scan->ppi * factor;
  axes->first_line_mm = (scan->first_line + row_spacing + (factor - 1) / 2.0) *
                        MM_PER_INCH /
                        scan->ppi;
  double first_ccd_pixel =
    ft_get_ccd_pixel_from_output(scan->first_pixel, 0, scan->bin) + box_centre;
  double next_ccd_pixel =
    ft_get_ccd_pixel_from_output(scan->first_pixel, factor, scan->bin) +
    box_centre;
  axes->first_pixel_mm = ft_get_mm_from_ccd_pixel(first_ccd_pixel, scale_ppi);
  axes->mm_per_pixel =
    ft_get_mm_from_ccd_pixel(next_ccd_pixel, scale_ppi) - axes->first_pixel_mm;
}

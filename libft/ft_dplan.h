/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Detailed-scan planning (sub-project 2b). Pure: libm only, no I/O, no
 * allocation, so the browser preview can run it.
 *
 * A detailed scan (3150 or 6300 ppi, always bin 1: the most light) groups
 * the selected frames into passes: one scan window each, scanning through
 * any unselected frames between. A pass has one focus setting, and the
 * film is not flat along a strip (~1900 tandem steps of height variation
 * measured 2026-10-14, beyond the depth of focus at these
 * magnifications), so the frames of one pass must have focus offsets
 * within FT_DETAIL_FOCUS_SPAN_STEPS of each other. Among the groupings
 * that keep every frame in focus, an exact dynamic program picks the
 * fastest (ft_estimate_scan_seconds: every pass pays the overhead and the
 * calibration lines). */
#ifndef FT_DPLAN_H
#define FT_DPLAN_H

#include <stdint.h>

#include "ft.h"
#include "ft_detect.h"
#include "ft_focus.h"

/* mm kept around every frame along and across: creep between the preview
 * and the scan (~1 mm per run, 2026-10-13) */
#define FT_DETAIL_MARGIN_MM 3.0
/* tandem steps the focus offsets of one pass may span:
 * +-FT_FOCUS_MARGIN_STEPS (~5 % sharpness loss) around its middle.
 * ESTIMATE until measured at 3150 / 6300 optics. */
#define FT_DETAIL_FOCUS_SPAN_STEPS (2 * FT_FOCUS_MARGIN_STEPS)
/* detailed scans always read at bin 1 */
#define FT_DETAIL_BIN 1
/* the detection grid of ft_detect (the preview's) */
#define FT_DETAIL_THUMBNAIL_PPI 525.0

/* One selected frame, as the plan needs it. */
typedef struct ft_detail_frame {
  /* the frame's number in the job */
  uint32_t index;
  /* holder mm along the strip where the frame starts (the preview's box) */
  double start_mm;
  /* the frame's length along the strip, mm */
  double length_mm;
  /* the tandem focus offset measured for it */
  int64_t focus_offset_steps;
} ft_detail_frame;

/* One pass: a scan window over consecutive selected frames. */
typedef struct ft_detail_pass {
  /* the first and last frame it covers: frames[first_frame..last_frame] */
  uint32_t first_frame;
  uint32_t last_frame;
  /* the scan window along the strip, margins included */
  double start_mm;
  double length_mm;
  /* the middle of its frames' focus offsets */
  int64_t focus_offset_steps;
  /* the estimated scan time */
  double seconds;
} ft_detail_pass;

/* The fastest set of passes over the selected frames. */
typedef struct ft_detail_plan {
  /* passes in passes[] */
  uint32_t pass_count;
  /* the estimated time of all passes together */
  double seconds;
  /* the passes, in strip order */
  ft_detail_pass passes[FT_MAX_FRAMES];
} ft_detail_plan;

/* Plan the passes for the selected frames.
 *
 * frames              the selected frames in strip order (start_mm
 *                     ascending, not overlapping)
 * frame_count         1..FT_MAX_FRAMES
 * scan_length_mm      the holder's scan length
 * focus_span_steps    the widest focus spread one pass may have
 * plan                receives the passes
 *
 * Returns 0, or -1 on bad input or a ppi bin 1 cannot focus
 * (ft_is_bin_usable). */
int32_t ft_make_detail_plan(const ft_detail_frame* frames, uint32_t frame_count,
                            double ppi, double scan_length_mm,
                            int64_t focus_span_steps, ft_detail_plan* plan,
                            ft_err* error);

/* Is frame index in a list like "1,3-6,8" (job frame numbers, ascending
 * or not)? Returns 1 yes, 0 no, or -1 when the list is malformed. */
int32_t ft_is_frame_selected(const char* list, uint32_t index, ft_err* error);

/* The middle of low..high, rounded half away from zero: a pass's focus. */
int64_t ft_get_focus_middle(int64_t low, int64_t high);

/* A pass scan as sent to the scanner. The window descriptor carries
 * first_pixel at byte 14, first_line at 18, pixel_count at 22 and
 * line_count at 26. */
typedef struct ft_detail_scan {
  /* output pixels per inch, along and across */
  double ppi;
  /* CCD pixels per output pixel */
  int32_t bin;
  /* the first output pixel across */
  int64_t first_pixel;
  /* the first line along */
  int64_t first_line;
  /* output pixels per line */
  int64_t pixel_count;
  /* lines read, before the RGB alignment */
  int64_t line_count;
} ft_detail_scan;

/* What a crop over a window sends, with Sancho's crop rounding.
 *
 * across_start_mm, width_mm   the window across the strip, holder mm
 * along_start_mm, length_mm   the window along the strip, holder mm */
void ft_make_detail_scan(double ppi, int32_t bin, double across_start_mm,
                         double width_mm, double along_start_mm,
                         double length_mm, ft_detail_scan* scan);

/* The part of the holder across the strip, in mm, that the CCD covers at
 * the optics ppi x bin. */
void ft_get_ccd_reach_mm(double ppi, int32_t bin, double* first_mm,
                         double* last_mm);

/* Find a window (holder mm) in the developed pass, clipped to the scan
 * (line_count raw lines, less the RGB alignment's 2 x row spacing).
 *
 * ranges  receives aligned lines [ranges[0], ranges[1]) along and pixels
 *         [ranges[2], ranges[3]) across
 *
 * Returns 0, or -1 when no part of the window is in the scan. */
int32_t ft_find_scan_ranges(const ft_detail_scan* scan, double along_start_mm,
                            double along_end_mm, double across_start_mm,
                            double across_end_mm, int32_t ranges[4]);

/* The box factor from the scan to the detection grid, at least 1. */
int32_t ft_get_thumbnail_factor(const ft_detail_scan* scan);

/* The axes of the thumbnail: aligned lines and pixels box-averaged
 * factor x factor, leftovers dropped. Like ft_get_preview_axes for the
 * preview. */
void ft_get_detail_axes(const ft_detail_scan* scan, int32_t factor,
                        ft_axes* axes);

/* The aligned line (rounded) at along_mm, holder mm along the strip. */
int64_t ft_get_scan_line(const ft_detail_scan* scan, double along_mm);

/* The pixel (rounded) at across_mm, holder mm across the strip. */
int64_t ft_get_scan_pixel(const ft_detail_scan* scan, double across_mm);

#endif

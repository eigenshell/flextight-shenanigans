/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Scan to share: plan the scans for small (~2000 px) shareable frames.
 *
 * A share scan uses the lowest magnification that still gives the target
 * size on the frame's long side: less magnification puts more light on
 * every sample (ft_plan.h), so Dmax is higher and noise lower than a
 * detail scan resampled down. The stock firmware scans one window per
 * scan and calibrates (holder out and back) every time, so the selected
 * frames are grouped into windows: a run of frames in one window scans
 * through any unselected frame between them, a split pays another
 * calibration. ft_estimate_share prices one {ppi, bin} with the best
 * grouping (exact, dynamic programming); ft_find_share_candidates prices every
 * usable bin; a policy picks one (now: most light). Pure: no I/O, no
 * allocation (the planner runs in the browser too). */
#ifndef FT_SHARE_H
#define FT_SHARE_H

#include <stdbool.h>
#include <stdint.h>

#include "ft.h"
#include "ft_focus.h"
#include "ft_job.h"

/* the frame's long side, pixels */
#define FT_SHARE_TARGET_PIXELS 2000.0
/* along, at each window end: room for creep */
#define FT_SHARE_MARGIN_MM 2.0
/* kept around each cut frame */
#define FT_SHARE_BLEED_MM 0.5
/* a full frame's long side: half frames at the full frame's ppi count
 * as this long */
#define FT_SHARE_FULL_FRAME_LONG_MM 36.0
/* Focus: a share job (focus = per-frame) gets one focus bracket in scan
 * --job, at its first window's optics, before that scan: ~
 * FT_SHARE_FOCUS_CROPS crops of ~FT_FOCUS_SAMPLE_SECONDS each (more when
 * the peak sits at an edge). Later windows carry its offset on the curve
 * (a tandem offset moves the film-side focus plane, the same at any
 * scale): no crops, but a 3-crop check at a scale above
 * FT_SHARE_FOCUS_RECHECK x the measured one (not priced). focus =
 * shifted: the curve and the strip's offset as they are, no bracket. */
#define FT_SHARE_FOCUS_CROPS 5
#define FT_SHARE_FOCUS_RECHECK 1.25
#define FT_FOCUS_SAMPLE_SECONDS 12.0

/* One selected frame, as the planner needs it. */
typedef struct ft_share_frame {
  /* the job's frame number */
  uint32_t index;
  ft_frame_type type;
  /* the preview's box, holder mm */
  double along_start_mm;
  double length_mm;
  double across_start_mm;
  double width_mm;
} ft_share_frame;

/* What to plan for. */
typedef struct ft_share_options {
  /* the long side, pixels; FT_SHARE_TARGET_PIXELS */
  double target_pixels;
  /* half frames at the full frame's ppi */
  bool half_frames_at_full_frame_ppi;
  /* FT_SHARE_MARGIN_MM */
  double margin_mm;
  /* the holder: windows stay in 0..scan_length_mm */
  double scan_length_mm;
} ft_share_options;

/* One scan window over consecutive selected frames. */
typedef struct ft_share_window {
  /* along, holder mm, margins included */
  double start_mm;
  double length_mm;
  /* frames[first_frame..last_frame] (selected, in order) */
  uint32_t first_frame;
  uint32_t last_frame;
} ft_share_window;

/* The price of one {ppi, bin}, with its windows. */
typedef struct ft_share_estimate {
  double ppi;
  int32_t bin;
  /* ppi x bin: the optics */
  double scale_ppi;
  /* scan_seconds + focus_seconds */
  double seconds;
  double scan_seconds;
  double focus_seconds;
  /* ft_estimate_relative_light and ft_estimate_dmax_for_light */
  double relative_light;
  double dmax;
  uint32_t window_count;
  ft_share_window windows[FT_MAX_FRAMES];
} ft_share_estimate;

/* The default options for a holder of scan_length_mm. */
void ft_init_share_options(ft_share_options* options, double scan_length_mm);

/* The long side the target is pinned to: max(length, width); half frames
 * at the full frame's ppi count as FT_SHARE_FULL_FRAME_LONG_MM. */
double ft_get_share_long_side_mm(const ft_share_frame* frame,
                                 bool half_frames_at_full_frame_ppi);

/* The pixels wanted on the frame's own long side: the target, or for a
 * half frame at the full frame's ppi, target x long /
 * FT_SHARE_FULL_FRAME_LONG_MM. */
double ft_get_share_target_pixels(const ft_share_frame* frame,
                                  const ft_share_options* options);

/* The output ppi the frames need (the largest over them), rounded up. */
double ft_get_share_ppi_needed(const ft_share_frame* frames,
                               uint32_t frame_count,
                               const ft_share_options* options);

/* Price {ppi, bin} with the cheapest grouping into windows.
 *
 * frames       the selected ones, along_start_mm ascending, at least 1
 *
 * Returns 0, or -1 with error filled when ppi x bin is out of focus
 * range, a frame is outside the holder, or the frames overlap or are out
 * of order. */
int32_t ft_estimate_share(const ft_share_frame* frames, uint32_t frame_count,
                          const ft_share_options* options, double ppi,
                          int32_t bin, const ft_lens* lens,
                          ft_share_estimate* estimate, ft_err* error);

/* Price every usable bin at the needed ppi (each bin's ppi is the needed
 * ppi, or higher where FT_SCALE_MIN_PPI demands it), largest bin
 * (fastest) first. Returns how many (1..6), or -1 with error filled. */
int32_t ft_find_share_candidates(const ft_share_frame* frames,
                                 uint32_t frame_count,
                                 const ft_share_options* options,
                                 const ft_lens* lens,
                                 ft_share_estimate candidates[6],
                                 ft_err* error);

/* The policy "most light": the highest Dmax, ties to the fastest.
 * Returns the chosen candidate's index. */
uint32_t ft_pick_most_light(const ft_share_estimate* candidates,
                            uint32_t candidate_count);

/* The frames of a job to share, as planner input, in strip order.
 *
 * list  "1,3-6,8-12" (job frame numbers); NULL or "" = every frame that
 *       is not partial
 *
 * Returns 0, or -1 with error filled on a malformed list, a number the
 * job lacks, or no frame at all. */
int32_t ft_select_share_frames(const ft_job* job, const char* list,
                               ft_share_frame* frames, uint32_t* frame_count,
                               ft_err* error);

/* Write a plan into the job: share and half frames from the options; the
 * selected frames included with the estimate's ppi and bin, group = their
 * window (1..); every other frame excluded (group 0). */
void ft_apply_share_plan(ft_job* job, const ft_share_frame* selected,
                         uint32_t selected_count,
                         const ft_share_options* options,
                         const ft_share_estimate* estimate);

/* Quixote's sizes: pixels on the frame's long side (3:2 at ~4 / 6 MP) */
#define FT_SHARE_4MP_PIXELS 2450.0
#define FT_SHARE_6MP_PIXELS 3000.0

/* Plan sizes per frame.
 *
 * target_pixels  per job frame (target_pixels[i] for job->frames[i]);
 *                0 = not shared
 *
 * The chosen frames, in strip order, split into runs of one size; each
 * run is priced (ft_find_share_candidates) and keeps the most light
 * (ft_pick_most_light); runs never share a window. Writes the plan into
 * job: share = the smallest size, share_pixels on frames of another
 * size, groups numbered along the strip, every other frame excluded,
 * focus measured on the first window (per-frame, offset 0: the first
 * bracket's centre).
 *
 * Returns the number of groups, or -1 with error filled. */
int32_t ft_plan_share_sizes(ft_job* job, const double* target_pixels,
                            bool half_frames_at_full_frame_ppi,
                            const ft_lens* lens, double scan_length_mm,
                            ft_err* error);

/* Group group's scan window along, as the planner priced it: from the
 * first included frame's start - margin to the last's end + margin,
 * clipped to 0..scan_length_mm. Returns 0, or -1 when no included frame
 * is in the group. */
int32_t ft_get_share_group_window(const ft_job* job, int32_t group,
                                  const ft_share_options* options,
                                  double* start_mm, double* length_mm);

#endif

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
 * calibration. ft_share_estimate prices one {ppi, bin} with the best
 * grouping (exact, dynamic programming); ft_share_candidates prices every
 * usable bin; a policy picks one (now: most light). Pure: no I/O, no
 * allocation (the planner can run in the browser too). */
#ifndef FT_SHARE_H
#define FT_SHARE_H
#include "ft.h"
#include "ft_focus.h"
#include "ft_job.h"

#define FT_SHARE_TARGET_PX 2000.0 /* long side of the frame, px */
#define FT_SHARE_MARGIN_MM 2.0    /* along, at each window end: creep */
#define FT_SHARE_BLEED_MM 0.5     /* kept around each cut frame */
#define FT_SHARE_FF_LONG_MM 36.0  /* half-res half frames get the FF ppi */
/* Focus: a share job (focus = per-frame) gets one focus bracket in scan
 * --job, at its first window's optics, before that scan: ~
 * FT_SHARE_FOCUS_CROPS crops of ~FT_FOCUS_SAMPLE_S each (more when the
 * peak sits at an edge). Later windows carry its offset on the curve (a
 * tandem offset moves the film-side focus plane, the same at any
 * scale): no crops, but a 3-crop check at a scale above
 * FT_SHARE_FOCUS_RECHECK x the measured one (not priced). focus =
 * shifted: the curve and the strip's offset as they are, no bracket. */
#define FT_SHARE_FOCUS_CROPS 5
#define FT_SHARE_FOCUS_RECHECK 1.25
#define FT_FOCUS_SAMPLE_S 12.0

typedef struct {
  int index; /* job frame number */
  ft_frame_type type;
  double y0, len;   /* along, holder mm (preview box) */
  double x0, width; /* across, holder mm */
} ft_share_frame;

typedef struct {
  double target_px;   /* long side; FT_SHARE_TARGET_PX */
  int hf_half_res;    /* half frames at the full frame's ppi */
  double margin_mm;   /* FT_SHARE_MARGIN_MM */
  double scan_len_mm; /* holder: windows stay in 0..scan_len_mm */
} ft_share_opts;

typedef struct {
  double y0, len;  /* along, holder mm, margins included */
  int first, last; /* frames[first..last] (selected, in order) */
} ft_share_window;

typedef struct {
  double ppi;
  int bin;
  double scale;   /* ppi x bin: the optics */
  double seconds; /* scan_s + focus_s */
  double scan_s, focus_s;
  double light, dmax; /* ft_light_rel, ft_dmax_est */
  int nwin;
  ft_share_window win[FT_MAX_FRAMES];
} ft_share_est;

void ft_share_opts_default(ft_share_opts* o, double scan_len_mm);
/* the long side the target is pinned to: max(len, width); half-res half
 * frames count as FT_SHARE_FF_LONG_MM (the full frame's ppi) */
double ft_share_long_mm(const ft_share_frame* f, int hf_half_res);
/* px wanted on the frame's own long side: target, or for a half-res half
 * frame target x long / FT_SHARE_FF_LONG_MM (the same ppi as a FF) */
double ft_share_target_px(const ft_share_frame* f, const ft_share_opts* o);
/* output ppi the frames need (the largest over them), rounded up */
double ft_share_ppi_needed(const ft_share_frame* f, int n,
                           const ft_share_opts* o);
/* frames: the selected ones, sorted along (y0 ascending), n >= 1.
 * Prices {ppi, bin} with the cheapest grouping into windows. -1 with e
 * filled if ppi x bin is out of focus range or a frame is outside the
 * holder or the frames overlap / are out of order. */
int ft_share_estimate(const ft_share_frame* f, int n, const ft_share_opts* o,
                      double ppi, int bin, const ft_lens* lens,
                      ft_share_est* out, ft_err* e);
/* every usable bin at the needed ppi (each bin's ppi is the needed ppi,
 * or higher where FT_SCALE_MIN demands it), largest bin (fastest) first;
 * returns how many (1..6), -1 with e filled on bad input */
int ft_share_candidates(const ft_share_frame* f, int n, const ft_share_opts* o,
                        const ft_lens* lens, ft_share_est c[6], ft_err* e);
/* policy "most light": the highest Dmax, ties to the fastest */
int ft_share_pick_max_dmax(const ft_share_est* c, int n);

/* Jobs (ft_job.h). The frames to share, as planner input: list is
 * "1,3-6,8-12" (job frame numbers); NULL or "" = every frame that is not
 * partial. Strip order. -1 with e filled for a malformed list, a number
 * the job lacks or no frame at all. */
int ft_share_select(const ft_job* j, const char* list, ft_share_frame* out,
                    int* n, ft_err* e);
/* writes a plan into the job: share and hf_half_res from o; the selected
 * frames included with est's ppi and bin, group = their window (1..);
 * every other frame excluded (group 0) */
void ft_share_apply(ft_job* j, const ft_share_frame* sel, int n,
                    const ft_share_opts* o, const ft_share_est* est);
/* The web page's sizes: px on the frame's long side (3:2 at ~4 / 6 MP) */
#define FT_SHARE_4MP 2450.0
#define FT_SHARE_6MP 3000.0
/* sizes per frame: target_px[i] for j->f[i], 0 = not shared. The chosen
 * frames, in strip order, split into runs of one size; each run is priced
 * (ft_share_candidates) and keeps the most light (ft_share_pick_max_dmax);
 * runs never share a window. Writes the plan into j: share = the smallest
 * size, share_px on frames of another size, groups numbered along the
 * strip, every other frame excluded, focus measured on the first window
 * (per-frame, offset 0: the first bracket's centre). Returns the number
 * of groups; -1 with e filled. */
int ft_share_plan_sizes(ft_job* j, const double* target_px, int hf_half_res,
                        const ft_lens* lens, double scan_len_mm, ft_err* e);
/* group g's scan window along, as the planner priced it: from the first
 * included frame's y0 - margin to the last's end + margin, clipped to
 * 0..scan_len_mm. -1 if no included frame is in group g. */
int ft_share_group_window(const ft_job* j, int g, const ft_share_opts* o,
                          double* y0, double* len);
#endif

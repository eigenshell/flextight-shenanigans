/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Detailed-scan planning (sub-project 2b). Pure: libm only, no I/O, no
 * allocation (the browser preview can run it).
 *
 * A detailed scan (3150 or 6300 ppi, always bin 1: the most light) groups
 * the selected frames into passes: one scan window each, scanning through
 * any unselected frames between. A pass has one focus setting, and the
 * film is not flat along a strip (~1900 tandem steps of height variation
 * measured 2026-10-14, beyond the depth of focus at these
 * magnifications), so the frames of one pass must have focus offsets
 * within FT_DETAIL_DOF_SPAN of each other. Among the groupings that keep
 * every frame in focus, an exact DP picks the fastest (ft_scan_seconds:
 * every pass pays the fixed part and the calibration lines). */
#ifndef FT_DPLAN_H
#define FT_DPLAN_H
#include "ft.h"
#include "ft_detect.h"
#include "ft_focus.h"

/* mm kept around every frame along and across: creep between the preview
 * and the scan (~1 mm per run, 2026-10-13) */
#define FT_DETAIL_MARGIN_MM 3.0
/* tandem steps the focus offsets of one pass may span: +-FT_FOCUS_MARGIN
 * (~5 % sharpness loss) around its middle. ESTIMATE until measured at
 * 3150 / 6300 optics. */
#define FT_DETAIL_DOF_SPAN (2 * FT_FOCUS_MARGIN)
#define FT_DETAIL_BIN 1
/* the detection grid of ft_detect (the preview's) */
#define FT_DETAIL_THUMB_PPI 525.0

typedef struct {
  int index;      /* job frame number */
  double y0, len; /* along, holder mm (the preview's box) */
  long focus;     /* tandem focus offset measured for it */
} ft_dframe;

typedef struct {
  int first, last; /* frames[first..last] */
  double y0, len;  /* scan window along, margins included */
  long focus;      /* the middle of its frames' offsets */
  double seconds;
} ft_dpass;

typedef struct {
  int npass;
  double seconds;
  ft_dpass p[FT_MAX_FRAMES];
} ft_dplan;

/* frames: the selected ones in strip order (y0 ascending, not
 * overlapping), 1..FT_MAX_FRAMES. -1 with e filled on bad input or a ppi
 * bin 1 cannot focus (ft_bin_ok). */
int ft_dplan_make(
    const ft_dframe *f, int n, double ppi, double scan_len_mm, long span,
    ft_dplan *out, ft_err *e
);

/* index in a frame list "1,3-6,8" (job frame numbers, ascending or not)?
 * 1 yes, 0 no, -1 malformed (with e filled) */
int ft_dplan_selected(const char *list, int index, ft_err *e);
/* the middle of lo..hi, rounded half away from zero (a pass's focus) */
long ft_dplan_mid(long lo, long hi);

/* A pass scan as sent: descriptor @14 first output px, @18 first line,
 * @22 px per line, @26 lines, at ppi and bin. */
typedef struct {
  double ppi;
  int bin;
  long x_out, y_line, px, lines;
} ft_dscan;

/* what a synthesized crop over the window sends (ftpoc's crop_scan
 * rounding): x0/w across, y0/len along, holder mm */
void ft_dscan_for(
    double ppi, int bin, double x0, double w, double y0, double len, ft_dscan *s
);
/* across, holder mm, the CCD covers at optics ppi x bin */
void ft_dscan_reach(double ppi, int bin, double *lo, double *hi);
/* a window (holder mm) in the developed pass: aligned lines [r[0], r[1])
 * along, pixels [r[2], r[3]) across, clipped to the scan (`lines` raw
 * lines less 2k by the alignment). -1 if nothing of it is in the scan. */
int ft_dscan_ranges(
    const ft_dscan *s, double y0, double y1, double x0, double x1, int r[4]
);
/* box factor from the scan to the detection grid, >= 1 */
int ft_dscan_thumb(const ft_dscan *s);
/* axes of the thumbnail (aligned lines and pixels box-averaged f x f,
 * leftovers dropped), as ft_preview_axes for the preview */
void ft_dscan_axes(const ft_dscan *s, int f, ft_axes *ax);
/* holder mm along -> aligned line (rounded), and across -> pixel */
long ft_dscan_line(const ft_dscan *s, double y_mm);
long ft_dscan_px(const ft_dscan *s, double x_mm);

#endif

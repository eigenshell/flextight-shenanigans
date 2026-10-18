/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Whole-holder previews: fast, for framing and detection. "standard" is
 * FlexColor's captured 525 ppi preview (6300 optics, bin 12) with its
 * CCD/lens on the focus curve (ft_preview_std_desc). "o3150" is
 * 525 ppi at the 3150 optics (bin 6): a detailed scan's preview is taken
 * at the scan's own optics, so its light level and Dmax match the scan
 * (6300 scans preview with "standard"). "1000" and
 * "1500" are synthesized at the 6000 optics FlexColor's crops use (bin 6
 * and 4), the fastest way to those resolutions: the stock firmware costs
 * ~84k output px/s for the full line (7992 / bin px) whatever the window,
 * so a larger bin is faster. Lower-magnification (more light, slower)
 * optics are a per-frame choice for the real scans (ft_plan.h).
 * All cover the standard preview's window. Frame detection runs on a copy
 * box-averaged by `down` to ~500 ppi, near the 525 ppi it is tuned on. */
#ifndef FT_PREVIEW_H
#define FT_PREVIEW_H
#include "ft_detect.h"
#include "ft_focus.h"

#define FT_PREVIEW_LINES_525 4795L  /* holder length at 525 ppi (~232 mm) */
#define FT_PREVIEW_X_OUT     2      /* standard preview: first output px */
#define FT_PREVIEW_PX        666    /* standard preview: px per line */

typedef struct {
    const char *name;
    double ppi;        /* output pitch along and across */
    int bin;           /* CCD pixels per output pixel across (one is kept) */
    int down;          /* box factor to the detection grid */
} ft_preview;

/* "standard" (or "525"), "o3150", "1000", "1500"; NULL: unknown */
const ft_preview *ft_preview_find(const char *name);
/* window in holder mm: across x0..x0+w (the standard preview's span),
 * along 0..len */
void ft_preview_window(double *x0, double *w, double *len);
long ft_preview_lines(const ft_preview *m);
/* tri-linear row offset in lines at the scan's resolution (12 / bin) */
int ft_preview_rgb_k(const ft_preview *m);
/* axes of the detection image of a scan whose first output pixel is
 * x_out: box-averaged by m->down, then RGB-aligned (k / down lines) */
void ft_preview_axes(const ft_preview *m, long x_out, ft_axes *ax);
/* The standard preview's SetWindow descriptor: tmpl (FlexColor's captured
 * 525 ppi window, FT_DESC_LEN bytes) copied to out with only the CCD and
 * lens positions (BE u32 @FT_DESC_CCD / @FT_DESC_OPTIC) moved to the
 * focus curve at the 6300 optics, offset 0 (ft_optics_for, as crops and
 * the synthesized previews): FlexColor's own pair sits ~10000 CCD steps
 * short of the focus peak. *ccd / *optic: the pair. 0 ok; -1 no focus at
 * 6300 (out untouched). The caller checks the pair (ft_positions_ok). */
#define FT_DESC_LEN   84
#define FT_DESC_CCD   56
#define FT_DESC_OPTIC 60
int ft_preview_std_desc(const unsigned char *tmpl, const ft_lens *l,
                        unsigned char *out, long *ccd, long *optic);
/* box average f x f: in lines x px x 3 -> out (lines/f) x (px/f) x 3;
 * leftover lines and columns are dropped */
void ft_box_down(const float *in, int lines, int px, int f, float *out);

#endif

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Optics options for a scan at a given ppi, with estimates.
 *
 * Any bin dividing 12 whose optical scale ppi x bin is in focus range
 * (FT_SCALE_MIN..FT_SCALE_MAX) gives the same output ppi. A larger bin
 * is faster: the stock firmware costs ~FT_OUT_PX_RATE output px/s for the
 * full line (FT_CCD_PIXEL_COUNT / bin px) whatever the window. A smaller bin is
 * a lower magnification m and puts ~(1+m)^-2 more light on every sample
 * (and keeps more of the CCD's pixels: the firmware keeps one per bin).
 * Measured 2026-10-13: line rate x 7992/bin = 84k px/s at bin 12, 2, 1;
 * per-read Dmax (R) ~3.0 at 6300 optics, 3.60 at 2000, 3.58 at 1500
 * (white near the ADC top there: ~3.73 is the per-read ceiling). */
#ifndef FT_PLAN_H
#define FT_PLAN_H
#include "ft_focus.h"

#define FT_OUT_PX_RATE 84000.0 /* output px/s over the full line */
#define FT_SCAN_FIXED 10.0     /* s per scan: hold, settling, moves */
#define FT_CAL_LINES 250       /* calibration lines, same line rate */
#define FT_DMAX_6300 3.0       /* per-read Dmax (R) at 6300 optics */
#define FT_DMAX_CEIL 3.73      /* log10((16383 - 310) / 3 DN) */

/* bins usable at ppi, largest (fastest) first; returns how many (0..6) */
int ft_bins_for_ppi(double ppi, int bins[6]);
/* 1 if bin is one of ft_bins_for_ppi's */
int ft_bin_ok(double ppi, int bin);
/* light per sample relative to the 6300 optics */
double ft_light_rel(const ft_lens* l, double scale);
/* per-read Dmax (R) estimate for that light */
double ft_dmax_est(double light);
/* seconds to read `lines` lines at bin (measured 2026-10-13: 9133 at
 * bin 2 433.5 s, 13700 at bin 1 1300.7 s, 1409 at bin 1 134.0 s) */
double ft_line_seconds(double lines, int bin);
/* seconds for a whole scan len_mm along the strip: fixed part, the
 * calibration lines and the scan lines (250 cal lines took 24.2 s at
 * bin 1, ~7.8 s at bin 3) */
double ft_scan_seconds(double ppi, int bin, double len_mm);

#endif

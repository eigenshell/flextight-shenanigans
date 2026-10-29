/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Optics options for a scan at a given ppi, with estimates.
 *
 * Any bin dividing 12 whose optical scale ppi x bin is in focus range
 * (FT_SCALE_MIN_PPI..FT_SCALE_MAX_PPI) gives the same output ppi. A larger bin
 * is faster: the stock firmware costs ~FT_OUTPUT_PIXELS_PER_SECOND output
 * pixels per second for the
 * full line (FT_CCD_PIXEL_COUNT / bin pixels) whatever the window. A smaller
 * bin is a lower magnification m and puts ~(1+m)^-2 more light on every sample
 * (and keeps more of the CCD's pixels: the firmware keeps one per bin).
 * Measured 2026-10-13: line rate x 7992/bin = 84k px/s at bin 12, 2, 1;
 * per-read Dmax (R) ~3.0 at 6300 optics, 3.60 at 2000, 3.58 at 1500
 * (white near the ADC top there: ~3.73 is the per-read ceiling). */
#ifndef FT_PLAN_H
#define FT_PLAN_H

#include <stdbool.h>
#include <stdint.h>

#include "ft_focus.h"

/* output pixels the firmware delivers per second over the full line */
#define FT_OUTPUT_PIXELS_PER_SECOND 84000.0
/* seconds every scan costs on top of its lines: hold, settling, moves */
#define FT_SCAN_OVERHEAD_SECONDS 10.0
/* calibration lines before each scan, read at the same line rate */
#define FT_CALIBRATION_LINES 250
/* per-read Dmax (R) at the 6300 optics */
#define FT_DMAX_AT_6300 3.0
/* the per-read Dmax ceiling: log10((16383 - 310) / 3 DN) */
#define FT_DMAX_CEILING 3.73

/* List the bins usable at ppi: those whose scale ppi x bin lies within
 * FT_SCALE_MIN_PPI..FT_SCALE_MAX_PPI.
 *
 * bins  receives the bins, largest (fastest) first
 *
 * Returns how many, 0..6. */
uint32_t ft_find_bins_for_ppi(double ppi, uint32_t bins[6]);

/* Is bin one of ft_find_bins_for_ppi's bins for ppi? */
bool ft_is_bin_usable(double ppi, uint32_t bin);

/* The light per sample at scale_ppi, relative to the 6300 optics. */
double ft_estimate_relative_light(const ft_lens* lens, double scale_ppi);

/* The per-read Dmax (R) for a relative light (see
 * ft_estimate_relative_light), at most FT_DMAX_CEILING. */
double ft_estimate_dmax_for_light(double relative_light);

/* Seconds to read line_count lines at bin. Measured 2026-10-13: 9133
 * lines at bin 2 took 433.5 s, 13700 at bin 1 1300.7 s, 1409 at bin 1
 * 134.0 s. */
double ft_estimate_line_seconds(double line_count, uint32_t bin);

/* Seconds for a whole scan length_mm along the strip: the overhead, the
 * calibration lines and the scan lines. 250 calibration lines took
 * 24.2 s at bin 1 and ~7.8 s at bin 3. */
double ft_estimate_scan_seconds(double ppi, uint32_t bin, double length_mm);

#endif

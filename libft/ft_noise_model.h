/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The noise profile of a developed frame from the scan's own .cal:
 * var_c(T) = S_c * T + O_c per channel, in developed transmittance units
 * (T = (raw - dark) / (white - dark), as ft_flatfield).
 *
 * The CCD: raw DN = dark + signal, var = g * signal + read^2 (shot noise on
 * the signal above the black level, which is an electronic offset).
 * read^2 is the variance of the dark lines about each pixel's own mean
 * (fixed pattern is not noise) and about each line's own offset (lamp
 * drift and flicker move a whole line: banding, not pixel noise); g the
 * slope of the white-line variance over the signal. Lines are classified
 * as ft_split_cal does (FT_CAL_BAND): the lamp's on/off transition lines
 * are neither dark nor white. Dividing by the span gives S = g / span, and
 * O = read^2 (1 + 1/n_dark) / span^2, the dark reference being the mean of
 * n_dark lines; both averaged over the pixels. ft_align_rgb copies whole
 * rows, so the realignment leaves the variance as it is (a fractional
 * aligner would multiply it by w^2 + (1 - w)^2).
 *
 * Pure C99 (no I/O, no allocation). */
#ifndef FT_NOISE_MODEL_H
#define FT_NOISE_MODEL_H
#include "ft.h"

#define FT_NOISE_MIN_DARK 50
#define FT_NOISE_MIN_WHITE 20
#define FT_NOISE_MAX_LINES 1024 /* a .cal has 250 */

/* cal: lines x px x 3. prof = S_r O_r S_g O_g S_b O_b.
 * 0: ok. 1: ok, but a value was clamped (S < 0 or non-finite -> 0, O below
 * 0.25 DN^2 -> that floor); e says which. -1: too few dark or white lines
 * (or a degenerate span), no profile; e says why. */
int ft_noise_model(const unsigned short* cal, int lines, int px, double prof[6],
                   ft_err* e);

#endif

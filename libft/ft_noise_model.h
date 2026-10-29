/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The noise profile of a developed frame from the scan's own .cal:
 * variance_c(T) = S_c * T + O_c per channel, in developed transmittance units
 * (T = (raw - dark) / (white - dark), as ft_apply_flat_field).
 *
 * The CCD: raw DN = dark + signal, var = g * signal + read^2 (shot noise on
 * the signal above the black level, which is an electronic offset).
 * read^2 is the variance of the dark lines about each pixel's own mean
 * (fixed pattern is not noise) and about each line's own offset (lamp
 * drift and flicker move a whole line: banding, not pixel noise); g the
 * slope of the white-line variance over the signal. Lines are classified
 * as ft_split_calibration does (FT_CALIBRATION_BAND): the lamp's on/off
 * transition lines are neither dark nor white. Dividing by the span gives S = g
 * / span, and O = read^2 (1 + 1/n_dark) / span^2, the dark reference being the
 * mean of n_dark lines; both averaged over the pixels. ft_align_rgb_rows copies
 * whole rows, so the realignment leaves the variance as it is (a fractional
 * aligner would multiply it by w^2 + (1 - w)^2).
 *
 * Pure C99 (no I/O, no allocation). */
#ifndef FT_NOISE_MODEL_H
#define FT_NOISE_MODEL_H

#include <stdint.h>

#include "ft.h"

/* the fewest dark and white calibration lines a profile needs */
#define FT_NOISE_MIN_DARK 50
#define FT_NOISE_MIN_WHITE 20
/* the most calibration lines read (a .cal has 250) */
#define FT_NOISE_MAX_LINES 1024

/* What ft_measure_noise_profile found. The values match its old int
 * results. */
typedef enum ft_noise_result {
  /* a profile, as measured */
  FT_NOISE_OK = 0,
  /* a profile, but a value was clamped (S < 0 or not finite -> 0, O
   * below 0.25 DN^2 -> that floor); the error says which */
  FT_NOISE_CLAMPED = 1,
  /* no profile: too few dark or white lines, or a degenerate span; the
   * error says why */
  FT_NOISE_NONE = -1
} ft_noise_result;

/* Measure the noise profile from the calibration lines.
 *
 * calibration  lines x pixels x 3 raw values
 * profile      receives S_r O_r S_g O_g S_b O_b */
ft_noise_result ft_measure_noise_profile(const uint16_t* calibration,
                                         uint32_t lines, uint32_t pixels,
                                         double profile[6], ft_err* error);

#endif

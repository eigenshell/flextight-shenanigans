/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_plan.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ft_geom.h"

#define MM_PER_INCH 25.4
/* slack when comparing a scale with its limits */
#define SCALE_TOLERANCE 1e-6
/* the lowest ppi a scan can have: 6300 / 12 */
#define MIN_PPI 525
/* the optics the light and Dmax model is measured at */
#define REFERENCE_SCALE_PPI 6300.0

uint32_t ft_find_bins_for_ppi(double ppi, uint32_t bins[6]) {
  static const uint32_t ALL_BINS[] = {12, 6, 4, 3, 2, 1};
  /* written as "not >=" so that NaN is refused too */
  if(isfinite(ppi) == false || !(ppi >= MIN_PPI)) return 0;
  uint32_t count = 0;
  for(size_t index = 0; index < sizeof ALL_BINS / sizeof ALL_BINS[0]; index++) {
    double scale_ppi = ppi * ALL_BINS[index];
    if(scale_ppi >= FT_SCALE_MIN_PPI - SCALE_TOLERANCE &&
       scale_ppi <= FT_SCALE_MAX_PPI + SCALE_TOLERANCE) {
      bins[count] = ALL_BINS[index];
      count++;
    }
  }
  return count;
}

bool ft_is_bin_usable(double ppi, uint32_t bin) {
  uint32_t bins[6];
  uint32_t count = ft_find_bins_for_ppi(ppi, bins);
  for(uint32_t index = 0; index < count; index++) {
    if(bins[index] == bin) return true;
  }
  return false;
}

double ft_estimate_relative_light(const ft_lens* lens, double scale_ppi) {
  /* light per sample goes as (1 + m)^-2, m = pixel pitch x scale */
  double ratio = (1 + lens->pixel_pitch_inches * REFERENCE_SCALE_PPI) /
                 (1 + lens->pixel_pitch_inches * scale_ppi);
  return ratio * ratio;
}

double ft_estimate_dmax_for_light(double relative_light) {
  double dmax = FT_DMAX_AT_6300 + log10(relative_light);
  return dmax < FT_DMAX_CEILING ? dmax : FT_DMAX_CEILING;
}

double ft_estimate_line_seconds(double line_count, uint32_t bin) {
  /* the firmware sends the full line, whatever the window */
  double pixels_per_line = FT_CCD_PIXEL_COUNT / (double)bin;
  return line_count * pixels_per_line / FT_OUTPUT_PIXELS_PER_SECOND;
}

double ft_estimate_scan_seconds(double ppi, uint32_t bin, double length_mm) {
  double scan_lines = length_mm / MM_PER_INCH * ppi;
  return FT_SCAN_OVERHEAD_SECONDS +
         ft_estimate_line_seconds(scan_lines + FT_CALIBRATION_LINES, bin);
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_plan.h"

#include <math.h>

#include "ft_geom.h"

int ft_bins_for_ppi(double ppi, int bins[6]) {
  static const int all[] = {12, 6, 4, 3, 2, 1};
  int i, n = 0;
  if(!isfinite(ppi) || !(ppi >= 525)) return 0;
  for(i = 0; i < 6; i++) {
    double scale = ppi * all[i];
    if(scale >= FT_SCALE_MIN - 1e-6 && scale <= FT_SCALE_MAX + 1e-6)
      bins[n++] = all[i];
  }
  return n;
}

int ft_bin_ok(double ppi, int bin) {
  int bins[6], n = ft_bins_for_ppi(ppi, bins), i;
  for(i = 0; i < n; i++)
    if(bins[i] == bin) return 1;
  return 0;
}

double ft_light_rel(const ft_lens* l, double scale) {
  double r = (1 + l->p * 6300.0) / (1 + l->p * scale);
  return r * r;
}

double ft_dmax_est(double light) {
  double d = FT_DMAX_6300 + log10(light);
  return d < FT_DMAX_CEIL ? d : FT_DMAX_CEIL;
}

double ft_line_seconds(double lines, int bin) {
  return lines * (FT_CCD_PIXELS / (double)bin) / FT_OUT_PX_RATE;
}

double ft_scan_seconds(double ppi, int bin, double len_mm) {
  return FT_SCAN_FIXED +
         ft_line_seconds(len_mm / 25.4 * ppi + FT_CAL_LINES, bin);
}

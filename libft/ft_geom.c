/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_geom.h"

/* Below the measured range the focus curve's optics land a little off the
 * nominal scale and axis. Hardware 2026-10-13: each scale scanned back to
 * back with the standard preview (the film moves between longer runs),
 * five edges at the strip start, residuals <= 6 um. At 3150 and above the
 * 2026-10-12 model holds as measured. Linear in scale between the points,
 * held flat below 1500. */
static const struct {
  double scale, ratio, axis;
} EFF[] = {
    {1500.0, 1491.5 / 1500.0, 4132.9},
    {2000.0, 1992.4 / 2000.0, 4140.5},
    {3150.0, 1.0, FT_CCD_AXIS},
};
#define NEFF (sizeof EFF / sizeof EFF[0])

static void eff(double scale, double *s, double *axis) {
  unsigned i;
  double w;
  if(!(scale < EFF[NEFF - 1].scale)) {
    *s = scale;
    *axis = FT_CCD_AXIS;
    return;
  }
  if(!(scale > EFF[0].scale)) {
    *s = scale * EFF[0].ratio;
    *axis = EFF[0].axis;
    return;
  }
  for(i = 1; scale > EFF[i].scale; i++)
    ;
  w = (scale - EFF[i - 1].scale) / (EFF[i].scale - EFF[i - 1].scale);
  *s = scale * (EFF[i - 1].ratio + w * (EFF[i].ratio - EFF[i - 1].ratio));
  *axis = EFF[i - 1].axis + w * (EFF[i].axis - EFF[i - 1].axis);
}

double ft_mm_to_ccd(double mm, double scale) {
  double s, a;
  eff(scale, &s, &a);
  return a + (mm / 25.4 * 6000.0 + FT_CCD_FIRST - a) * s / 6000.0;
}

double ft_ccd_to_mm(double ccd, double scale) {
  double s, a;
  eff(scale, &s, &a);
  return ((ccd - a) * 6000.0 / s + a - FT_CCD_FIRST) * 25.4 / 6000.0;
}

double ft_out_to_ccd(double x, double c, int bin) {
  return (x + c) * bin + FT_CCD_FIRST;
}

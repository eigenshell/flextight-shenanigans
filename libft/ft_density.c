/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_density.h"
#include <math.h>

void ft_dens_t16(const float *t, unsigned short *out, size_t n) {
  size_t i;
  for(i = 0; i < n; i++) {
    float v = t[i];
    out[i] = !(v > 0) ? 0
             : v >= 1 ? 65535
                      : (unsigned short)(v * 65535.0f + 0.5f);
  }
}

void ft_dens_init(ft_dens *d, const double base[3]) {
  int c;
  long u;
  for(c = 0; c < 3; c++) {
    double b = base[c];
    if(!(b >= 0))
      b = 0;
    if(b > 4.5)
      b = 4.5;
    d->base[c] = b;
    d->lut[c][0] = 65535; /* no light: densest */
    for(u = 1; u < 65536; u++) {
      double dd = -log10(u / 65535.0);
      double code =
          floor((dd - b + FT_DENS_OFFSET) / FT_DENS_RANGE * 65535.0 + 0.5);
      d->lut[c][u] = (unsigned short)(code < 0       ? 0
                                      : code > 65535 ? 65535
                                                     : code);
    }
  }
}

void ft_dens_rgb(
    const ft_dens *d, const unsigned short *t16, unsigned short *out, size_t npx
) {
  size_t i;
  for(i = 0; i < npx; i++) {
    out[3 * i + 0] = d->lut[0][t16[3 * i + 0]];
    out[3 * i + 1] = d->lut[1][t16[3 * i + 1]];
    out[3 * i + 2] = d->lut[2][t16[3 * i + 2]];
  }
}

void ft_dens_mono(
    const ft_dens *d, const double w[3], const unsigned short *t16,
    unsigned short *out, size_t npx
) {
  float w0 = (float)w[0], w1 = (float)w[1], w2 = (float)w[2];
  size_t i;
  for(i = 0; i < npx; i++) {
    float v = w0 * d->lut[0][t16[3 * i + 0]] + w1 * d->lut[1][t16[3 * i + 1]] +
              w2 * d->lut[2][t16[3 * i + 2]] + 0.5f;
    out[i] = (unsigned short)(v >= 65535.0f ? 65535 : v);
  }
}

void ft_dens_weights(const double var[3], double w[3]) {
  double s = 0;
  int c;
  for(c = 0; c < 3; c++)
    if(!(var[c] > 0) || !isfinite(var[c]))
      break;
  if(c < 3) {
    w[0] = w[1] = w[2] = 1.0 / 3;
    return;
  }
  for(c = 0; c < 3; c++)
    s += 1 / var[c];
  for(c = 0; c < 3; c++)
    w[c] = 1 / var[c] / s;
}

double ft_dens_code_to_d(unsigned code) {
  return code / 65535.0 * FT_DENS_RANGE - FT_DENS_OFFSET;
}

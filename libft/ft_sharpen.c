/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_sharpen.h"

#include <math.h>
#include <string.h>

#define MAXHW 3 /* ceil(3 * 0.8) */

size_t ft_sharpen_scratch_bytes(int w, int h) {
  return ((size_t)2 * w * h + w) * sizeof(float);
}

static int refl(int i, int n) {
  while (i < 0 || i >= n) i = i < 0 ? -i : 2 * n - 2 - i;
  return i;
}

void ft_sharpen(float* rgb, int w, int h, double amount, double scale,
                void* scratch) {
  size_t n = (size_t)w * h, i;
  float *lp = scratch, *bl = lp + n, *row = bl + n;
  float k[2 * MAXHW + 1];
  double r, s = 0;
  int hw, t, x, y;
  if (!(amount > 0) || w < 1 || h < 1) return;
  if (!(scale > 0) || scale > 1) scale = 1;
  r = 0.5 + 0.3 * (1 / scale - 1 < 1 ? 1 / scale - 1 : 1);
  hw = (int)ceil(3 * r);
  for (t = -hw; t <= hw; t++)
    s += k[t + hw] = (float)exp(-0.5 * t * t / (r * r));
  for (t = 0; t <= 2 * hw; t++) k[t] = (float)(k[t] / s);
  for (i = 0; i < 3 * n; i++)
    if (!isfinite(rgb[i])) rgb[i] = 0;
  for (i = 0; i < n; i++) {
    double yv =
      0.2627 * rgb[3 * i] + 0.6780 * rgb[3 * i + 1] + 0.0593 * rgb[3 * i + 2];
    lp[i] = (float)cbrt(yv);
  }
  /* vertical lp -> bl, then horizontal in place through row */
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++) {
      float v = 0;
      for (t = -hw; t <= hw; t++)
        v += k[t + hw] * lp[(size_t)refl(y + t, h) * w + x];
      bl[(size_t)y * w + x] = v;
    }
  for (y = 0; y < h; y++) {
    float* o = bl + (size_t)y * w;
    memcpy(row, o, (size_t)w * sizeof *row);
    for (x = 0; x < w; x++) {
      float v = 0;
      for (t = -hw; t <= hw; t++) v += k[t + hw] * row[refl(x + t, w)];
      o[x] = v;
    }
  }
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++) {
      size_t at = (size_t)y * w + x;
      float l = lp[at], d = l - bl[at], lo = l, hi = l, f, ln;
      int dx, dy;
      if (!(l > 1e-6f)) continue;
      d = d > FT_SHARPEN_T    ? d - (float)FT_SHARPEN_T
          : d < -FT_SHARPEN_T ? d + (float)FT_SHARPEN_T
                              : 0.f;
      if (d == 0) continue;
      for (dy = -1; dy <= 1; dy++)
        for (dx = -1; dx <= 1; dx++) {
          float v = lp[(size_t)refl(y + dy, h) * w + refl(x + dx, w)];
          if (v < lo) lo = v;
          if (v > hi) hi = v;
        }
      ln = l + (float)amount * d;
      {
        float m = (float)FT_SHARPEN_CLAMP * (hi - lo);
        if (ln > hi + m) ln = hi + m;
        if (ln < lo - m) ln = lo - m;
      }
      if (ln < 0) ln = 0;
      f = ln / l;
      f = f * f * f;
      rgb[3 * at] *= f;
      rgb[3 * at + 1] *= f;
      rgb[3 * at + 2] *= f;
    }
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_atrous.h"

#include <math.h>
#include <string.h>

/* sqrt(sum((a_j - a_{j+1})^2)) over the 2-D equivalent filters a_j of
 * c_j: the exact per-scale std for unit white noise */
const double FT_ATROUS_F[FT_ATROUS_SCALES] = {0.890796, 0.200664, 0.085508,
                                              0.041217, 0.020425};
const double FT_ATROUS_W[FT_ATROUS_SCALES] = {1.0, 0.8, 0.5, 0.25, 0.1};

static const float B3[5] = {1.f / 16, 4.f / 16, 6.f / 16, 4.f / 16, 1.f / 16};

/* mirror without repeating the edge sample; one reflection is enough for
 * |i| < n + n - 1 (n >= FT_ATROUS_MIN covers the hole 16 x 2) */
static int refl(int i, int n) {
  return i < 0 ? -i : i >= n ? 2 * n - 2 - i : i;
}

static float clean(float v) { return isfinite(v) ? v : 0.f; }

size_t ft_atrous_scratch_bytes(int w, int band_h) {
  int rows = band_h + 2 * FT_ATROUS_HALO;
  return ((size_t)2 * rows * w + w) * sizeof(float);
}

void ft_atrous_smooth(const float* in, int w, int h, int j, float* out,
                      float* row) {
  int s = 1 << j, x, y, t;
  /* vertical in -> out, then horizontal in place through row */
  for (y = 0; y < h; y++) {
    const float* r[5];
    float* o = out + (size_t)y * w;
    for (t = 0; t < 5; t++) r[t] = in + (size_t)refl(y + (t - 2) * s, h) * w;
    for (x = 0; x < w; x++)
      o[x] = B3[0] * r[0][x] +
             B3[1] * r[1][x] +
             B3[2] * r[2][x] +
             B3[3] * r[3][x] +
             B3[4] * r[4][x];
  }
  for (y = 0; y < h; y++) {
    float* o = out + (size_t)y * w;
    memcpy(row, o, (size_t)w * sizeof *row);
    for (x = 0; x < w; x++)
      o[x] = B3[0] * row[refl(x - 2 * s, w)] +
             B3[1] * row[refl(x - s, w)] +
             B3[2] * row[x] +
             B3[3] * row[refl(x + s, w)] +
             B3[4] * row[refl(x + 2 * s, w)];
  }
}

static float soft(float d, float t) {
  return d > t ? d - t : d < -t ? d + t : 0.f;
}

void ft_atrous_band(const float* in, int w, int h, int y0, int y1, float* out,
                    double k, double sigma, const double* wts, void* scratch) {
  int r0, r1, n, j, y, x;
  float *a = scratch, *b, *row, *t;
  if (y0 < 0) y0 = 0;
  if (y1 > h) y1 = h;
  if (y0 >= y1) return;
  if (w < FT_ATROUS_MIN || h < FT_ATROUS_MIN) {
    for (y = y0; y < y1; y++)
      for (x = 0; x < w; x++)
        out[(size_t)y * w + x] = clean(in[(size_t)y * w + x]);
    return;
  }
  if (!wts) wts = FT_ATROUS_W;
  /* the region the band depends on; mirrored at its ends, which only
   * matters (and is exact) where they are the image edges */
  r0 = y0 - FT_ATROUS_HALO < 0 ? 0 : y0 - FT_ATROUS_HALO;
  r1 = y1 + FT_ATROUS_HALO > h ? h : y1 + FT_ATROUS_HALO;
  n = r1 - r0;
  b = a + (size_t)n * w;
  row = b + (size_t)n * w;
  for (y = 0; y < n; y++)
    for (x = 0; x < w; x++)
      a[(size_t)y * w + x] = clean(in[(size_t)(r0 + y) * w + x]);
  for (y = y0; y < y1; y++)
    memset(out + (size_t)y * w, 0, (size_t)w * sizeof *out);
  for (j = 0; j < FT_ATROUS_SCALES; j++) {
    float th = (float)(k * sigma * FT_ATROUS_F[j] * wts[j]);
    ft_atrous_smooth(a, w, n, j, b, row);
    for (y = y0; y < y1; y++) {
      const float* c = a + (size_t)(y - r0) * w;
      const float* s = b + (size_t)(y - r0) * w;
      float* o = out + (size_t)y * w;
      for (x = 0; x < w; x++) o[x] += soft(c[x] - s[x], th);
    }
    t = a;
    a = b;
    b = t;
  }
  for (y = y0; y < y1; y++) {
    const float* c = a + (size_t)(y - r0) * w;
    float* o = out + (size_t)y * w;
    for (x = 0; x < w; x++) o[x] += c[x];
  }
}

void ft_atrous(const float* in, int w, int h, float* out, double k,
               double sigma, const double* wts, void* scratch) {
  /* the band copies the whole input first, so out may equal in */
  ft_atrous_band(in, w, h, 0, h, out, k, sigma, wts, scratch);
}

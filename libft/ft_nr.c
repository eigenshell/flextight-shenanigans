/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_nr.h"
#include "ft_atrous.h"
#include <math.h>
#include <string.h>

#define BLOCK 32
#define R3 0.57735026918962576 /* 1/sqrt3 */
#define R2 0.70710678118654752 /* 1/sqrt2 */
#define R6 0.40824829046386302 /* 1/sqrt6 */

size_t ft_nr_scratch_bytes(int w, int h) {
  /* 3 opponent planes + the a-trous run (2 planes + a row) */
  return (size_t)3 * w * h * sizeof(float) + ft_atrous_scratch_bytes(w, h);
}

void ft_nr_vst(const float *y, size_t n, double s, double o, float *z) {
  size_t i;
  if(o < 1e-30)
    o = 1e-30;
  for(i = 0; i < n; i++) {
    double v = y[i];
    if(s > 0) {
      double a = s * v + 0.375 * s * s + o;
      z[i] = (float)(2 / s * sqrt(a > 0 ? a : 0));
    } else {
      z[i] = (float)(v / sqrt(o));
    }
  }
}

void ft_nr_vst_inv(const float *z, size_t n, double s, double o, float *y) {
  /* normalised: y' = y / S, sigma'^2 = O / S^2, z = 2 sqrt(y' + 3/8 +
   * sigma'^2); Makitalo & Foi's closed form of E[z] -> y', clamped at 0.
   * Below z = 2 sqrt(3/8) (y' = 0 without read noise) the series is
   * meaningless and y' = 0. */
  const double c = sqrt(1.5), zmin = 2 * sqrt(0.375);
  double s2;
  size_t i;
  if(o < 1e-30)
    o = 1e-30;
  if(!(s > 0)) {
    for(i = 0; i < n; i++)
      y[i] = (float)(z[i] * sqrt(o));
    return;
  }
  s2 = o / (s * s);
  for(i = 0; i < n; i++) {
    double d = z[i], e = 0;
    if(d > zmin) {
      double r = 1 / d;
      e = 0.25 * d * d + 0.25 * c * r - 1.375 * r * r + 0.625 * c * r * r * r -
          0.125 - s2;
      if(e < 0)
        e = 0;
    }
    y[i] = (float)(e * s);
  }
}

/* k-th smallest of a[0..n), reordering a (Hoare selection) */
static float select_k(float *a, long n, long k) {
  long lo = 0, hi = n - 1;
  while(lo < hi) {
    float p = a[lo + (hi - lo) / 2], t;
    long i = lo, j = hi;
    while(i <= j) {
      while(a[i] < p)
        i++;
      while(a[j] > p)
        j--;
      if(i <= j) {
        t = a[i];
        a[i] = a[j];
        a[j] = t;
        i++;
        j--;
      }
    }
    if(k <= j)
      hi = j;
    else if(k >= i)
      lo = i;
    else
      break;
  }
  return a[k];
}

double ft_nr_sigma(const float *x, int w, int h, void *scratch) {
  size_t n = (size_t)w * h;
  float *c1 = scratch, *c2 = c1 + n, *c3 = c2 + n, *d = c3 + n;
  int bw = w / BLOCK, bh = h / BLOCK, bx, by, xx, yy;
  long nb = (long)bw * bh, b, cnt = 0, keep;
  float cut;
  if(w < FT_ATROUS_MIN || h < FT_ATROUS_MIN || nb < 1)
    return 0;
  /* c1, c2, c3; the rows of d serve as the smoothing row buffer */
  ft_atrous_smooth(x, w, h, 0, c1, d);
  ft_atrous_smooth(c1, w, h, 1, c2, d);
  ft_atrous_smooth(c2, w, h, 2, c3, d);
  /* block energy of scale 2 (c2 - c3) into d[0..nb), a copy for the cut
   * into d[nb..2nb) */
  for(b = 0; b < nb; b++) {
    double s = 0;
    bx = (int)(b % bw);
    by = (int)(b / bw);
    for(yy = by * BLOCK; yy < (by + 1) * BLOCK; yy++)
      for(xx = bx * BLOCK; xx < (bx + 1) * BLOCK; xx++) {
        size_t i = (size_t)yy * w + xx;
        double v = (double)c2[i] - c3[i];
        s += v * v;
      }
    d[b] = (float)s;
    d[nb + b] = (float)s;
  }
  keep = nb < 4 ? nb : nb / 4;
  cut = select_k(d + nb, nb, keep - 1);
  /* |finest scale| of the kept blocks into c3 (no longer needed) */
  for(b = 0; b < nb && cnt < keep * BLOCK * BLOCK; b++) {
    if(d[b] > cut)
      continue;
    bx = (int)(b % bw);
    by = (int)(b / bw);
    for(yy = by * BLOCK; yy < (by + 1) * BLOCK; yy++)
      for(xx = bx * BLOCK; xx < (bx + 1) * BLOCK; xx++) {
        size_t i = (size_t)yy * w + xx;
        c3[cnt++] = fabsf(x[i] - c1[i]);
      }
  }
  return select_k(c3, cnt, cnt / 2) / 0.6745 / FT_ATROUS_F[0];
}

static float fin(float v) {
  return isfinite(v) ? v : 0.f;
}

void ft_nr_estimate(
    const float *rgb, int w, int h, double prof[6], void *scratch
) {
  size_t n = (size_t)w * h, i;
  float *p = scratch;
  int c;
  for(c = 0; c < 3; c++) {
    double sg;
    for(i = 0; i < n; i++)
      p[i] = fin(rgb[3 * i + c]);
    sg = ft_nr_sigma(p, w, h, p + n);
    prof[2 * c] = 0;
    prof[2 * c + 1] = sg * sg;
  }
}

static double density(float t) {
  double v = fin(t);
  return -log10(v > 1e-5 ? v : 1e-5);
}

double ft_nr_grain_sigma(const float *rgb, int w, int h, void *scratch) {
  size_t n = (size_t)w * h, i;
  float *y = scratch;
  for(i = 0; i < n; i++)
    y[i] = (float)((density(rgb[3 * i]) + density(rgb[3 * i + 1]) +
                    density(rgb[3 * i + 2])) *
                   R3);
  return ft_nr_sigma(y, w, h, y + n);
}

/* opponent planes from three channel values */
static void to_opp(
    double r, double g, double b, float *p0, float *p1, float *p2, size_t i
) {
  p0[i] = (float)((r + g + b) * R3);
  p1[i] = (float)((r - b) * R2);
  p2[i] = (float)((r - 2 * g + b) * R6);
}

static void from_opp(
    const float *p0, const float *p1, const float *p2, size_t i, double v[3]
) {
  double y = p0[i] * R3, c1 = p1[i] * R2, c2 = p2[i] * R6;
  v[0] = y + c1 + c2;
  v[1] = y - 2 * c2;
  v[2] = y - c1 + c2;
}

void ft_nr_sensor(
    float *rgb, int w, int h, const double prof[6], double k, double kc,
    void *scratch
) {
  size_t n = (size_t)w * h, i;
  float *p0 = scratch, *p1 = p0 + n, *p2 = p1 + n, *a = p2 + n;
  int c;
  if(!(k > 0) && !(kc > 0))
    return;
  if(w < FT_ATROUS_MIN || h < FT_ATROUS_MIN)
    return;
  /* the transform channel by channel (through p0), then opponent */
  for(c = 0; c < 3; c++) {
    for(i = 0; i < n; i++)
      a[i] = fin(rgb[3 * i + c]);
    ft_nr_vst(a, n, prof[2 * c], prof[2 * c + 1], a);
    for(i = 0; i < n; i++)
      rgb[3 * i + c] = a[i];
  }
  for(i = 0; i < n; i++)
    to_opp(rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2], p0, p1, p2, i);
  ft_atrous(p0, w, h, p0, k, 1, NULL, a);
  ft_atrous(p1, w, h, p1, kc, 1, NULL, a);
  ft_atrous(p2, w, h, p2, kc, 1, NULL, a);
  for(i = 0; i < n; i++) {
    double v[3];
    from_opp(p0, p1, p2, i, v);
    rgb[3 * i] = (float)v[0];
    rgb[3 * i + 1] = (float)v[1];
    rgb[3 * i + 2] = (float)v[2];
  }
  for(c = 0; c < 3; c++) {
    for(i = 0; i < n; i++)
      a[i] = rgb[3 * i + c];
    ft_nr_vst_inv(a, n, prof[2 * c], prof[2 * c + 1], a);
    for(i = 0; i < n; i++)
      rgb[3 * i + c] = a[i];
  }
}

void ft_nr_grain(float *rgb, int w, int h, double k, void *scratch) {
  size_t n = (size_t)w * h, i;
  float *p0 = scratch, *p1 = p0 + n, *p2 = p1 + n, *a = p2 + n;
  double sg;
  if(!(k > 0) || w < FT_ATROUS_MIN || h < FT_ATROUS_MIN)
    return;
  sg = ft_nr_grain_sigma(rgb, w, h, p0); /* all 5 planes */
  for(i = 0; i < n; i++)
    to_opp(
        density(rgb[3 * i]), density(rgb[3 * i + 1]), density(rgb[3 * i + 2]),
        p0, p1, p2, i
    );
  ft_atrous(p0, w, h, p0, k, sg, NULL, a);
  ft_atrous(p1, w, h, p1, 0.5 * k, sg, NULL, a);
  ft_atrous(p2, w, h, p2, 0.5 * k, sg, NULL, a);
  for(i = 0; i < n; i++) {
    double v[3];
    int c;
    from_opp(p0, p1, p2, i, v);
    for(c = 0; c < 3; c++)
      rgb[3 * i + c] = (float)pow(10, -v[c]);
  }
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_resize.h"

#include <math.h>
#include <string.h>

#define PI 3.14159265358979323846
#define LOBES 3

static double lanczos(double x) {
  double a;
  if (x < 0) x = -x;
  if (x < 1e-9) return 1;
  if (x >= LOBES) return 0;
  a = PI * x;
  return LOBES * sin(a) * sin(a / LOBES) / (a * a);
}

void ft_resize_dims(int w, int h, double s, int* ow, int* oh) {
  *ow = (int)floor(w * s + 0.5);
  *oh = (int)floor(h * s + 0.5);
  if (*ow < 1) *ow = 1;
  if (*oh < 1) *oh = 1;
}

size_t ft_resize_scratch_bytes(int w, int h, int ow, int oh) {
  (void)w;
  (void)oh;
  return (size_t)h * ow * 3 * sizeof(float);
}

/* one output sample along a run of n inputs (stride st floats): the
 * filter centred on output pixel i of on; RGB at once */
static void sample(const float* src, int n, size_t st, int i, int on,
                   float out[3]) {
  double scale = (double)on / n;      /* <= 1 */
  double c = (i + 0.5) / scale - 0.5; /* centre in input pixels */
  double r = LOBES / scale, acc[3] = {0, 0, 0}, wsum = 0;
  int a = (int)ceil(c - r), b = (int)floor(c + r), k;
  if (a < 0) a = 0;
  if (b > n - 1) b = n - 1;
  for (k = a; k <= b; k++) {
    double wk = lanczos((k - c) * scale);
    const float* p = src + (size_t)k * st;
    acc[0] += wk * p[0];
    acc[1] += wk * p[1];
    acc[2] += wk * p[2];
    wsum += wk;
  }
  if (wsum == 0) wsum = 1;
  out[0] = (float)(acc[0] / wsum);
  out[1] = (float)(acc[1] / wsum);
  out[2] = (float)(acc[2] / wsum);
}

int ft_resize(const float* in, int w, int h, float* out, int ow, int oh,
              void* scratch, ft_err* e) {
  float* t = scratch;
  int x, y;
  if (w < 1 || h < 1 || ow < 1 || oh < 1)
    return ft_fail(e, "resize: empty image");
  if (ow > w || oh > h)
    return ft_fail(e, "resize: %dx%d -> %dx%d would upscale", w, h, ow, oh);
  if (ow == w && oh == h) {
    memmove(out, in, (size_t)w * h * 3 * sizeof *out);
    return 0;
  }
  for (y = 0; y < h; y++) /* rows: w -> ow */
    for (x = 0; x < ow; x++)
      sample(in + (size_t)y * w * 3, w, 3, x, ow, t + ((size_t)y * ow + x) * 3);
  for (x = 0; x < ow; x++) /* columns: h -> oh */
    for (y = 0; y < oh; y++)
      sample(t + (size_t)x * 3, h, (size_t)ow * 3, y, oh,
             out + ((size_t)y * ow + x) * 3);
  return 0;
}

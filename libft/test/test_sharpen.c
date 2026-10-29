/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "ft_sharpen.h"
#include "test/ftest.h"

static uint32_t rng = 9;
static double urand(void) {
  rng = rng * 1664525u + 1013904223u;
  return ((rng >> 8) + 0.5) / 16777216.0;
}
static double gauss(void) {
  return sqrt(-2 * log(urand())) * cos(6.283185307179586 * urand());
}

enum { W = 120, H = 80 };
static float in[W * H * 3], out[W * H * 3];
static unsigned char scratch[(2 * W * H + W) * sizeof(float)];

static void grey(float* im, int x, int y, double v) {
  im[3 * (y * W + x)] = im[3 * (y * W + x) + 1] = im[3 * (y * W + x) + 2] =
    (float)v;
}

int main(void) {
  int i, x, y;

  CHECK(ft_get_sharpen_scratch_bytes(W, H) <= sizeof scratch);

  /* amount 0: untouched */
  for(i = 0; i < W * H * 3; i++) in[i] = (float)(0.1 + 0.05 * urand());
  memcpy(out, in, sizeof out);
  ft_sharpen(out, W, H, 0, 1, scratch);
  CHECK(memcmp(out, in, sizeof out) == 0);

  /* a soft step (Y 0.2 -> 0.8 over 3 px): steeper after, and Lp stays
   * within the 3x3 range +- 10 % (no halos), with some overshoot */
  for(y = 0; y < H; y++)
    for(x = 0; x < W; x++)
      grey(in, x, y,
           x < W / 2 - 1   ? 0.2
           : x > W / 2 + 1 ? 0.8
                           : 0.2 + 0.6 * (x - (W / 2 - 1)) / 3.0);
  memcpy(out, in, sizeof out);
  ft_sharpen(out, W, H, 1.5, 1, scratch);
  {
    double lo = cbrt(0.2), hi = cbrt(0.8), m = 0.1 * (hi - lo);
    double gin = 0, gout = 0, top = 0;
    for(x = 1; x < W; x++) {
      double a = cbrt(in[3 * (H / 2 * W + x) + 1]) -
                 cbrt(in[3 * (H / 2 * W + x - 1) + 1]);
      double b = cbrt(out[3 * (H / 2 * W + x) + 1]) -
                 cbrt(out[3 * (H / 2 * W + x - 1) + 1]);
      if(a > gin) gin = a;
      if(b > gout) gout = b;
    }
    CHECK(gout > 1.05 * gin); /* measured 1.11 at amount 1.5, r 0.5 */
    for(i = 0; i < W * H; i++) {
      double l = cbrt(out[3 * i + 1]);
      CHECK(l <= hi + m + 1e-6 && l >= lo - m - 1e-6);
      if(l > top) top = l;
    }
    CHECK(top > hi + 1e-4);
  }

  /* noise far below the threshold on a flat grey: untouched */
  for(y = 0; y < H; y++)
    for(x = 0; x < W; x++) {
      double l = 0.5 + 0.0005 * gauss();
      grey(in, x, y, l * l * l);
    }
  memcpy(out, in, sizeof out);
  ft_sharpen(out, W, H, 2, 0.6, scratch);
  CHECK(memcmp(out, in, sizeof out) == 0);

  /* colour keeps its ratios, neutrals stay neutral, NaN -> 0 */
  for(y = 0; y < H; y++)
    for(x = 0; x < W; x++) {
      float* p = &in[3 * (y * W + x)];
      double v = x < W / 2 ? 0.1 : 0.5;
      p[0] = (float)v;
      p[1] = (float)(0.5 * v);
      p[2] = (float)(0.25 * v);
    }
  in[3 * (5 * W + 5) + 1] = NAN;
  memcpy(out, in, sizeof out);
  ft_sharpen(out, W, H, 1, 0.5, scratch);
  CHECK(out[3 * (5 * W + 5) + 1] == 0);
  for(i = 0; i < W * H; i++)
    if(i != 5 * W + 5)
      CHECK(fabs(out[3 * i + 1] / out[3 * i] - 0.5) < 1e-5 &&
            fabs(out[3 * i + 2] / out[3 * i] - 0.25) < 1e-5);
  for(y = 0; y < H; y++)
    for(x = 0; x < W; x++) grey(in, x, y, x < W / 2 ? 0.1 : 0.5);
  memcpy(out, in, sizeof out);
  ft_sharpen(out, W, H, 1, 1, scratch);
  for(i = 0; i < W * H; i++)
    CHECK(out[3 * i] == out[3 * i + 1] && out[3 * i + 1] == out[3 * i + 2]);
  return TEST_DONE();
}

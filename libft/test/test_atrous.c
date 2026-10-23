/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_atrous.h"
#include "test/ftest.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

static uint32_t rng = 1234;
static double urand(void) {
  rng = rng * 1664525u + 1013904223u;
  return ((rng >> 8) + 0.5) / 16777216.0;
}
static double gauss(void) {
  return sqrt(-2 * log(urand())) * cos(6.283185307179586 * urand());
}

enum { N = 1024, W = 300, H = 400 };
static float big[N * N], c0[N * N], c1[N * N], row[N];
static float in[W * H], out[W * H], band[W * H];
static unsigned char scratch[(2 * N * N + N) * sizeof(float)];

int main(void) {
  int i, j, y0;
  float *a = c0, *b = c1, *t;

  /* the exact factors F_j match the measured std of w_j on unit white
   * noise (+-2 %) */
  for(i = 0; i < N * N; i++)
    big[i] = (float)gauss();
  memcpy(a, big, sizeof big);
  for(j = 0; j < FT_ATROUS_SCALES; j++) {
    double m = 0, m2 = 0;
    ft_atrous_smooth(a, N, N, j, b, row);
    for(i = 0; i < N * N; i++) {
      double d = (double)a[i] - b[i];
      m += d;
      m2 += d * d;
    }
    m /= N * N;
    CHECK_NEAR(
        sqrt(m2 / (N * N) - m * m), FT_ATROUS_F[j], 0.02 * FT_ATROUS_F[j]
    );
    t = a;
    a = b;
    b = t;
  }

  /* threshold 0 reconstructs */
  for(i = 0; i < W * H; i++)
    in[i] = (float)urand();
  ft_atrous(in, W, H, out, 0, 1, NULL, scratch);
  for(i = 0; i < W * H; i++)
    CHECK(fabs(out[i] - in[i]) <= 1e-6);

  /* shrinkage at k = 3 removes most of unit white noise */
  for(i = 0; i < W * H; i++)
    in[i] = (float)gauss();
  ft_atrous(in, W, H, out, 3, 1, NULL, scratch);
  {
    double so = 0;
    for(i = 0; i < W * H; i++)
      so += (double)out[i] * out[i];
    CHECK(sqrt(so / (W * H)) < 0.25);
  }

  /* a gradient has no detail away from the mirrored edges: it survives
   * any threshold */
  for(i = 0; i < W * H; i++)
    in[i] = (float)(0.001 * (i % W) + 0.0005 * (i / W));
  ft_atrous(in, W, H, out, 3, 0.01, NULL, scratch);
  for(i = 0; i < W * H; i++)
    if(i % W >= 64 && i % W < W - 64 && i / W >= 64 && i / W < H - 64)
      CHECK(fabs(out[i] - in[i]) <= 1e-5);

  /* bands are bit-identical to the whole-image run */
  for(i = 0; i < W * H; i++)
    in[i] = (float)(urand() + (i % W > W / 2));
  ft_atrous(in, W, H, out, 1.7, 0.2, NULL, scratch);
  memset(band, 0, sizeof band);
  for(y0 = 0; y0 < H; y0 += 37)
    ft_atrous_band(in, W, H, y0, y0 + 37, band, 1.7, 0.2, NULL, scratch);
  CHECK(memcmp(out, band, sizeof out) == 0);
  CHECK(ft_atrous_scratch_bytes(W, 37) < ft_atrous_scratch_bytes(W, H));

  /* in place */
  memcpy(band, in, sizeof in);
  ft_atrous(band, W, H, band, 1.7, 0.2, NULL, scratch);
  CHECK(memcmp(out, band, sizeof out) == 0);

  /* a plane below the support passes through, NaN/inf -> 0 */
  for(i = 0; i < 20 * 20; i++)
    in[i] = (float)urand();
  in[0] = NAN;
  in[1] = INFINITY;
  ft_atrous(in, 20, 20, out, 2, 1, NULL, scratch);
  CHECK(out[0] == 0 && out[1] == 0);
  for(i = 2; i < 20 * 20; i++)
    CHECK(out[i] == in[i]);
  return TEST_DONE();
}

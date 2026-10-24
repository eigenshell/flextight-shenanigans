/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "ft_nr.h"
#include "test/ftest.h"

static uint32_t rng = 5;
static double urand(void) {
  rng = rng * 1664525u + 1013904223u;
  return ((rng >> 8) + 0.5) / 16777216.0;
}
static double gauss(void) {
  return sqrt(-2 * log(urand())) * cos(6.283185307179586 * urand());
}
/* Knuth below 30, a rounded normal above */
static double poisson(double lam) {
  if (lam < 30) {
    double l = exp(-lam), p = 1;
    int k = -1;
    do {
      k++;
      p *= urand();
    } while (p > l);
    return k;
  } else {
    double v = floor(lam + sqrt(lam) * gauss() + 0.5);
    return v < 0 ? 0 : v;
  }
}

/* a scanner-like profile: ~5000 photo-electron equivalents at T = 1 */
static const double S = 2e-4, O = 1e-7;
static double pg(double t) /* one Poisson-Gaussian sample */
{
  return S * poisson(t / S) + sqrt(O) * gauss();
}

enum { NZ = 1 << 18, W = 256, H = 256 };
static float z[NZ], zi[NZ];
static float clean[W * H * 3], noisy[W * H * 3], out[W * H * 3];
static unsigned char scratch[5 * W * H * sizeof(float) + W * 4 + 4096 * 1024];

static double opp(const float* p, int c) {
  return c == 0   ? (p[0] + p[1] + p[2]) / sqrt(3.0)
         : c == 1 ? (p[0] - p[2]) / sqrt(2.0)
                  : (p[0] - 2 * p[1] + p[2]) / sqrt(6.0);
}

/* the 10-90 % rise of the row-averaged green across the step at W/2 */
static double rise(const float* im, double lo, double hi) {
  double prof[W], x10 = -1, x90 = -1;
  int x, y;
  for (x = 0; x < W; x++) {
    prof[x] = 0;
    for (y = 40; y < H - 40; y++) prof[x] += im[3 * (y * W + x) + 1];
    prof[x] /= H - 80;
  }
  for (x = W / 2 - 20; x < W / 2 + 20; x++) {
    double a = (prof[x] - lo) / (hi - lo), b = (prof[x + 1] - lo) / (hi - lo);
    if (x10 < 0 && a < 0.1 && b >= 0.1) x10 = x + (0.1 - a) / (b - a);
    if (x90 < 0 && a < 0.9 && b >= 0.9) x90 = x + (0.9 - a) / (b - a);
  }
  return x90 - x10;
}

int main(void) {
  double prof[6];
  int i, c, lvl, x, y;

  for (c = 0; c < 3; c++) {
    prof[2 * c] = S;
    prof[2 * c + 1] = O;
  }

  /* the transform stabilises Poisson-Gaussian noise to sigma 1 +- 0.05
   * from T = 0.001 (5 counts) to 1 */
  for (lvl = 0; lvl < 8; lvl++) {
    double t = 0.001 * pow(1000, lvl / 7.0), m = 0, m2 = 0;
    for (i = 0; i < NZ; i++) z[i] = (float)pg(t);
    ft_nr_vst(z, NZ, S, O, zi);
    for (i = 0; i < NZ; i++) {
      m += zi[i];
      m2 += (double)zi[i] * zi[i];
    }
    m /= NZ;
    CHECK_NEAR(sqrt(m2 / NZ - m * m), 1.0, 0.05);
  }

  /* the unbiased inverse of a denoised value (the mean of z over a flat
   * patch) returns the patch's mean T within 0.2 %; the algebraic inverse
   * misses by > 2 % at T = 0.001 */
  for (lvl = 0; lvl < 4; lvl++) {
    static const double tv[4] = {0.001, 0.01, 0.15, 0.6};
    double m = 0, my = 0;
    float mz, back;
    for (i = 0; i < NZ; i++) {
      z[i] = (float)pg(tv[lvl]);
      my += z[i];
    }
    my /= NZ; /* the sample's own mean T */
    ft_nr_vst(z, NZ, S, O, zi);
    for (i = 0; i < NZ; i++) m += zi[i];
    mz = (float)(m / NZ);
    ft_nr_vst_inv(&mz, 1, S, O, &back);
    CHECK_NEAR(back, my, 0.002 * my);
    if (lvl == 0) {
      double alg = (S * mz / 2) * (S * mz / 2) / S - 0.375 * S - O / S;
      CHECK(fabs(alg / my - 1) > 0.02);
    }
  }

  /* a step from T 0.05 to 0.4 (a gradient on the left): sensor NR gains
   * >= 6 dB, the edge rise grows <= 1 px, chroma falls harder than luma,
   * k = 0 is bit-identical */
  for (y = 0; y < H; y++)
    for (x = 0; x < W; x++) {
      double t = x < W / 2 ? 0.03 + 0.04 * x / (W / 2) : 0.4;
      for (c = 0; c < 3; c++) {
        clean[3 * (y * W + x) + c] = (float)t;
        noisy[3 * (y * W + x) + c] = (float)pg(t);
      }
    }
  memcpy(out, noisy, sizeof out);
  ft_nr_sensor(out, W, H, prof, 1, 2, scratch);
  {
    double en = 0, ed = 0;
    for (i = 0; i < W * H * 3; i++) {
      en += (noisy[i] - clean[i]) * (double)(noisy[i] - clean[i]);
      ed += (out[i] - clean[i]) * (double)(out[i] - clean[i]);
    }
    CHECK(10 * log10(en / ed) >= 6);
  }
  CHECK(rise(out, 0.07, 0.4) - rise(clean, 0.07, 0.4) <= 1.0);
  {
    double rn[3] = {0, 0, 0}, rd[3] = {0, 0, 0};
    for (y = 8; y < H - 8; y++)
      for (x = W / 2 + 8; x < W - 8; x++)
        for (c = 0; c < 3; c++) {
          size_t at = 3 * (size_t)(y * W + x);
          double a = opp(noisy + at, c) - opp(clean + at, c);
          double b = opp(out + at, c) - opp(clean + at, c);
          rn[c] += a * a;
          rd[c] += b * b;
        }
    CHECK((rd[1] + rd[2]) / (rn[1] + rn[2]) < 0.8 * rd[0] / rn[0]);
  }
  memcpy(out, noisy, sizeof out);
  ft_nr_sensor(out, W, H, prof, 0, 0, scratch);
  CHECK(memcmp(out, noisy, sizeof out) == 0);

  /* a dense flat patch (T 0.002, 10 counts) keeps its mean within 0.2 %
   * through the whole stage */
  {
    double mn = 0, mo = 0;
    for (i = 0; i < W * H * 3; i++) noisy[i] = (float)pg(0.002);
    memcpy(out, noisy, sizeof out);
    ft_nr_sensor(out, W, H, prof, 1, 2, scratch);
    for (i = 0; i < W * H * 3; i++) {
      mn += noisy[i];
      mo += out[i];
    }
    CHECK_NEAR(mo / mn, 1.0, 0.002);
  }

  /* the fallback estimate: Gaussian sigma 0.01 with a step edge in it,
   * O within 10 % of sigma^2, S = 0 */
  for (y = 0; y < H; y++)
    for (x = 0; x < W; x++)
      for (c = 0; c < 3; c++)
        noisy[3 * (y * W + x) + c] =
          (float)((x < W / 2 ? 0.2 : 0.6) + 0.01 * gauss());
  ft_nr_estimate(noisy, W, H, prof, scratch);
  for (c = 0; c < 3; c++) {
    CHECK(prof[2 * c] == 0);
    CHECK_NEAR(prof[2 * c + 1], 1e-4, 1e-5);
  }

  /* grain: independent sigma 0.02 D per channel over a film base, a
   * gradient and a step edge -> the Y sigma within 10 % of 0.02; k = 0
   * bit-identical; k = 2 smooths a flat patch */
  {
    const double base[3] = {0.4, 0.7, 1.0}, sg = 0.02;
    double before = 0, after = 0;
    for (y = 0; y < H; y++)
      for (x = 0; x < W; x++)
        for (c = 0; c < 3; c++) {
          double d = 0.5 + 0.3 * x / W + (x > W / 2 ? 0.8 : 0) + sg * gauss();
          noisy[3 * (y * W + x) + c] = (float)pow(10, -(d + base[c]));
        }
    CHECK_NEAR(ft_nr_grain_sigma(noisy, W, H, scratch), sg, 0.1 * sg);
    memcpy(out, noisy, sizeof out);
    ft_nr_grain(out, W, H, 0, scratch);
    CHECK(memcmp(out, noisy, sizeof out) == 0);
    memcpy(out, noisy, sizeof out);
    ft_nr_grain(out, W, H, 2, scratch);
    for (y = 20; y < 100; y++)
      for (x = 20; x < 100; x++) {
        size_t at = 3 * (size_t)(y * W + x) + 1;
        double a = log10(noisy[at]) - log10(noisy[at + 3]);
        double b = log10(out[at]) - log10(out[at + 3]);
        before += a * a;
        after += b * b;
      }
    CHECK(after < 0.5 * before);
  }
  return TEST_DONE();
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ft_output.h"
#include "test/ftest.h"

#define PI 3.14159265358979323846

static ft_output out_s, out_p3, out_a, out_clip;

static double frand(void) { return rand() / (RAND_MAX + 1.0); }

static void lch_of(const ft_output* o, const float rgb[3], double lch[3]) {
  double xyz[3], lab[3];
  int i;
  for (i = 0; i < 3; i++)
    xyz[i] = o->to_xyz[3 * i] * rgb[0] +
             o->to_xyz[3 * i + 1] * rgb[1] +
             o->to_xyz[3 * i + 2] * rgb[2];
  ft_xyz_to_oklab(xyz, lab);
  lch[0] = lab[0];
  lch[1] = sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
  lch[2] = atan2(lab[2], lab[1]);
}

/* Rec.2020 linear RGB of OkLCh (L, C, h) */
static void rec2020_of(double L, double C, double h, float rgb[3]) {
  double lab[3], xyz[3], xy[8], m[9], inv[9];
  int i;
  lab[0] = L;
  lab[1] = C * cos(h);
  lab[2] = C * sin(h);
  ft_oklab_to_xyz(lab, xyz);
  ft_space_primaries(FT_SPACE_REC2020, xy);
  ft_rgb_to_xyz(xy, m);
  ft_mat3_inv(m, inv);
  for (i = 0; i < 3; i++)
    rgb[i] = (float)(inv[3 * i] * xyz[0] +
                     inv[3 * i + 1] * xyz[1] +
                     inv[3 * i + 2] * xyz[2]);
}

/* the target's chroma boundary at (L, h), by bisection (the test's own) */
static double boundary_c(const ft_output* o, double L, double h) {
  double lo = 0, hi = 0.6;
  int i, k;
  for (i = 0; i < 50; i++) {
    double c = 0.5 * (lo + hi), lab[3], xyz[3], v[3];
    int in = 1;
    lab[0] = L;
    lab[1] = c * cos(h);
    lab[2] = c * sin(h);
    ft_oklab_to_xyz(lab, xyz);
    for (k = 0; k < 3; k++) {
      v[k] = o->from_xyz[3 * k] * xyz[0] +
             o->from_xyz[3 * k + 1] * xyz[1] +
             o->from_xyz[3 * k + 2] * xyz[2];
      in &= v[k] >= 0 && v[k] <= 1;
    }
    if (in)
      lo = c;
    else
      hi = c;
  }
  return lo;
}

static void to_target(const ft_output* o, const float in[3], float v[3]) {
  v[0] = o->m[0] * in[0] + o->m[1] * in[1] + o->m[2] * in[2];
  v[1] = o->m[3] * in[0] + o->m[4] * in[1] + o->m[5] * in[2];
  v[2] = o->m[6] * in[0] + o->m[7] * in[1] + o->m[8] * in[2];
}

static double hue_diff(double a, double b) {
  double d = fabs(a - b);
  return d > PI ? 2 * PI - d : d;
}

int main(void) {
  double xy[8], m[9], inv[9];
  ft_space s;
  ft_gamut g;
  ft_err e;
  int i, k;

  /* names */
  CHECK(ft_space_find("srgb", &s) == 0 && s == FT_SPACE_SRGB);
  CHECK(ft_space_find("p3", &s) == 0 && s == FT_SPACE_DISPLAY_P3);
  CHECK(ft_space_find("adobe", &s) == 0 && s == FT_SPACE_ADOBE_RGB);
  CHECK(ft_space_find("rec2020", &s) == 0 && s == FT_SPACE_REC2020);
  CHECK(ft_space_find("dci", &s) == -1 && ft_space_find(NULL, &s) == -1);
  CHECK(strcmp(ft_space_name(FT_SPACE_DISPLAY_P3), "p3") == 0);
  CHECK(strcmp(ft_space_what(FT_SPACE_ADOBE_RGB), "Adobe RGB (1998)") == 0);
  CHECK(ft_gamut_find("soft", &g) == 0 && g == FT_GAMUT_SOFT);
  CHECK(ft_gamut_find("clip", &g) == 0 && g == FT_GAMUT_CLIP);
  CHECK(ft_gamut_find("perceptual", &g) == -1);

  /* matrices from the primaries vs published values (those use slightly
   * different white XYZ, Z up to 2.3e-4 apart, hence 5e-4) */
  {
    static const double SRGB[9] = {0.4124564, 0.3575761, 0.1804375,
                                   0.2126729, 0.7151522, 0.0721750,
                                   0.0193339, 0.1191920, 0.9503041};
    static const double P3[9] = {0.4865709, 0.2656677, 0.1982173,
                                 0.2289746, 0.6917385, 0.0792869,
                                 0.0000000, 0.0451134, 1.0439444};
    static const double ADOBE[9] = {0.5767309, 0.1855540, 0.1881852,
                                    0.2973769, 0.6273491, 0.0752741,
                                    0.0270343, 0.0706872, 0.9911085};
    static const double R2020[9] = {0.6369580, 0.1446169, 0.1688810,
                                    0.2627002, 0.6779981, 0.0593017,
                                    0.0000000, 0.0280727, 1.0609851};
    const double* want[4] = {SRGB, P3, ADOBE, R2020};
    for (k = 0; k < 4; k++) {
      ft_space_primaries((ft_space)k, xy);
      ft_rgb_to_xyz(xy, m);
      for (i = 0; i < 9; i++) CHECK_NEAR(m[i], want[k][i], 5e-4);
      /* white: Y = 1, at D65 */
      CHECK_NEAR(m[3] + m[4] + m[5], 1.0, 1e-12);
      CHECK_NEAR(
        (m[0] + m[1] + m[2]) / (m[0] + m[1] + m[2] + 1 + m[6] + m[7] + m[8]),
        0.3127, 1e-9);
    }
    CHECK(ft_mat3_inv(m, inv) == 0);
    {
      double z[9] = {1, 2, 3, 2, 4, 6, 0, 1, 1};
      CHECK(ft_mat3_inv(z, inv) == -1);
    }
  }
  /* Rec.2020 -> sRGB: the inverse of ITU-R BT.2087 (as ft_out.c had) */
  CHECK(ft_output_compile(FT_SPACE_SRGB, FT_GAMUT_SOFT, &out_s, &e) == 0);
  {
    static const double W[9] = {1.6604910021,  -0.5876411388, -0.0728498633,
                                -0.1245504745, 1.1328998971,  -0.0083494226,
                                -0.0181507634, -0.1005788980, 1.1187296614};
    for (i = 0; i < 9; i++) CHECK_NEAR(out_s.m[i], W[i], 2e-6);
  }
  CHECK(ft_output_compile(FT_SPACE_DISPLAY_P3, FT_GAMUT_SOFT, &out_p3, &e) ==
        0);
  CHECK(ft_output_compile(FT_SPACE_ADOBE_RGB, FT_GAMUT_SOFT, &out_a, &e) == 0);
  CHECK(ft_output_compile(FT_SPACE_SRGB, FT_GAMUT_CLIP, &out_clip, &e) == 0);
  CHECK(ft_output_compile((ft_space)9, FT_GAMUT_SOFT, &out_clip, &e) == -1);
  CHECK(ft_output_compile(FT_SPACE_SRGB, (ft_gamut)5, &out_clip, &e) == -1);
  CHECK(ft_output_compile(FT_SPACE_SRGB, FT_GAMUT_CLIP, &out_clip, &e) == 0);

  /* OkLab: Ottosson's published pairs */
  {
    static const double X[4][3] = {
      {0.950, 1.000, 1.089},
      {1,     0,     0    },
      {0,     1,     0    },
      {0,     0,     1    }
    };
    static const double L[4][3] = {
      {1.000, 0.000,  0.000 },
      {0.450, 1.236,  -0.019},
      {0.922, -0.671, 0.263 },
      {0.153, -1.415, -0.449}
    };
    for (k = 0; k < 4; k++) {
      double lab[3], back[3];
      ft_xyz_to_oklab(X[k], lab);
      for (i = 0; i < 3; i++) CHECK_NEAR(lab[i], L[k][i], 1.5e-3);
      ft_oklab_to_xyz(lab, back);
      for (i = 0; i < 3; i++) CHECK_NEAR(back[i], X[k][i], 1e-9);
    }
  }

  /* transfer curves */
  CHECK_NEAR(ft_space_encode(FT_SPACE_SRGB, 0.5), 0.735356983, 1e-8);
  CHECK_NEAR(ft_space_encode(FT_SPACE_SRGB, 0.002), 0.02584, 1e-9);
  CHECK_NEAR(ft_space_encode(FT_SPACE_DISPLAY_P3, 0.5), 0.735356983, 1e-8);
  CHECK_NEAR(ft_space_encode(FT_SPACE_ADOBE_RGB, 0.5), pow(0.5, 256.0 / 563.0),
             1e-12);
  CHECK_NEAR(ft_space_encode(FT_SPACE_REC2020, 0.01), 0.045, 1e-12);
  CHECK(ft_space_encode(FT_SPACE_SRGB, -1) == 0);
  CHECK(ft_space_encode(FT_SPACE_SRGB, 2) == 1);
  for (k = 0; k < 4; k++)
    for (i = 0; i <= 1000; i++) {
      double v = i / 1000.0;
      CHECK_NEAR(ft_space_decode((ft_space)k, ft_space_encode((ft_space)k, v)),
                 v, 1e-9);
    }

  /* apply: the LUT path within 1.5/65535 of the exact curve */
  {
    double worst = 0;
    srand(3);
    for (i = 0; i < 100000; i++) {
      float in[3], o[3], v[3];
      double x = pow(frand(), 2.2);
      in[0] = in[1] = in[2] = (float)x; /* grey: fits every space */
      ft_output_apply(&out_a, in, o, 1);
      to_target(&out_a, in, v);
      for (k = 0; k < 3; k++) {
        double d = fabs(o[k] - ft_space_encode(FT_SPACE_ADOBE_RGB, v[k]));
        if (d > worst) worst = d;
      }
      ft_output_apply(&out_s, in, o, 1);
      to_target(&out_s, in, v);
      for (k = 0; k < 3; k++) {
        double d = fabs(o[k] - ft_space_encode(FT_SPACE_SRGB, v[k]));
        if (d > worst) worst = d;
      }
    }
    CHECK(worst <= 1.5 / 65535);
  }
  /* mid grey 0.18 -> sRGB 0.4614; NaN and inf -> black; in place */
  {
    float px[9] = {0.18f, 0.18f, 0.18f, NAN, 0.5f, 0.5f, 0.5f, INFINITY, 0.5f};
    ft_output_apply(&out_s, px, px, 3);
    CHECK_NEAR(px[0], 0.46135612, 2.0 / 65535);
    CHECK(fabs(px[0] - px[1]) < 1e-6 && fabs(px[1] - px[2]) < 1e-6);
    for (i = 3; i < 9; i++) CHECK(px[i] == 0);
  }

  /* soft gamut: below the knee bit-identical (greys too) */
  srand(7);
  k = 0;
  for (i = 0; i < 20000; i++) {
    float rgb[3], before[3];
    double lch[3];
    rgb[0] = (float)frand();
    rgb[1] = (float)frand();
    rgb[2] = (float)frand();
    if (i % 10 == 0) rgb[1] = rgb[2] = rgb[0];
    lch_of(&out_s, rgb, lch);
    memcpy(before, rgb, sizeof rgb);
    ft_output_gamut(&out_s, rgb);
    /* 10 % under the knee: the table is within 8 % of the edge */
    if (i % 10 == 0 ||
        lch[1] < 0.90 * FT_OUTPUT_KNEE * boundary_c(&out_s, lch[0], lch[2])) {
      CHECK(memcmp(before, rgb, sizeof rgb) == 0);
      k++;
    }
  }
  CHECK(k > 5000); /* the check above ran enough */
  /* every Rec.2020 colour lands in range, keeps its hue and lightness;
   * chroma is monotone and continuous along C at fixed L, h, across
   * the target's edge */
  {
    const ft_output* outs[3] = {&out_s, &out_p3, &out_a};
    double worst_h = 0, worst_l = 0;
    int nsoft = 0;
    for (k = 0; k < 3; k++) {
      int li, hi;
      for (li = 1; li < 20; li++)
        for (hi = 0; hi < 72; hi++) {
          double L = li / 20.0, h = 2 * PI * hi / 72, prev = -1;
          double cmax2020 = 0, prev_in = -1, bc;
          int ci;
          /* the Rec.2020 boundary at (L, h) */
          for (ci = 1; ci <= 400; ci++) {
            float r[3];
            rec2020_of(L, ci * 0.001, h, r);
            if (r[0] < 0 ||
                r[0] > 1 ||
                r[1] < 0 ||
                r[1] > 1 ||
                r[2] < 0 ||
                r[2] > 1)
              break;
            cmax2020 = ci * 0.001;
          }
          bc = boundary_c(outs[k], L, h);
          for (ci = 0; ci <= 100; ci++) {
            double C = cmax2020 * ci / 100, lch[3], lin[3];
            float r[3], v[3];
            rec2020_of(L, C, h, r);
            to_target(outs[k], r, v);
            lch_of(outs[k], v, lin);
            ft_output_gamut(outs[k], v);
            lch_of(outs[k], v, lch);
            /* soft: the Rec.2020 edge is mapped onto (nearly)
             * the target's edge, not onto the knee */
            if (ci == 100 && cmax2020 > 1.02 * bc) CHECK(lch[1] >= 0.9 * bc);
            /* above the knee the slope is at most 1, and a
             * colour inside the target is pulled in */
            if (prev >= 0 && lin[1] > FT_OUTPUT_KNEE * bc)
              CHECK(lch[1] - prev <= lin[1] - prev_in + 1e-4);
            if (cmax2020 > 1.02 * bc &&
                lin[1] > 0.85 * bc &&
                lin[1] < 0.98 * bc) {
              CHECK(lch[1] < lin[1]);
              nsoft++;
            }
            prev_in = lin[1];
            CHECK(lch[1] >= prev - 2e-4); /* monotone */
            CHECK(prev < 0 || lch[1] - prev < 0.02);
            if (C > 0.02 && lch[1] > 0.02 && hue_diff(lch[2], h) > worst_h)
              worst_h = hue_diff(lch[2], h);
            if (fabs(lch[0] - L) > worst_l) worst_l = fabs(lch[0] - L);
            prev = lch[1];
          }
        }
    }
    CHECK(nsoft > 100);            /* the pull-in check ran */
    CHECK(worst_h < 2 * PI / 180); /* < 2 degrees */
    CHECK(worst_l < 0.01);
  }
  /* huge and NaN input never fails: finite, in range, black for NaN */
  {
    static const float BAD[3][3] = {
      {3e38f,  0,     0    },
      {-3e38f, 3e38f, 0    },
      {1e30f,  1e30f, 1e30f}
    };
    const ft_output* all[4] = {&out_s, &out_p3, &out_a, &out_clip};
    ft_output rec;
    float nan3[3] = {NAN, 0.5f, 0.5f};
    int j, c;
    CHECK(ft_output_compile(FT_SPACE_REC2020, FT_GAMUT_SOFT, &rec, &e) == 0);
    for (k = 0; k < 5; k++)
      for (j = 0; j < 3; j++) {
        float o[3];
        ft_output_apply(k < 4 ? all[k] : &rec, BAD[j], o, 1);
        for (c = 0; c < 3; c++) CHECK(o[c] >= 0 && o[c] <= 1);
      }
    ft_output_gamut(&out_s, nan3);
    CHECK(nan3[0] == 0 && nan3[1] == 0 && nan3[2] == 0);
  }
  /* clip: a plain per-channel clamp */
  {
    float v[3] = {-0.2f, 0.5f, 1.3f};
    ft_output_gamut(&out_clip, v);
    CHECK(v[0] == 0 && v[1] == 0.5f && v[2] == 1);
  }

  /* 8-bit: TPDF dither, deterministic, exact at 0 and 1, unbiased */
  {
    enum { N = 60000 };
    static float flat[N * 3];
    static unsigned char q[N * 3], q2[N * 3];
    double sum = 0;
    for (i = 0; i < N * 3; i++) flat[i] = 0.5004f;
    ft_output_quantise8(flat, q, N, 0);
    ft_output_quantise8(flat, q2, N, 0);
    CHECK(memcmp(q, q2, sizeof q) == 0);
    for (i = 0; i < N * 3; i++) {
      CHECK(q[i] >= 127 && q[i] <= 129);
      sum += q[i];
    }
    CHECK_NEAR(sum / (N * 3) / 255, 0.5004, 0.01 / 255);
    /* tiles: two halves with their own first pixel = the whole */
    ft_output_quantise8(flat, q2, N / 2, 0);
    ft_output_quantise8(flat + N / 2 * 3, q2 + N / 2 * 3, N / 2, N / 2);
    CHECK(memcmp(q, q2, sizeof q) == 0);
    flat[0] = 0;
    flat[1] = 1;
    flat[2] = -3;
    flat[3] = NAN;
    ft_output_quantise8(flat, q, 2, 77);
    CHECK(q[0] == 0 && q[1] == 255 && q[2] == 0 && q[3] == 0);
  }
  /* 16-bit: rounded */
  {
    float v[3] = {0.5f, 1.0f / 65535 * 0.49f, 1.2f};
    unsigned short q[3];
    ft_output_quantise16(v, q, 1);
    CHECK(q[0] == 32768 && q[1] == 0 && q[2] == 65535);
  }
  return TEST_DONE();
}

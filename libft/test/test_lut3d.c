/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include "ft_pm.h"
#include "ft_print.h"
#include "test/ftest.h"

static ft_print_render pr;

/* an affine table: out_k = A[k] + B[k] . x (x = the node's 0..1
 * coordinates), which tetrahedral interpolation reproduces exactly */
static const double A[3] = {0.05, 0.1, 0.02};
static const double B[3][3] = {
  {0.8, 0.1, 0.0}, {0.0, 0.7, 0.2}, {0.1, 0.0, 0.85}};
enum { NC = 5 };
static float nodes[NC * NC * NC * 3];
static const ft_cube CUBE = {NC, nodes};
static const float IDM[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
static const ft_pm_lut3d_par LP = {&CUBE, -0.2f, 2.0f};
static const ft_pm_stock_par FLAT = {1, 1, {1, 1, 1}};
static const ft_emulsion LUT = {"test-lut",
                                "Test lut",
                                FT_RENDER_NEG,
                                &ft_pm_film_stock,
                                &FLAT,
                                &ft_pm_balance_classic,
                                NULL,
                                &ft_pm_print_lut3d,
                                &LP,
                                &ft_pm_output_classic,
                                IDM};
static const ft_emulsion LUT_BW = {"test-lut-bw",
                                   "Test",
                                   FT_RENDER_BW,
                                   &ft_pm_film_stock,
                                   &FLAT,
                                   &ft_pm_balance_classic,
                                   NULL,
                                   &ft_pm_print_lut3d,
                                   &LP,
                                   &ft_pm_output_classic,
                                   IDM};

/* the chain by hand: e = d + lift, x = (e - lo) / (hi - lo), the affine
 * table, sRGB decode */
static void expect(const ft_recipe* r, const float t[3], double out[3]) {
  double x[3];
  int c, k;
  for(c = 0; c < 3; c++) {
    double e = (-log10(t[c]) - r->base[c]) / r->range[c] * r->range[1] +
               r->exposure * log10(2) +
               r->tint[c] / 100.0;
    x[c] = (e - LP.lo) / (LP.hi - LP.lo);
    x[c] = x[c] < 0 ? 0 : x[c] > 1 ? 1 : x[c];
  }
  for(k = 0; k < 3; k++)
    out[k] =
      ft_srgb_decode(A[k] + B[k][0] * x[0] + B[k][1] * x[1] + B[k][2] * x[2]);
}

int main(void) {
  static const float BASE[3] = {0.3f, 0.5f, 0.7f};
  ft_print_frame fr;
  ft_print_roll roll;
  ft_recipe r;
  ft_image im;
  const ft_image* one[1];
  ft_err e;
  int i, j, k, c;

  for(k = 0; k < NC; k++)
    for(j = 0; j < NC; j++)
      for(i = 0; i < NC; i++) {
        double x[3];
        float* n = nodes + 3 * ((k * NC + j) * NC + i);
        x[0] = (double)i / (NC - 1);
        x[1] = (double)j / (NC - 1);
        x[2] = (double)k / (NC - 1);
        for(c = 0; c < 3; c++)
          n[c] =
            (float)(A[c] + B[c][0] * x[0] + B[c][1] * x[1] + B[c][2] * x[2]);
      }
  ft_image_init(&im);
  CHECK(ft_image_alloc(&im, 64, 32, &e) == 0);
  for(i = 0; i < 64 * 32; i++)
    for(c = 0; c < 3; c++)
      im.rgb[3 * i + c] =
        (float)pow(10, -(BASE[c] + 0.02 * (i % 64) + 0.1 * c));
  one[0] = &im;
  CHECK(ft_print_roll_measure(&LUT, BASE, one, 1, &fr, &roll, &e) == 0);
  CHECK(ft_print_recipe(&LUT, &roll, &fr, (ft_cmyd){0, 0, 0, 0}, &r, NULL, NULL,
                        &e) == 0);
  CHECK(ft_print_compile(&LUT, &r, &pr, &e) == 0);
  CHECK(pr.lut == 1);

  /* any transmittance: the compiled path matches the chain by hand */
  {
    double worst = 0;
    for(i = 0; i < 2000; i++) {
      float t[3], o[3];
      double want[3];
      for(c = 0; c < 3; c++)
        t[c] = (float)pow(
          10, -(BASE[c] - 0.1 + 2.2 * ((i * (c + 3)) % 997) / 997.0));
      ft_print_apply(&pr, t, o, 1);
      expect(&r, t, want);
      for(c = 0; c < 3; c++)
        if(fabs(o[c] - want[c]) > worst) worst = fabs(o[c] - want[c]);
    }
    CHECK(worst < 2e-4);
  }

  /* +D: less exposure on the print, so lower e: a darker print */
  {
    float t[3] = {0.1f, 0.1f, 0.1f}, o0[3], o1[3];
    ft_print_apply(&pr, t, o0, 1);
    CHECK(ft_print_recipe(&LUT, &roll, &fr, (ft_cmyd){0, 0, 0, 8}, &r, NULL,
                          NULL, &e) == 0);
    CHECK(ft_print_compile(&LUT, &r, &pr, &e) == 0);
    ft_print_apply(&pr, t, o1, 1);
    for(c = 0; c < 3; c++) CHECK(o1[c] < o0[c]);
  }

  /* in place */
  {
    float buf[6] = {0.2f, 0.1f, 0.05f, 0.5f, 0.3f, 0.2f}, o[6];
    ft_print_apply(&pr, buf, o, 2);
    ft_print_apply(&pr, buf, buf, 2);
    CHECK(memcmp(buf, o, sizeof o) == 0);
  }

  /* refusals */
  {
    ft_recipe rb;
    ft_pm_lut3d_par bad = {&CUBE, 1.0f, 1.0f};
    ft_recipe_default(&rb, FT_RENDER_BW);
    CHECK(ft_print_compile(&LUT_BW, &rb, &pr, &e) == -1);
    CHECK(strcmp(e.msg, "lut3d: colour negatives only") == 0);
    CHECK(ft_pm_print_lut3d.compile(&bad, &r, &pr, &e) == -1);
    CHECK(strcmp(e.msg, "lut3d: bad table") == 0);
    r.grade = 99;
    CHECK(ft_print_compile(&LUT, &r, &pr, &e) == -1);
    CHECK(strncmp(e.msg, "recipe: grade", 13) == 0);
  }

  ft_image_free(&im);
  return TEST_DONE();
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdint.h>
#include <string.h>

#include "ft_pm.h"
#include "ft_print.h"
#include "ft_render_auto.h"
#include "test/ftest.h"

static ft_render ref;
static ft_print_render pr;

static uint32_t rng = 7;
static double urand(void) {
  rng = rng * 1664525u + 1013904223u;
  return ((rng >> 8) + 0.5) / 16777216.0;
}
static double gauss(void) {
  return sqrt(-2 * log(urand())) * cos(6.283185307179586 * urand());
}

enum { W = 480, H = 120, STEPS = 12 };
static const float BASE[3] = {0.25f, 0.6f, 0.9f};
static const double GAMMA[3] = {0.55, 0.62, 0.70};

/* a colour negative of a grey ramp (12 steps over two decades of
 * exposure, the brightest at scale) with scanner-like noise */
static void negative(ft_image* im, double scale) {
  int x, y, c;
  for(y = 0; y < H; y++)
    for(x = 0; x < W; x++) {
      int s = x * STEPS / W;
      double ex = scale * pow(10, -2 + 2.0 * s / (STEPS - 1));
      for(c = 0; c < 3; c++) {
        double d = BASE[c] + GAMMA[c] * log10(ex / 0.002);
        double t = pow(10, -d), sig = sqrt(0.0005 * 0.0005 + t / 6000);
        im->pixels[3 * (y * W + x) + c] = (float)(t + sig * gauss());
      }
    }
}

static const ft_emulsion CLASSIC = {"test-classic",
                                    "Test classic",
                                    FT_RENDER_NEGATIVE,
                                    &ft_film_module_classic,
                                    NULL,
                                    &ft_balance_module_classic,
                                    NULL,
                                    &ft_print_module_classic,
                                    NULL,
                                    &ft_output_module_classic,
                                    NULL};
static const ft_emulsion CLASSIC_SLIDE = {"test-slide",
                                          "Test slide",
                                          FT_RENDER_SLIDE,
                                          &ft_film_module_classic,
                                          NULL,
                                          &ft_balance_module_classic,
                                          NULL,
                                          &ft_print_module_classic,
                                          NULL,
                                          &ft_output_module_classic,
                                          NULL};
static const ft_stock_film_parameters STOCK_ROLL = {1.2f, 0, {1, 1, 1}};
static const ft_stock_film_parameters STOCK_FIXED = {1.2f, 1, {1.1f, 1, 0.9f}};
static const ft_emulsion STOCK = {"test-stock",
                                  "Test stock",
                                  FT_RENDER_NEGATIVE,
                                  &ft_film_module_stock,
                                  &STOCK_ROLL,
                                  &ft_balance_module_classic,
                                  NULL,
                                  &ft_print_module_classic,
                                  NULL,
                                  &ft_output_module_classic,
                                  NULL};
static const ft_emulsion STOCK_FIX = {"test-fixed",
                                      "Test fixed",
                                      FT_RENDER_NEGATIVE,
                                      &ft_film_module_stock,
                                      &STOCK_FIXED,
                                      &ft_balance_module_classic,
                                      NULL,
                                      &ft_print_module_classic,
                                      NULL,
                                      &ft_output_module_classic,
                                      NULL};
static const ft_emulsion BW = {"test-bw",    "Test b&w",
                               FT_RENDER_BW, &ft_film_module_stock,
                               &STOCK_ROLL,  &ft_balance_module_classic,
                               NULL,         &ft_print_module_classic,
                               NULL,         &ft_output_module_classic,
                               NULL};

static double lift(const ft_recipe* r, int c) {
  return r->exposure * log10(2) + r->tint[c] / 100.0;
}

static void one_frame(const ft_emulsion* em, const ft_image* im, ft_cmyd k,
                      ft_recipe* r) {
  const ft_image* one[1];
  ft_print_frame fr;
  ft_print_roll roll;
  ft_err e;
  one[0] = im;
  CHECK(ft_measure_print_roll(em, BASE, one, 1, &fr, &roll, &e) == 0);
  CHECK(ft_make_print_recipe(em, &roll, &fr, k, r, NULL, NULL, &e) == 0);
}

int main(void) {
  static const ft_cmyd NONE = {0, 0, 0, 0};
  const ft_image* one[1];
  ft_image im, under, out1, out2;
  ft_recipe r, a;
  ft_err e;
  int c;

  ft_init_image(&im);
  ft_init_image(&under);
  ft_init_image(&out1);
  ft_init_image(&out2);
  CHECK(ft_allocate_image(&im, W, H, &e) == 0);
  CHECK(ft_allocate_image(&under, W, H, &e) == 0);
  CHECK(ft_allocate_image(&out1, W, H, &e) == 0);
  CHECK(ft_allocate_image(&out2, W, H, &e) == 0);
  negative(&im, 1);
  negative(&under, 0.1); /* 3.3 stops under: thin highlights */
  one[0] = &im;

  /* the classic chain is ftdev export's recipe, bit for bit */
  ft_init_recipe(&a, FT_RENDER_NEGATIVE);
  ft_set_recipe_base(&a, BASE);
  CHECK(ft_estimate_recipe(one, 1, &a, &e) == 0);
  one_frame(&CLASSIC, &im, NONE, &r);
  CHECK(memcmp(&r, &a, sizeof r) == 0);
  CHECK(ft_compile_render(&a, &ref, &e) == 0);
  CHECK(ft_compile_print(&CLASSIC, &r, &pr, &e) == 0);
  CHECK(pr.is_lut == 0);
  ft_apply_render(&ref, im.pixels, out1.pixels, (size_t)W * H);
  ft_apply_print(&pr, im.pixels, out2.pixels, (size_t)W * H);
  CHECK(memcmp(out1.pixels, out2.pixels, sizeof(float) * 3 * W * H) == 0);
  /* and for a slide: black from the base, no exposure, no tint */
  {
    static const float BLACK[3] = {3.0f, 3.1f, 3.2f};
    ft_print_frame fr;
    ft_print_roll roll;
    ft_init_recipe(&a, FT_RENDER_SLIDE);
    ft_set_recipe_base(&a, BLACK);
    CHECK(ft_estimate_recipe(one, 1, &a, &e) == 0);
    CHECK(ft_measure_print_roll(&CLASSIC_SLIDE, BLACK, one, 1, &fr, &roll,
                                &e) == 0);
    CHECK(ft_make_print_recipe(&CLASSIC_SLIDE, &roll, &fr, NONE, &r, NULL, NULL,
                               &e) == 0);
    CHECK(memcmp(&r, &a, sizeof r) == 0);
  }

  /* clicks: 0.025 D each at the filtration point */
  {
    ft_cmyd cmy = {1, 1, 1, 0}, d1 = {0, 0, 0, 1}, d12 = {0, 0, 0, 12},
            cc = {1, 0, 0, 0}, mm = {0, 1, 0, 0}, yy = {0, 0, 1, 0};
    ft_recipe r0, r1, r2;
    one_frame(&CLASSIC, &im, NONE, &r0);
    one_frame(&CLASSIC, &im, cmy, &r1);
    one_frame(&CLASSIC, &im, d1, &r2);
    for(c = 0; c < 3; c++) { /* +1 C +1 M +1 Y == +1 D */
      CHECK_NEAR(lift(&r1, c), lift(&r2, c), 1e-6);
      CHECK_NEAR(lift(&r2, c) - lift(&r0, c), -FT_PRINT_CLICK_DENSITY, 1e-6);
    }
    CHECK(FT_PRINT_CLICK_DENSITY == 0.025);
    one_frame(&CLASSIC, &im, d12, &r1); /* 12 clicks: 0.3 D, a stop */
    CHECK_NEAR(lift(&r1, 1) - lift(&r0, 1), -0.3, 1e-6);
    CHECK_NEAR(r1.exposure - r0.exposure, -1, 0.004);
    one_frame(&CLASSIC, &im, cc, &r1); /* +C: less red, 2.5 CC */
    CHECK_NEAR(r1.tint[0] - r0.tint[0], -2.5, 1e-6);
    CHECK(r1.tint[1] == r0.tint[1] && r1.tint[2] == r0.tint[2]);
    CHECK(r1.exposure == r0.exposure);
    one_frame(&CLASSIC, &im, mm, &r1);
    CHECK_NEAR(r1.tint[1] - r0.tint[1], -2.5, 1e-6);
    one_frame(&CLASSIC, &im, yy, &r1);
    CHECK_NEAR(r1.tint[2] - r0.tint[2], -2.5, 1e-6);
    /* on the print: +C takes red out of a mid grey, +D darkens it */
    {
      float t[3], o0[3], o1[3];
      for(c = 0; c < 3; c++) t[c] = (float)pow(10, -(BASE[c] + GAMMA[c] * 1.4));
      ft_render_pixel_directly(&r0, t, o0);
      one_frame(&CLASSIC, &im, (ft_cmyd){4, 0, 0, 0}, &r1);
      ft_render_pixel_directly(&r1, t, o1);
      CHECK(o1[0] < o0[0] - 0.01);
      one_frame(&CLASSIC, &im, (ft_cmyd){0, 0, 0, 4}, &r1);
      ft_render_pixel_directly(&r1, t, o1);
      for(c = 0; c < 3; c++) CHECK(o1[c] < o0[c] - 0.01);
    }
  }

  /* click limits and b&w */
  {
    ft_print_frame fr;
    ft_print_roll roll;
    bool clamped = true;
    CHECK(ft_measure_print_roll(&CLASSIC, BASE, one, 1, &fr, &roll, &e) == 0);
    CHECK(ft_make_print_recipe(&CLASSIC, &roll, &fr, (ft_cmyd){100, 0, 0, 0},
                               &r, NULL, NULL, &e) == -1);
    CHECK(strcmp(e.message, "cmyd: C 100 is out of range (-99..99)") == 0);
    CHECK(ft_check_print_clicks(&CLASSIC, (ft_cmyd){0, 0, 0, -100}, &e) == -1);
    CHECK(strcmp(e.message, "cmyd: D -100 is out of range (-99..99)") == 0);
    CHECK(ft_check_print_clicks(&BW, (ft_cmyd){0, 1, 0, 0}, &e) == -1);
    CHECK(strcmp(e.message,
                 "cmyd: test-bw is black and white: C, M and Y "
                 "must be 0") == 0);
    CHECK(ft_check_print_clicks(&BW, (ft_cmyd){0, 0, 0, -99}, &e) == 0);
    /* beyond the recipe's +-50 CC: clamped and reported */
    CHECK(ft_make_print_recipe(&CLASSIC, &roll, &fr, (ft_cmyd){30, 0, 0, 0}, &r,
                               NULL, &clamped, &e) == 0);
    CHECK(clamped == true && r.tint[0] == -50);
    CHECK(ft_validate_recipe(&r, &e) == 0);
    CHECK(ft_make_print_recipe(&CLASSIC, &roll, &fr, (ft_cmyd){20, 0, 0, 0}, &r,
                               NULL, &clamped, &e) == 0);
    CHECK(clamped == false && r.tint[0] == -50);
  }

  /* stock: one range for the roll, no per-frame stretch */
  {
    const ft_image* roll2[2];
    ft_print_frame fr[2];
    ft_print_roll roll;
    ft_recipe r0, r1;
    roll2[0] = &im;
    roll2[1] = &under;
    /* classic: the thin frame is stretched to its own, smaller range */
    CHECK(ft_measure_print_roll(&CLASSIC, BASE, roll2, 2, fr, &roll, &e) == 0);
    CHECK(ft_make_print_recipe(&CLASSIC, &roll, &fr[0], NONE, &r0, NULL, NULL,
                               &e) == 0);
    CHECK(ft_make_print_recipe(&CLASSIC, &roll, &fr[1], NONE, &r1, NULL, NULL,
                               &e) == 0);
    CHECK(r1.range_density[1] < 0.7 * r0.range_density[1]);
    /* stock: both frames at scale x the roll's median ratio */
    CHECK(ft_measure_print_roll(&STOCK, BASE, roll2, 2, fr, &roll, &e) == 0);
    CHECK(roll.has_range);
    CHECK(ft_make_print_recipe(&STOCK, &roll, &fr[0], NONE, &r0, NULL, NULL,
                               &e) == 0);
    CHECK(ft_make_print_recipe(&STOCK, &roll, &fr[1], NONE, &r1, NULL, NULL,
                               &e) == 0);
    CHECK(memcmp(r0.range_density, r1.range_density, sizeof r0.range_density) ==
          0);
    CHECK_NEAR(r0.range_density[1], 1.2, 1e-6);
    for(c = 0; c < 3; c += 2) /* the dye gammas' ratio */
      CHECK_NEAR(r0.range_density[c] / r0.range_density[1], GAMMA[c] / GAMMA[1],
                 0.05);
    /* a fixed ratio: exactly scale x ratio */
    CHECK(ft_measure_print_roll(&STOCK_FIX, BASE, roll2, 2, fr, &roll, &e) ==
          0);
    CHECK(ft_make_print_recipe(&STOCK_FIX, &roll, &fr[1], NONE, &r1, NULL, NULL,
                               &e) == 0);
    CHECK(r1.range_density[0] == 1.2f * 1.1f &&
          r1.range_density[1] == 1.2f &&
          r1.range_density[2] == 1.2f * 0.9f);
    /* b&w: luminance base, one range, tint 0 */
    CHECK(ft_measure_print_roll(&BW, BASE, roll2, 2, fr, &roll, &e) == 0);
    CHECK(roll.base_density[0] == roll.base_density[1] &&
          roll.base_density[1] == roll.base_density[2]);
    CHECK(ft_make_print_recipe(&BW, &roll, &fr[0], (ft_cmyd){0, 0, 0, 3}, &r1,
                               NULL, NULL, &e) == 0);
    CHECK(r1.range_density[0] == 1.2f && r1.range_density[2] == 1.2f);
    CHECK(r1.tint[0] == 0 && r1.tint[1] == 0 && r1.tint[2] == 0);
    CHECK(ft_validate_recipe(&r1, &e) == 0);
  }

  /* roll size limits */
  {
    ft_print_frame fr[1];
    ft_print_roll roll;
    CHECK(ft_measure_print_roll(&CLASSIC, BASE, one, 0, fr, &roll, &e) == -1);
    CHECK(strcmp(e.message, "print: 0 frames (1..64)") == 0);
  }

  /* the stats proxy: 512 px on the long side, never upscaled */
  {
    uint32_t pw, ph;
    ft_get_print_proxy_size(6000, 4000, &pw, &ph);
    CHECK(pw == 512 && ph == 341);
    ft_get_print_proxy_size(3000, 4500, &pw, &ph);
    CHECK(pw == 341 && ph == 512);
    ft_get_print_proxy_size(400, 300, &pw, &ph);
    CHECK(pw == 400 && ph == 300);
  }

  /* the median helper */
  {
    double v[4] = {3, 1, 2, 10};
    CHECK(ft_get_print_median(v, 3) == 2);
    v[0] = 3;
    v[1] = 1;
    v[2] = 2;
    CHECK(ft_get_print_median(v, 4) == 2.5);
  }

  ft_free_image(&im);
  ft_free_image(&under);
  ft_free_image(&out1);
  ft_free_image(&out2);
  return TEST_DONE();
}

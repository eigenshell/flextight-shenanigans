/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include "ft_pm.h"
#include "ft_print.h"
#include "test/ftest.h"

enum { W = 400, H = 100, N = 8 };
static const float ZERO[3] = {0, 0, 0};

static const ft_stock_film_parameters FLAT = {1, 1, {1, 1, 1}};
static const ft_frontier_parameters FP = {0.7, 0.9,       0.6, 0.3,
                                          0.2, {0, 0, 0}, 6};
static const ft_emulsion NEG = {"test-neg",
                                "Test",
                                FT_RENDER_NEGATIVE,
                                &ft_film_module_stock,
                                &FLAT,
                                &ft_balance_module_frontier,
                                &FP,
                                &ft_print_module_classic,
                                NULL,
                                &ft_output_module_classic,
                                NULL};
static const ft_emulsion BW = {"test-bw",    "Test b&w",
                               FT_RENDER_BW, &ft_film_module_stock,
                               &FLAT,        &ft_balance_module_frontier,
                               &FP,          &ft_print_module_classic,
                               NULL,         &ft_output_module_classic,
                               NULL};
static const ft_emulsion SLIDE = {"test-slide",
                                  "Test slide",
                                  FT_RENDER_SLIDE,
                                  &ft_film_module_classic,
                                  NULL,
                                  &ft_balance_module_frontier,
                                  &FP,
                                  &ft_print_module_classic,
                                  NULL,
                                  &ft_output_module_classic,
                                  NULL};

/* A frame straight in film density (base 0, range 1: d = D): columns 8 px
 * wide alternating mean -+ 0.2 (exact means over the measured blocks),
 * plus a cast; x < sat_x also gets +0.6 D of green (a saturated subject) */
static void frame(ft_image* im, double mean, const double cast[3], int sat_x) {
  int x, y, c;
  for(y = 0; y < H; y++)
    for(x = 0; x < W; x++)
      for(c = 0; c < 3; c++) {
        double d = mean +
                   ((x / 8) % 2 ? 0.2 : -0.2) +
                   cast[c] +
                   (x < sat_x && c == 1 ? 0.6 : 0);
        im->pixels[3 * (y * W + x) + c] = (float)pow(10, -d);
      }
}

static double lift(const ft_recipe* r, int c) {
  return r->exposure * log10(2) + r->tint[c] / 100.0;
}

/* the print exposure of the frame's mean, per channel */
static void printed(const ft_print_frame* fr, const ft_recipe* r, double e[3]) {
  int c;
  for(c = 0; c < 3; c++) e[c] = fr->mean_density[c] + lift(r, c);
}

static double avg3(const double v[3]) { return (v[0] + v[1] + v[2]) / 3; }

static ft_image ims[N + 1];
static const ft_image* ptr[N + 1];

int main(void) {
  static const double CAST[3] = {0.12, 0, 0};
  static const double MEANS[N] = {0.5, 0.6, 0.7, 0.8, 0.9, 1.0, 1.1, 1.2};
  static const ft_cmyd NONE = {0, 0, 0, 0};
  ft_print_frame fr[N + 1];
  ft_print_roll roll;
  ft_recipe r;
  ft_err e;
  int i, c;

  for(i = 0; i <= N; i++) {
    ft_init_image(&ims[i]);
    CHECK(ft_allocate_image(&ims[i], W, H, &e) == 0);
    ptr[i] = &ims[i];
  }
  for(i = 0; i < N; i++) frame(&ims[i], MEANS[i], CAST, 0);

  /* a roll with one cast: removed in full; density by k_d */
  CHECK(ft_measure_print_roll(&NEG, ZERO, ptr, N, fr, &roll, &e) == 0);
  CHECK(roll.has_reference);
  CHECK_NEAR(roll.reference_density, (0.8 + 0.9) / 2 + 0.04, 1e-4);
  CHECK_NEAR(roll.reference_colour[0], 0.08, 1e-4);
  CHECK_NEAR(roll.reference_colour[1], -0.04, 1e-4);
  for(i = 0; i < N; i++) {
    double ev[3];
    CHECK(fr[i].has_mean);
    CHECK_NEAR(fr[i].mean_density[0], MEANS[i] + 0.12, 1e-4);
    CHECK_NEAR(fr[i].mean_density[1], MEANS[i], 1e-4);
    CHECK(ft_make_print_recipe(&NEG, &roll, &fr[i], NONE, &r, NULL, NULL, &e) ==
          0);
    printed(&fr[i], &r, ev);
    for(c = 0; c < 3; c++) /* neutral */
      CHECK_NEAR(ev[c], avg3(ev), 1e-4);
    /* the roll prints at the aim, a frame off by (1 - k_d) of its
     * deviation from the roll */
    CHECK_NEAR(
      avg3(ev),
      0.7 + (1 - 0.9) * (avg3(fr[i].mean_density) - roll.reference_density),
      1e-4);
  }

  /* one frame with its own blue cast: k_c of it removed */
  {
    static const double BLUE[3] = {0.12, 0, 0.1};
    double ev[3], dev[3], m;
    frame(&ims[N], 0.8, BLUE, 0);
    CHECK(ft_measure_print_roll(&NEG, ZERO, ptr, N + 1, fr, &roll, &e) == 0);
    CHECK(ft_make_print_recipe(&NEG, &roll, &fr[N], NONE, &r, NULL, NULL, &e) ==
          0);
    printed(&fr[N], &r, ev);
    m = avg3(fr[N].mean_density);
    for(c = 0; c < 3; c++)
      dev[c] = (fr[N].mean_density[c] - m) - roll.reference_colour[c];
    for(c = 0; c < 3; c++)
      CHECK_NEAR(ev[c] - avg3(ev), (1 - 0.6) * (dev[c] - avg3(dev)), 1e-4);
    CHECK(ev[2] > avg3(ev) + 0.02); /* bluer film stays */
  }

  /* a cast beyond the clamp: the frame's own correction is scaled down
   * as a whole until its largest channel moves 0.3 D (12 clicks) */
  {
    static const double BIG[3] = {0.12, 0, 1.2};
    ft_print_auto a0, a1;
    frame(&ims[N], 0.8, BIG, 0);
    CHECK(ft_measure_print_roll(&NEG, ZERO, ptr, N + 1, fr, &roll, &e) == 0);
    CHECK(ft_make_print_recipe(&NEG, &roll, &fr[0], NONE, &r, &a0, NULL, &e) ==
          0);
    CHECK(ft_make_print_recipe(&NEG, &roll, &fr[N], NONE, &r, &a1, NULL, &e) ==
          0);
    /* deviation -0.4 -0.4 +0.8 D, x 0.6 = -0.24 -0.24 +0.48: scaled
     * by 0.3 / 0.48 */
    CHECK_NEAR(a1.tint[2] - a0.tint[2], -30, 0.01);
    CHECK_NEAR(a1.tint[0] - a0.tint[0], 15, 0.01);
    CHECK_NEAR(a1.tint[1] - a0.tint[1], 15, 0.01);
  }

  /* a saturated subject (80 % strong green) is left out of the means:
   * the frame gets the roll's colour, not a correction for the green */
  {
    ft_print_auto a0, a1;
    frame(&ims[N], 0.8, CAST, 320);
    CHECK(ft_measure_print_roll(&NEG, ZERO, ptr, N + 1, fr, &roll, &e) == 0);
    CHECK(ft_make_print_recipe(&NEG, &roll, &fr[0], NONE, &r, &a0, NULL, &e) ==
          0);
    CHECK(ft_make_print_recipe(&NEG, &roll, &fr[N], NONE, &r, &a1, NULL, &e) ==
          0);
    for(c = 0; c < 3; c++) CHECK_NEAR(a1.tint[c], a0.tint[c], 1.0);
    CHECK(fabs(a1.tint[1] - a0.tint[1]) < 3); /* << the 30 CC clamp */
  }

  /* a short roll: the emulsion's colour aim, not the roll median */
  {
    double ev[3], ch[3];
    CHECK(ft_measure_print_roll(&NEG, ZERO, ptr, 3, fr, &roll, &e) == 0);
    for(c = 0; c < 3; c++) CHECK(roll.reference_colour[c] == 0);
    CHECK(ft_make_print_recipe(&NEG, &roll, &fr[1], NONE, &r, NULL, NULL, &e) ==
          0);
    printed(&fr[1], &r, ev);
    for(c = 0; c < 3; c++)
      ch[c] = fr[1].mean_density[c] - avg3(fr[1].mean_density);
    for(c = 0; c < 3; c++) /* k_c of the cast only */
      CHECK_NEAR(ev[c] - avg3(ev), (1 - 0.6) * ch[c], 1e-4);
  }

  /* b&w: density only */
  {
    ft_print_auto a;
    double m;
    CHECK(ft_measure_print_roll(&BW, ZERO, ptr, N, fr, &roll, &e) == 0);
    CHECK(ft_make_print_recipe(&BW, &roll, &fr[2], NONE, &r, &a, NULL, &e) ==
          0);
    CHECK(a.tint[0] == 0 && a.tint[1] == 0 && a.tint[2] == 0);
    m = fr[2].mean_density[1];
    CHECK_NEAR(
      r.exposure * log10(2),
      0.7 - roll.reference_density + 0.9 * (roll.reference_density - m), 1e-5);
  }

  /* refusals */
  CHECK(ft_measure_print_roll(&SLIDE, ZERO, ptr, 1, fr, &roll, &e) == -1);
  CHECK(strcmp(e.message, "frontier: colour negatives and b&w only") == 0);
  {
    ft_recipe rr;
    size_t k;
    for(k = 0; k < (size_t)W * H * 3; k++)
      ims[N].pixels[k] = 1; /* open beam everywhere */
    ft_init_recipe(&rr, FT_RENDER_NEGATIVE);
    CHECK(ft_balance_module_frontier.measure(&FP, &rr, &ims[N], &fr[0], &e) ==
          -1);
    CHECK(strcmp(e.message, "frontier: no film pixels to measure") == 0);
  }

  for(i = 0; i <= N; i++) ft_free_image(&ims[i]);
  return TEST_DONE();
}

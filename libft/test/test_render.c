/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdint.h>
#include <string.h>

#include "ft_out.h"
#include "ft_render.h"
#include "test/ftest.h"

static ft_render cr; /* ~380 KB: static, not on the stack */

static uint32_t rng = 12345;
static double urand(void) {
  rng = rng * 1664525u + 1013904223u;
  return (rng >> 8) / 16777216.0;
}

static uint32_t fbits(float f) {
  uint32_t b;
  memcpy(&b, &f, sizeof b);
  return b;
}

/* worst |encode(lut) - encode(direct)| over n log-uniform pixels */
static double lut_error(const ft_recipe* r, int n) {
  double worst = 0;
  ft_err e;
  int i, c;
  CHECK(ft_compile_render(r, &cr, &e) == 0);
  for(i = 0; i < n; i++) {
    float t[3], a[3], b[3];
    for(c = 0; c < 3; c++) t[c] = (float)pow(10, -4.5 + urand() * 4.6);
    ft_apply_render(&cr, t, a, 1);
    ft_render_pixel_directly(r, t, b);
    for(c = 0; c < 3; c++) {
      double d = fabs(ft_encode_srgb(a[c]) - ft_encode_srgb(b[c]));
      if(d > worst) worst = d;
    }
  }
  {
    static const float odd[] = {0, -1, 1e-6f, 1.26f, 1.3f, 2, (float)INFINITY};
    for(i = 0; i < (int)(sizeof odd / sizeof *odd); i++) {
      float t[3], a[3], b[3];
      for(c = 0; c < 3; c++) t[c] = odd[i];
      ft_apply_render(&cr, t, a, 1);
      ft_render_pixel_directly(r, t, b);
      for(c = 0; c < 3; c++) {
        double d = fabs(ft_encode_srgb(a[c]) - ft_encode_srgb(b[c]));
        if(d > worst) worst = d;
      }
    }
  }
  return worst;
}

/* the direct output for one grey-ish pixel, channel c */
static float px(const ft_recipe* r, float t0, float t1, float t2, int c) {
  float t[3], o[3];
  t[0] = t0;
  t[1] = t1;
  t[2] = t2;
  ft_render_pixel_directly(r, t, o);
  return o[c];
}

int main(void) {
  static const float ID[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  ft_recipe r, q;
  ft_err e;
  int i, c;

  /* LUT layout constants match the float bit patterns */
  CHECK(fbits((float)FT_TRANSMITTANCE_FLOOR) >> FT_RENDER_INPUT_SHIFT ==
        FT_RENDER_INPUT_FIRST_KEY);
  CHECK((fbits(FT_RENDER_MAX_TRANSMITTANCE) >> FT_RENDER_INPUT_SHIFT) -
          FT_RENDER_INPUT_FIRST_KEY +
          2 ==
        FT_RENDER_INPUT_SIZE);

  /* defaults validate; each bad field is named */
  for(c = FT_RENDER_NEGATIVE; c <= FT_RENDER_BW; c++) {
    ft_init_recipe(&r, (ft_render_mode)c);
    CHECK(ft_validate_recipe(&r, &e) == 0);
  }
  ft_init_recipe(&r, FT_RENDER_NEGATIVE);
  q = r;
  q.base_density[1] = 5;
  CHECK(ft_validate_recipe(&q, &e) == -1 && strstr(e.message, "base g"));
  q = r;
  q.range_density[2] = 0.01f;
  CHECK(ft_validate_recipe(&q, &e) == -1 && strstr(e.message, "range b"));
  q = r;
  q.tint[0] = 51;
  CHECK(ft_validate_recipe(&q, &e) == -1 && strstr(e.message, "tint r"));
  q = r;
  q.exposure = 6.5f;
  CHECK(ft_validate_recipe(&q, &e) == -1 && strstr(e.message, "exposure"));
  q = r;
  q.grade = 0.1f;
  CHECK(ft_validate_recipe(&q, &e) == -1 && strstr(e.message, "grade"));
  q = r;
  q.paper_black = 0;
  CHECK(ft_validate_recipe(&q, &e) == -1 && strstr(e.message, "paper"));
  q = r;
  q.bw_weights[0] = 0.5f;
  CHECK(ft_validate_recipe(&q, &e) == -1 && strstr(e.message, "weights"));
  q = r;
  q.matrix[4] = (float)NAN;
  CHECK(ft_validate_recipe(&q, &e) == -1 && strstr(e.message, "matrix"));
  q = r;
  q.exposure = (float)NAN;
  CHECK(ft_validate_recipe(&q, &e) == -1);
  q = r;
  q.channels[1].point_count = 1;
  CHECK(ft_validate_recipe(&q, &e) == -1 && strstr(e.message, "green curve"));
  q = r;
  q.mode = (ft_render_mode)7;
  CHECK(ft_validate_recipe(&q, &e) == -1);
  CHECK(ft_compile_render(&q, &cr, &e) == -1); /* compile validates */
  ft_init_recipe(&q, FT_RENDER_BW);
  q.tint[1] = 5;
  CHECK(ft_validate_recipe(&q, &e) == -1 && strstr(e.message, "b&w"));
  ft_init_recipe(&q, FT_RENDER_SLIDE);
  q.base_density[0] = 0.05f;
  CHECK(ft_validate_recipe(&q, &e) == -1 && strstr(e.message, "slide"));

  /* compiled LUTs match the direct maths within 2/65535, encoded */
  {
    ft_recipe s[6];
    ft_init_recipe(&s[0], FT_RENDER_NEGATIVE);
    s[0].base_density[0] = 0.25f;
    s[0].base_density[1] = 0.6f;
    s[0].base_density[2] = 0.9f;
    s[0].range_density[0] = 0.9f;
    s[0].range_density[2] = 1.1f;
    s[0].exposure = 0.5f;
    s[1] = s[0];
    s[1].exposure = -2;
    s[1].tint[0] = 50;
    s[1].tint[1] = -50;
    s[1].tint[2] = 30;
    CHECK(ft_find_curve_preset("strong", &s[1].master) == 0);
    CHECK(ft_parse_curve("0,0 0.5,0.4 1,1", &s[1].channels[2], &e) == 0);
    s[2] = s[0];
    s[2].range_density[0] = s[2].range_density[1] = s[2].range_density[2] =
      0.3f;
    s[2].grade = 5;
    s[2].paper_black = 4;
    CHECK(ft_find_curve_preset("fade", &s[2].master) == 0);
    ft_init_recipe(&s[3], FT_RENDER_SLIDE);
    s[3].base_density[0] = 3.2f;
    s[3].base_density[1] = 3.0f;
    s[3].base_density[2] = 2.9f;
    s[3].exposure = 1;
    s[3].tint[2] = -20;
    s[4] = s[3];
    CHECK(ft_find_curve_preset("soft", &s[4].master) == 0);
    ft_init_recipe(&s[5], FT_RENDER_BW);
    s[5].base_density[0] = s[5].base_density[1] = s[5].base_density[2] = 0.2f;
    s[5].range_density[1] = 1.2f;
    s[5].grade = 2.5f;
    CHECK(ft_find_curve_preset("medium", &s[5].master) == 0);
    for(i = 0; i < 6; i++) CHECK_NEAR(lut_error(&s[i], 100000), 0, 2.0 / 65535);
  }

  /* negative semantics, at a mid-grey pixel (half the range above base) */
  ft_init_recipe(&r, FT_RENDER_NEGATIVE);
  r.base_density[0] = 0.25f;
  r.base_density[1] = 0.6f;
  r.base_density[2] = 0.9f;
  {
    float g0 = (float)pow(10, -(0.25 + 0.5)), g1 = (float)pow(10, -(0.6 + 0.5)),
          g2 = (float)pow(10, -(0.9 + 0.5)), a, b;
    /* neutral grey stays neutral (matrix rows sum to 1) */
    CHECK_NEAR(px(&r, g0, g1, g2, 0), px(&r, g0, g1, g2, 1), 1e-5);
    CHECK_NEAR(px(&r, g0, g1, g2, 2), px(&r, g0, g1, g2, 1), 1e-5);
    /* pinned by hand: mid grey is u = 0, s = 0.5, P = 1.0 -> 0.1 */
    CHECK_NEAR(px(&r, g0, g1, g2, 1), 0.1, 1e-6);
    /* film base: n = 0, u = 0.5, s = 1/(1+exp(-3)), P = 2 s */
    CHECK_NEAR(px(&r, (float)pow(10, -0.25), (float)pow(10, -0.6),
                  (float)pow(10, -0.9), 1),
               pow(10, -2 / (1 + exp(-3))), 1e-6);
    /* + red tint = redder */
    q = r;
    q.tint[0] = 20;
    CHECK(px(&q, g0, g1, g2, 0) / px(&q, g0, g1, g2, 1) > 1.2);
    q = r;
    q.tint[2] = -20;
    CHECK(px(&q, g0, g1, g2, 2) < px(&q, g0, g1, g2, 1) * 0.85);
    /* equal tint on all three == exposure of tint/100/log10(2) stops */
    q = r;
    q.tint[0] = q.tint[1] = q.tint[2] = 15;
    a = px(&q, g0, g1, g2, 1);
    q = r;
    q.exposure = (float)(0.15 / log10(2));
    b = px(&q, g0, g1, g2, 1);
    CHECK_NEAR(a, b, 1e-6);
    /* + exposure = brighter */
    q = r;
    q.exposure = 1;
    CHECK(px(&q, g0, g1, g2, 1) > px(&r, g0, g1, g2, 1));
    /* the paper: from black (film base) to white (dense), monotone */
    {
      float prev = 2;
      for(i = 0; i <= 300; i++) {
        double k = i / 100.0; /* D above base */
        float v = px(&r, (float)pow(10, -(0.25 + k)),
                     (float)pow(10, -(0.6 + k)), (float)pow(10, -(0.9 + k)), 1);
        CHECK(v >= prev - 1e-7 || i == 0);
        if(i == 0) CHECK(v < 0.02); /* base: near black */
        prev = v;
      }
      CHECK(prev > 0.95); /* dense: near white */
    }
  }

  /* slide: black point -> 0, tint is an exact gain */
  ft_init_recipe(&r, FT_RENDER_SLIDE);
  memcpy(r.matrix, ID, sizeof ID);
  r.base_density[0] = r.base_density[1] = r.base_density[2] = 3;
  CHECK_NEAR(px(&r, 1e-3f, 1e-3f, 1e-3f, 0), 0, 1e-7);
  {
    float t = 0.2f, a = px(&r, t, t, t, 0);
    q = r;
    q.tint[0] = 20;
    CHECK_NEAR(px(&q, t, t, t, 0) / a, pow(10, 0.2), 1e-4);
    q = r;
    q.exposure = -1;
    CHECK_NEAR(px(&q, t, t, t, 1) / a, 0.5, 1e-4);
  }

  /* b&w: R = G = B; base folds to luminance density */
  ft_init_recipe(&r, FT_RENDER_BW);
  {
    float d[3] = {0.3f, 0.3f, 0.3f};
    ft_set_recipe_base(&r, d);
    CHECK_NEAR(r.base_density[0], 0.3, 1e-5);
    CHECK_NEAR(r.base_density[2], 0.3, 1e-5);
    {
      float nd[3] = {0.5f, (float)NAN, 0.7f};
      ft_set_recipe_base(&r, nd); /* non-finite: unchanged */
      CHECK_NEAR(r.base_density[0], 0.3, 1e-5);
      CHECK_NEAR(r.base_density[1], 0.3, 1e-5);
      CHECK_NEAR(r.base_density[2], 0.3, 1e-5);
    }
    CHECK(ft_compile_render(&r, &cr, &e) == 0);
    for(i = 0; i < 1000; i++) {
      float t[3], o[3];
      for(c = 0; c < 3; c++) t[c] = (float)pow(10, -urand() * 3);
      ft_apply_render(&cr, t, o, 1);
      CHECK(o[0] == o[1] && o[1] == o[2]);
    }
  }

  /* NaN and inf never reach the output; in place == out of place */
  ft_init_recipe(&r, FT_RENDER_NEGATIVE);
  CHECK(ft_compile_render(&r, &cr, &e) == 0);
  {
    float bad[9] = {
      (float)NAN, (float)NAN, (float)NAN, (float)INFINITY, -(float)INFINITY, -1,
      0,          2,          0.01f};
    float o[9],
      floor_px[3] = {(float)FT_TRANSMITTANCE_FLOOR,
                     (float)FT_TRANSMITTANCE_FLOOR,
                     (float)FT_TRANSMITTANCE_FLOOR},
      fo[3];
    ft_apply_render(&cr, bad, o, 3);
    for(i = 0; i < 9; i++) CHECK(isfinite(o[i]) && o[i] >= 0 && o[i] <= 1);
    ft_apply_render(&cr, floor_px, fo, 1);
    CHECK(o[0] == fo[0]); /* NaN -> floor */
    ft_apply_render(&cr, bad, bad, 3);
    for(i = 0; i < 9; i++) CHECK(bad[i] == o[i]);
  }

  /* whole image: in place, now linear Rec.2020 */
  {
    ft_image im;
    ft_init_image(&im);
    CHECK(ft_allocate_image(&im, 4, 2, &e) == 0);
    for(i = 0; i < 24; i++) im.pixels[i] = 0.1f;
    ft_render_image(&cr, &im);
    CHECK_NEAR(im.xyz_to_rgb[0], 1.7166511880, 1e-9);
    CHECK(im.pixels[0] > 0 && im.pixels[0] < 1);

    /* the positive writer: grey 0.18 (Rec.2020) -> sRGB 0.4614 */
    {
      const ft_writer* w = ft_find_writer("positive");
      unsigned char b[64];
      const char* path = "test/positive_out.png";
      FILE* f;
      CHECK(w != NULL && strcmp(w->extension, "_positive.png") == 0);
      for(i = 0; i < 24; i++) im.pixels[i] = 0.18f;
      CHECK(w->write(path, &im, &e) == 0);
      f = fopen(path, "rb");
      CHECK(f != NULL && fread(b, 1, sizeof b, f) == sizeof b);
      fclose(f);
      remove(path);
      CHECK(b[24] == 16); /* 16-bit */
      CHECK_NEAR(b[49] << 8 | b[50], ft_encode_srgb(0.18) * 65535, 1.5);
      CHECK((b[49] << 8 | b[50]) == (b[51] << 8 | b[52])); /* grey */
    }
    ft_free_image(&im);
  }
  return TEST_DONE();
}

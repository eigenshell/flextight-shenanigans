/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_emulsion.h"
#include "ft_pm.h"
#include "test/ftest.h"
#include <string.h>

static ft_print_render pr;

/* a plausible colour negative: a grey ramp through dye gammas 0.55..0.70
 * on an orange mask */
static void negative(ft_image *im, double scale) {
  static const double GAMMA[3] = {0.55, 0.62, 0.70};
  static const double MASK[3] = {0.25, 0.6, 0.9};
  int x, y, c;
  for(y = 0; y < im->h; y++)
    for(x = 0; x < im->w; x++)
      for(c = 0; c < 3; c++) {
        double ex = scale * pow(10, -2 + 2.0 * x / (im->w - 1));
        im->rgb[3 * (y * im->w + x) + c] =
            (float)pow(10, -(MASK[c] + GAMMA[c] * log10(ex / 0.002)));
      }
}

static int id_ok(const char *s) {
  if(!*s)
    return 0;
  for(; *s; s++)
    if(!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-'))
      return 0;
  return 1;
}

int main(void) {
  static const float MASK[3] = {0.25f, 0.6f, 0.9f};
  static const float BLACK[3] = {3.0f, 3.0f, 3.0f};
  const ft_emulsion *em, *g;
  ft_image im[3];
  const ft_image *ptr[3];
  ft_err e;
  int i, j, n;

  CHECK(FT_EMULSION_REV >= 1);
  for(n = 0; ft_emulsion_at(n); n++)
    ;
  CHECK(n >= 3);
  CHECK(ft_emulsion_at(-1) == NULL && ft_emulsion_at(n) == NULL);
  /* the v1 table */
  g = ft_emulsion_find("generic-neg");
  CHECK(g && g->mode == FT_RENDER_NEG);
  CHECK(strcmp(g->name, "Generic colour negative") == 0);
  CHECK(
      strcmp(g->film->name, "stock") == 0 &&
      strcmp(g->balance->name, "frontier") == 0 &&
      strcmp(g->print->name, "classic") == 0 &&
      strcmp(g->output->name, "classic") == 0
  );
  em = ft_emulsion_find("generic-slide");
  CHECK(em && em->mode == FT_RENDER_SLIDE);
  em = ft_emulsion_find("generic-bw");
  CHECK(em && em->mode == FT_RENDER_BW);
  CHECK(ft_emulsion_find("portra400") == NULL);
  CHECK(ft_emulsion_find("") == NULL);

  for(i = 0; i < 3; i++) {
    ft_image_init(&im[i]);
    CHECK(ft_image_alloc(&im[i], 200, 60, &e) == 0);
    negative(&im[i], i == 1 ? 0.3 : 1);
    ptr[i] = &im[i];
  }
  /* every entry: unique id, a name, and a chain that prints a roll */
  for(i = 0; i < n; i++) {
    ft_print_frame fr[3];
    ft_print_roll roll;
    ft_recipe r;
    float px[3] = {0.1f, 0.05f, 0.02f};
    em = ft_emulsion_at(i);
    CHECK(id_ok(em->id) && em->name && em->name[0]);
    for(j = 0; j < i; j++)
      CHECK(strcmp(ft_emulsion_at(j)->id, em->id) != 0);
    CHECK(em->film && em->balance && em->print && em->output);
    CHECK(
        ft_print_roll_measure(
            em, em->mode == FT_RENDER_SLIDE ? BLACK : MASK, ptr, 3, fr, &roll,
            &e
        ) == 0
    );
    CHECK(
        ft_print_recipe(
            em, &roll, &fr[1], (ft_cmyd){0, 0, 0, 1}, &r, NULL, NULL, &e
        ) == 0
    );
    CHECK(ft_print_compile(em, &r, &pr, &e) == 0);
    ft_print_apply(&pr, px, px, 1);
    for(j = 0; j < 3; j++)
      CHECK(px[j] >= 0 && px[j] <= 1);
  }
  /* generic-neg's aim: the paper prints e = aim as 18 % grey */
  {
    const ft_pm_frontier_par *p = g->balance_par;
    const ft_pm_stock_par *s = g->film_par;
    ft_recipe r;
    float t[3], o[3];
    ft_recipe_default(&r, FT_RENDER_NEG);
    for(i = 0; i < 3; i++) {
      r.range[i] = s->scale;
      t[i] = (float)pow(10, -p->aim);
    }
    ft_render_direct(&r, t, o);
    CHECK_NEAR(o[1], 0.18, 0.01);
  }
  for(i = 0; i < 3; i++)
    ft_image_free(&im[i]);
  return TEST_DONE();
}

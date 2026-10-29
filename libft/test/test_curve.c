/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <string.h>

#include "ft_curve.h"
#include "test/ftest.h"

int main(void) {
  ft_curve c;
  ft_err e;
  int i, k;

  /* identity and the line through two points */
  ft_init_identity_curve(&c);
  CHECK(ft_validate_curve(&c, &e) == 0);
  for(i = 0; i <= 10; i++)
    CHECK_NEAR(ft_evaluate_curve(&c, i / 10.0), i / 10.0, 1e-12);
  CHECK_NEAR(ft_evaluate_curve(&c, -0.5), 0, 0); /* clamped */
  CHECK_NEAR(ft_evaluate_curve(&c, 1.5), 1, 0);

  /* every preset validates, passes through its points, is monotone */
  for(k = 0; ft_get_curve_preset_name(k); k++) {
    double prev = -1;
    CHECK(ft_find_curve_preset(ft_get_curve_preset_name(k), &c) == 0);
    CHECK(ft_validate_curve(&c, &e) == 0);
    for(i = 0; i < c.point_count; i++)
      CHECK_NEAR(ft_evaluate_curve(&c, c.x[i]), c.y[i], 1e-6);
    for(i = 0; i <= 1000; i++) {
      double v = ft_evaluate_curve(&c, i / 1000.0);
      CHECK(v >= prev && v >= 0 && v <= 1);
      prev = v;
    }
  }
  CHECK(k == 5);
  CHECK(ft_find_curve_preset("nope", &c) == -1);
  CHECK(ft_find_curve_preset("strong", &c) == 0);
  CHECK(ft_evaluate_curve(&c, 0.25) < 0.25 &&
        ft_evaluate_curve(&c, 0.75) > 0.75);
  CHECK(ft_find_curve_preset("fade", &c) == 0);
  CHECK_NEAR(ft_evaluate_curve(&c, 0), 0.06, 1e-6); /* lifted blacks */

  /* monotone data with a flat run and a steep step: no overshoot */
  CHECK(ft_parse_curve("0,0 0.3,0.1 0.4,0.1 0.45,0.9 1,1", &c, &e) == 0);
  {
    double prev = -1;
    for(i = 0; i <= 1000; i++) {
      double x = i / 1000.0, v = ft_evaluate_curve(&c, x);
      CHECK(v >= prev);
      if(x >= 0.3 && x <= 0.4)
        CHECK_NEAR(v, c.y[1], 1e-12); /* flat stays flat */
      prev = v;
    }
  }

  /* parser and validation errors */
  CHECK(ft_parse_curve("0,0 1,1", &c, &e) == 0 && c.point_count == 2);
  CHECK(ft_parse_curve("  0,0   0.5,0.6 1,1 ", &c, &e) == 0 &&
        c.point_count == 3);
  CHECK_NEAR(c.y[1], 0.6, 1e-6);
  CHECK(ft_parse_curve("0,0", &c, &e) == -1);     /* one point */
  CHECK(ft_parse_curve("0,0 0.5", &c, &e) == -1); /* no y */
  CHECK(ft_parse_curve("0,0 0.5;0.5 1,1", &c, &e) == -1);
  CHECK(ft_parse_curve("0,0 0.5,0.5x 1,1", &c, &e) == -1);
  CHECK(ft_parse_curve("0,0 0.5,0.5 0.5,0.6 1,1", &c, &e) == -1);
  CHECK(strstr(e.message, "increase") != NULL);
  CHECK(ft_parse_curve("0,0 1,1.2", &c, &e) == -1); /* y > 1 */
  CHECK(ft_parse_curve("0,0 0.1,0.1 0.15,0.15 0.2,0.2 0.25,0.25 0.3,0.3 "
                       "0.35,0.35 0.4,0.4 0.45,0.45 0.5,0.5 0.55,0.55 "
                       "0.6,0.6 0.65,0.65 0.7,0.7 0.75,0.75 0.8,0.8 1,1",
                       &c, &e) == -1); /* 17 points */
  /* review edge cases */
  {
    ft_curve h;
    memset(&h, 0, sizeof h);
    h.point_count = 1;
    CHECK(ft_validate_curve(&h, &e) == -1);
    h.point_count = FT_CURVE_MAX + 1;
    CHECK(ft_validate_curve(&h, &e) == -1);
    ft_init_identity_curve(&h);
    h.x[0] = NAN;
    CHECK(ft_validate_curve(&h, &e) == -1);
    h.x[0] = -0.5f;
    CHECK(ft_validate_curve(&h, &e) == -1);

    CHECK(ft_parse_curve("", &c, &e) == -1);
    CHECK(ft_parse_curve("0,0 1e300,1", &c, &e) == -1);

    CHECK(ft_parse_curve("0,0 0.5,1 1,0", &c, &e) == 0);
    CHECK_NEAR(ft_evaluate_curve(&c, 0.5), 1, 0);
    for(i = 0; i <= 1000; i++) {
      double v = ft_evaluate_curve(&c, i / 1000.0);
      CHECK(v >= 0 && v <= 1);
    }

    ft_init_identity_curve(&h);
    CHECK_NEAR(ft_evaluate_curve(&h, NAN), 0, 0);
    CHECK(ft_get_curve_preset_name(-1) == NULL);
  }
  return TEST_DONE();
}

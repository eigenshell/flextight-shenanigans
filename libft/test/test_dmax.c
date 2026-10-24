/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdio.h>

#include "ft_dmax.h"
#include "ft_plan.h"
#include "test/ftest.h"

int main(void) {
  ft_optics_limits now = {FT_SCALE_MIN, FT_SCALE_MAX, &ft_lens_default};
  ft_optics_limits low = {1200.0, FT_SCALE_MAX, &ft_lens_default};
  double d = -1, d2 = -1;

  /* the most light: the smallest scale ppi x bin in range, through
   * ft_plan's model (1220 ppi: bin 2, 2440) */
  CHECK(ft_dmax_for_ppi(&now, 1220, &d));
  CHECK(d == ft_dmax_est(ft_light_rel(&ft_lens_default, 1220 * 2)));
  CHECK(ft_dmax_for_ppi(&now, 2450, &d));
  CHECK(d == ft_dmax_est(ft_light_rel(&ft_lens_default, 2450)));
  CHECK(ft_dmax_for_ppi(&now, 1000, &d)); /* bins 6 4 3 2 */
  CHECK(d == ft_dmax_est(ft_light_rel(&ft_lens_default, 2000)));
  CHECK(ft_dmax_for_ppi(&now, 1500, &d)); /* the floor itself */
  CHECK(d == ft_dmax_est(ft_light_rel(&ft_lens_default, 1500)));
  CHECK(ft_dmax_for_ppi(&now, 6300, &d)); /* the ceiling itself */
  CHECK(d == ft_dmax_est(ft_light_rel(&ft_lens_default, 6300)));

  /* the limits are the caller's: a lower floor lets 1220 ppi use bin 1
   * (scale 1220), more light, a higher Dmax */
  CHECK(ft_dmax_for_ppi(&now, 1220, &d) && ft_dmax_for_ppi(&low, 1220, &d2));
  CHECK(d2 == ft_dmax_est(ft_light_rel(&ft_lens_default, 1220)));
  CHECK(d2 > d);
  /* the ceiling too: 4000 ppi has no bin under a 3000 ceiling */
  low.scale_min = FT_SCALE_MIN;
  low.scale_max = 3000;
  CHECK(!ft_dmax_for_ppi(&low, 4000, &d));

  /* no bin fits: no estimate, *dmax untouched */
  d = -1;
  CHECK(!ft_dmax_for_ppi(&now, 6301, &d) && d == -1);
  CHECK(!ft_dmax_for_ppi(&now, 100, &d) && d == -1);
  CHECK(!ft_dmax_for_ppi(&now, NAN, &d) && d == -1);
  CHECK(!ft_dmax_for_ppi(&now, INFINITY, &d) && d == -1);
  CHECK(!ft_dmax_for_ppi(&now, 0, &d) && d == -1);
  CHECK(!ft_dmax_for_ppi(&now, -2000, &d) && d == -1);
  /* no limits or no lens: none either */
  CHECK(!ft_dmax_for_ppi(NULL, 2000, &d) && d == -1);
  low.lens = NULL;
  CHECK(!ft_dmax_for_ppi(&low, 2000, &d) && d == -1);

  return TEST_DONE();
}

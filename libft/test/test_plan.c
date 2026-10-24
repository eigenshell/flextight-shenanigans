/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdio.h>

#include "ft_plan.h"
#include "test/ftest.h"

int main(void) {
  int b[6], n;

  /* usable bins: optics ppi x bin within 1500..6300, largest first */
  n = ft_bins_for_ppi(2000, b);
  CHECK(n == 3 && b[0] == 3 && b[1] == 2 && b[2] == 1);
  n = ft_bins_for_ppi(1000, b);
  CHECK(n == 4 && b[0] == 6 && b[1] == 4 && b[2] == 3 && b[3] == 2);
  n = ft_bins_for_ppi(525, b);
  CHECK(n == 4 && b[0] == 12 && b[3] == 3);
  n = ft_bins_for_ppi(3150, b);
  CHECK(n == 2 && b[0] == 2 && b[1] == 1);
  n = ft_bins_for_ppi(6300, b);
  CHECK(n == 1 && b[0] == 1);
  CHECK(ft_bins_for_ppi(6301, b) == 0);
  CHECK(ft_bins_for_ppi(500, b) == 0);
  CHECK(ft_bins_for_ppi(NAN, b) == 0);
  /* the default (fastest) is ft_bin_for_ppi's */
  CHECK(ft_bins_for_ppi(2000, b) && b[0] == ft_bin_for_ppi(2000));
  CHECK(ft_bins_for_ppi(1200, b) && b[0] == ft_bin_for_ppi(1200));
  CHECK(ft_bin_ok(2000, 1) && ft_bin_ok(2000, 3));
  CHECK(!ft_bin_ok(2000, 6) && !ft_bin_ok(2000, 5) && !ft_bin_ok(1000, 1));

  /* light: 1 at 6300 optics; ~3.5x at 2000 and ~4.4x at 1500 */
  CHECK_NEAR(ft_light_rel(&ft_lens_default, 6300), 1.0, 1e-12);
  CHECK_NEAR(ft_light_rel(&ft_lens_default, 2000), 3.543, 0.01);
  CHECK_NEAR(ft_light_rel(&ft_lens_default, 1500), 4.400, 0.01);
  /* Dmax: 3.0 at 6300 (measured), ~3.55 at 2000 (3.60 measured), the
   * per-read ceiling caps it */
  CHECK_NEAR(ft_dmax_est(1.0), 3.0, 1e-12);
  CHECK_NEAR(ft_dmax_est(ft_light_rel(&ft_lens_default, 2000)), 3.55, 0.01);
  CHECK_NEAR(ft_dmax_est(100), FT_DMAX_CEIL, 1e-12);

  /* time: the measured line floods (2026-10-13) */
  CHECK_NEAR(ft_line_seconds(9133, 2), 433.5, 5);         /* 1000 ppi bin 2 */
  CHECK_NEAR(ft_line_seconds(13700, 1), 1300.7, 15);      /* 1500 ppi bin 1 */
  CHECK_NEAR(ft_line_seconds(9133, 6), 144.4, 2);         /* 1000 ppi bin 6 */
  CHECK_NEAR(ft_line_seconds(1409, 1), 134.0, 2);         /* 2000 bin 1 crop */
  CHECK_NEAR(ft_line_seconds(4795, 12), 37.9, 1);         /* standard */
  CHECK_NEAR(ft_line_seconds(FT_CAL_LINES, 1), 24.2, 1);  /* cal, bin 1 */
  CHECK_NEAR(ft_line_seconds(FT_CAL_LINES, 3), 7.8, 0.5); /* cal, bin 3 */
  /* a whole scan: fixed + calibration + lines */
  CHECK_NEAR(ft_scan_seconds(2000, 3, 36),
             FT_SCAN_FIXED + (2834.6 + 250) * 2664 / 84000.0, 0.5);
  CHECK(ft_scan_seconds(2000, 1, 36) > 2.5 * ft_scan_seconds(2000, 3, 36));

  return TEST_DONE();
}

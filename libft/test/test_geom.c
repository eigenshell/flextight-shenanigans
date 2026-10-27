/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include "ft_geom.h"
#include "ft_holder.h"
#include "test/ftest.h"

int main(void) {
  const ft_holder* h;

  /* at the 6000 scale, mm map straight onto CCD pixels from 144 */
  CHECK_NEAR(ft_mm_to_ccd(10.0, 6000), 10.0 / 25.4 * 6000 + 144, 1e-9);
  /* the optical axis is the fixed point of every scale */
  CHECK_NEAR(ft_mm_to_ccd(ft_ccd_to_mm(FT_CCD_AXIS, 6000), 3800), FT_CCD_AXIS,
             1e-9);
  CHECK_NEAR(ft_ccd_to_mm(ft_mm_to_ccd(13.0, 4800), 4800), 13.0, 1e-9);
  /* the hardware run: 2400 ppi (bin 2) crop at 13 mm started at x=1626 */
  CHECK_NEAR((ft_mm_to_ccd(13.0, 4800) - FT_CCD_FIRST) / 2, 1626, 0.6);
  CHECK_NEAR(ft_out_to_ccd(98, 0, 6), 732, 1e-9);
  CHECK_NEAR(ft_out_to_ccd(2, 3, 12), 204, 1e-9);
  CHECK_NEAR(ft_out_to_ccd(5, 0, 1), 149, 1e-9);
  CHECK_NEAR(ft_ccd_to_mm(144, 6000), 0, 1e-9);
  CHECK_NEAR(ft_ccd_to_mm(4118, 6000), 16.8233, 1e-4);

  /* below 3150: the measured correction for the curve's optics. Edges at
   * the strip start as CCD pixels at scale 2000 and 1500, each scanned
   * back to back with a standard preview that put them at these mm
   * (hardware 2026-10-13) */
  {
    const double hc[5] = {5061.3, 2913.3, 3009.7, 4967.4, 5063.3};
    const double hm[5] = {28.663, 1.276, 2.499, 27.456, 28.681};
    const double nc[5] = {4822.9, 3214.4, 3287.1, 4752.3, 4824.1};
    const double nm[5] = {28.637, 1.248, 2.481, 27.431, 28.663};
    int i;
    for(i = 0; i < 5; i++) {
      CHECK_NEAR(ft_ccd_to_mm(hc[i], 2000), hm[i], 0.01);
      CHECK_NEAR(ft_ccd_to_mm(nc[i], 1500), nm[i], 0.01);
      CHECK_NEAR(ft_mm_to_ccd(ft_ccd_to_mm(hc[i], 2000), 2000), hc[i], 1e-6);
    }
  }
  /* unchanged at 3150 and above (the 2026-10-12 model) */
  CHECK_NEAR(ft_mm_to_ccd(5.0, 3150),
             4118 + (5.0 / 25.4 * 6000 + 144 - 4118) * 3150 / 6000, 1e-9);
  CHECK_NEAR(ft_ccd_to_mm(2000, 6300),
             ((2000 - 4118) * 6000 / 6300.0 + 4118 - 144) * 25.4 / 6000, 1e-9);
  /* continuous across the blend (no jump at 2000 / 3150) */
  CHECK_NEAR(ft_ccd_to_mm(3000, 3149.999), ft_ccd_to_mm(3000, 3150), 1e-4);
  CHECK_NEAR(ft_ccd_to_mm(3000, 1999.999), ft_ccd_to_mm(3000, 2000.001), 1e-4);

  h = ft_holder_find("24x217");
  CHECK(h != NULL);
  CHECK_NEAR(h->scan_len_mm, 231.0, 1e-9);
  CHECK_NEAR(h->scan_width_mm, 32.0, 1e-9);
  CHECK(h->grid_frames == 6);
  CHECK(strcmp(h->name, "24x217") == 0);
  /* FlexColor 3f autoframe: 50, 943, 992 lines at 630 ppi */
  CHECK_NEAR(h->grid_first_mm, 50 / 630.0 * 25.4, 0.01);
  CHECK_NEAR(h->grid_pitch_mm, 943 / 630.0 * 25.4, 0.01);
  CHECK_NEAR(h->grid_frame_mm, 992 / 630.0 * 25.4, 0.01);
  /* the last grid frame overshoots the scan length; callers clip it */
  CHECK_NEAR(h->grid_first_mm +
               (h->grid_frames - 1) * h->grid_pitch_mm +
               h->grid_frame_mm,
             232.12, 1e-9);
  CHECK(h->grid_first_mm +
          (h->grid_frames - 1) * h->grid_pitch_mm +
          h->grid_frame_mm >
        h->scan_len_mm);
  CHECK(ft_holder_find("60x180") == NULL);
  CHECK(ft_holder_find(NULL) == NULL);
  return TEST_DONE();
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pinned codes were computed by hand (Python, double precision) from the
 * formula in ft_density.h, not from this code. */
#include <math.h>
#include <string.h>

#include "ft_density.h"
#include "test/ftest.h"

static ft_dens g_d; /* 384 KB: not on the stack */

int main(void) {
  /* T -> u16 exactly as ft_dng_write rounds */
  {
    float t[6] = {0.25f, 0.5f, 1e-6f, -0.1f, 1.5f, 0};
    unsigned short o[6];
    t[5] = NAN;
    ft_dens_t16(t, o, 6);
    CHECK(o[0] == 16384 && o[1] == 32768 && o[2] == 0);
    CHECK(o[3] == 0 && o[4] == 65535 && o[5] == 0);
  }

  /* codes: base 0.5 (R), 0.0 (G), 0.62 (B) */
  {
    const double base[3] = {0.5, 0.0, 0.62};
    ft_dens_init(&g_d, base);
    CHECK(g_d.lut[0][20724] == 1638); /* T at the base */
    CHECK(g_d.lut[0][65535] == 0);    /* clear: thinner than base */
    CHECK(g_d.lut[0][6553] == 9831);
    CHECK(g_d.lut[0][1] == 65535); /* clamped at the top */
    CHECK(g_d.lut[0][0] == 65535); /* no light */
    CHECK(g_d.lut[1][32768] == 6570);
    CHECK(g_d.lut[2][655] == 24252);
    CHECK(g_d.lut[2][13107] == 2932);
    CHECK_NEAR(ft_dens_code_to_d(1638), 0.0, 1e-4);
    CHECK_NEAR(ft_dens_code_to_d(65535), 3.9, 1e-12);
  }

  /* denser film -> higher code, in every channel */
  {
    long u;
    int c, mono = 1;
    for (c = 0; c < 3; c++)
      for (u = 2; u < 65536; u++) mono &= g_d.lut[c][u] <= g_d.lut[c][u - 1];
    CHECK(mono);
  }

  /* rgb: table lookups per channel */
  {
    const unsigned short t16[6] = {20724, 32768, 655, 65535, 0, 13107};
    unsigned short o[6];
    ft_dens_rgb(&g_d, t16, o, 2);
    CHECK(o[0] == 1638 && o[1] == 6570 && o[2] == 24252);
    CHECK(o[3] == 0 && o[4] == 65535 && o[5] == 2932);
  }

  /* weights: inverse variance, normalised; bad input -> equal */
  {
    const double v[3] = {1, 1, 4}, bad[3] = {1, 0, 1};
    const double nan3[3] = {1, NAN, 1};
    double w[3];
    ft_dens_weights(v, w);
    CHECK_NEAR(w[0], 4.0 / 9, 1e-12);
    CHECK_NEAR(w[2], 1.0 / 9, 1e-12);
    CHECK_NEAR(w[0] + w[1] + w[2], 1, 1e-12);
    ft_dens_weights(bad, w);
    CHECK(w[0] == 1.0 / 3 && w[1] == 1.0 / 3 && w[2] == 1.0 / 3);
    ft_dens_weights(nan3, w);
    CHECK(w[1] == 1.0 / 3);
  }

  /* mono: weighted mean of the channel codes; equal bases and equal
   * transmittance (a neutral silver image) give that channel's code */
  {
    const double base[3] = {0.3, 0.3, 0.3};
    const double w[3] = {4.0 / 9, 4.0 / 9, 1.0 / 9};
    unsigned short t16[6] = {6553, 6553, 6553, 65535, 65535, 0};
    unsigned short o[2];
    ft_dens_init(&g_d, base);
    ft_dens_mono(&g_d, w, t16, o, 2);
    CHECK(o[0] == g_d.lut[0][6553]);
    /* 4/9 x 0 + 4/9 x 0 + 1/9 x 65535 = 7281.67 */
    CHECK(o[1] == 7282);
  }

  /* the base is clamped into the job's range */
  {
    const double base[3] = {-1, 9, NAN};
    ft_dens_init(&g_d, base);
    CHECK(g_d.base[0] == 0 && g_d.base[1] == 4.5 && g_d.base[2] == 0);
  }
  return TEST_DONE();
}

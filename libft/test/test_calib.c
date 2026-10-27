/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdlib.h>
#include <string.h>

#include "ft_calib.h"
#include "test/ftest.h"

int main(void) {
  enum { PX = 4, CL = 250 };
  static unsigned short cal[CL * PX * 3];
  float dark[PX * 3], white[PX * 3];
  ft_err e;
  int l, i;

  /* the 3150 ppi case: 188 dark lines, 12 white, 2 transition, 48 white */
  for(l = 0; l < CL; l++)
    for(i = 0; i < PX * 3; i++)
      cal[l * PX * 3 + i] = (unsigned short)(l < 188   ? 300 + l % 3
                                             : l < 200 ? 3600
                                             : l < 202 ? 2000
                                                       : 3600);
  CHECK(ft_split_cal(cal, CL, PX, dark, white, &e) == 0);
  CHECK_NEAR(dark[0], 300.995, 0.01);
  CHECK_NEAR(white[5], 3600, 1e-3);

  /* a half-lit lamp-transition line just above the dark level (seen at
   * bin 1: ~8 % of the range) is neither dark nor white: the dark
   * reference stays 300 (a 10 % band put it at ~301.4) */
  for(l = 0; l < CL; l++)
    for(i = 0; i < PX * 3; i++)
      cal[l * PX * 3 + i] = (unsigned short)(l < 194   ? 300
                                             : l < 195 ? 564
                                                       : 3600);
  CHECK(ft_split_cal(cal, CL, PX, dark, white, &e) == 0);
  CHECK_NEAR(dark[0], 300, 1e-3);
  CHECK_NEAR(white[0], 3600, 1e-3);

  for(i = 0; i < CL * PX * 3; i++) cal[i] = 300;
  CHECK(ft_split_cal(cal, CL, PX, dark, white, &e) == -1);

  /* flat-field: (raw - dark) / (white - dark), clipped to 0..1 */
  {
    unsigned short raw[3] = {1950, 100, 5000};
    float d[3] = {300, 300, 300}, w[3] = {3600, 3600, 3600}, t[3];
    ft_flatfield(raw, 1, 1, d, w, t);
    CHECK_NEAR(t[0], 0.5, 1e-6);
    CHECK_NEAR(t[1], 0.0, 1e-9);
    CHECK_NEAR(t[2], 1.0, 1e-9);
  }

  /* Faul Pixs: header, then per channel "100\t0" and "<pixel>\t+999" */
  {
    static const char txt[] =
      "Faul Pixs\0"
      "100\t0\n1234\t+999\n100\t0\n100\t0\n2000\t+999\n"
      "77\t-12\n";
    int pix[3][FT_FAUL_MAX], cnt[3];
    CHECK(ft_faul_parse(txt, sizeof txt - 1, pix, cnt) == 2);
    CHECK(cnt[0] == 1 && pix[0][0] == 1234);
    CHECK(cnt[1] == 0);
    CHECK(cnt[2] == 1 && pix[2][0] == 2000);
  }

  /* Faul fix: flagged output pixels interpolate from good neighbours */
  {
    float t[10 * 3];
    int pix[3][FT_FAUL_MAX], cnt[3] = {2, 0, 0};
    for(i = 0; i < 10; i++) t[i * 3] = i * 0.1f;
    t[4 * 3] = 9;
    t[5 * 3] = 9;
    pix[0][0] = 144 + 4; /* x = 0, bin = 1 -> output pixel 4 */
    pix[0][1] = 144 + 5;
    ft_faul_fix(t, 1, 10, pix, cnt, 0, 1);
    CHECK_NEAR(t[4 * 3], 0.4, 1e-6);
    CHECK_NEAR(t[5 * 3], 0.5, 1e-6);
  }

  /* tri-linear alignment: R[l + 2k], G[l + k], B[l] form output line l */
  {
    float t[6 * 3];
    for(l = 0; l < 6; l++)
      for(i = 0; i < 3; i++) t[l * 3 + i] = (float)(l * 10 + i);
    CHECK(ft_align_rgb(t, 6, 1, 1) == 4);
    CHECK_NEAR(t[0], 20, 1e-9);
    CHECK_NEAR(t[1], 11, 1e-9);
    CHECK_NEAR(t[2], 2, 1e-9);
    CHECK_NEAR(t[3 * 3 + 0], 50, 1e-9);
  }

  /* more faul-fix cases */
  {
    float t[2 * 10 * 3], u[3 * 3];
    int pix[3][FT_FAUL_MAX], cnt[3] = {1, 0, 0};
    int ln, j;
    memset(pix, 0, sizeof pix);
    for(ln = 0; ln < 2; ln++)
      for(j = 0; j < 10; j++) {
        t[(ln * 10 + j) * 3 + 0] = j * 0.1f + ln;
        t[(ln * 10 + j) * 3 + 1] = 5;
        t[(ln * 10 + j) * 3 + 2] = 6;
      }
    pix[0][0] = 100; /* d = -44 -> -11: outside */
    CHECK(ft_faul_fix(t, 2, 10, pix, cnt, 0, 4) == 0);
    CHECK_NEAR(t[0], 0, 1e-9);
    CHECK_NEAR(t[4 * 3], 0.4, 1e-6);
    t[3 * 3] = 9;
    t[(10 + 3) * 3] = 9;
    pix[0][0] = 144 + 4 * 5; /* bin 4, x 2 -> column 3 */
    CHECK(ft_faul_fix(t, 2, 10, pix, cnt, 2, 4) == 0);
    CHECK_NEAR(t[3 * 3], 0.3, 1e-6);
    CHECK_NEAR(t[(10 + 3) * 3], 1.3, 1e-6);
    CHECK_NEAR(t[3 * 3 + 1], 5, 1e-9);
    CHECK_NEAR(t[3 * 3 + 2], 6, 1e-9);
    CHECK_NEAR(t[(10 + 3) * 3 + 1], 5, 1e-9);
    CHECK(ft_faul_fix(t, 2, 10, pix, cnt, 0, 0) == -1);

    /* edges and all-flagged on a 3 px line */
    u[0] = 7;
    u[3] = 8;
    u[6] = 9;
    pix[0][0] = 144;
    CHECK(ft_faul_fix(u, 1, 3, pix, cnt, 0, 1) == 0);
    CHECK_NEAR(u[0], 8, 1e-9);
    pix[0][0] = 144 + 2;
    CHECK(ft_faul_fix(u, 1, 3, pix, cnt, 0, 1) == 0);
    CHECK_NEAR(u[6], 8, 1e-9);
    u[0] = 1;
    u[3] = 2;
    u[6] = 3;
    cnt[0] = 3;
    pix[0][0] = 144;
    pix[0][1] = 145;
    pix[0][2] = 146;
    CHECK(ft_faul_fix(u, 1, 3, pix, cnt, 0, 1) == 0);
    CHECK_NEAR(u[0], 1, 1e-9);
    CHECK_NEAR(u[3], 2, 1e-9);
    CHECK_NEAR(u[6], 3, 1e-9);
  }

  /* align: 6 lines x 2 px, k = 1 */
  {
    float t[6 * 2 * 3];
    int px, o;
    for(l = 0; l < 6; l++)
      for(px = 0; px < 2; px++)
        for(i = 0; i < 3; i++)
          t[(l * 2 + px) * 3 + i] = (float)(l * 100 + px * 10 + i);
    CHECK(ft_align_rgb(t, 6, 2, 1) == 4);
    for(o = 0; o < 4; o++)
      for(px = 0; px < 2; px++) {
        CHECK_NEAR(t[(o * 2 + px) * 3 + 0], (o + 2) * 100 + px * 10, 1e-9);
        CHECK_NEAR(t[(o * 2 + px) * 3 + 1], (o + 1) * 100 + px * 10 + 1, 1e-9);
        CHECK_NEAR(t[(o * 2 + px) * 3 + 2], o * 100 + px * 10 + 2, 1e-9);
      }
    CHECK(ft_align_rgb(t, 6, 2, 0) == 6);
    CHECK(ft_align_rgb(t, 4, 2, 2) == 4);
    CHECK_NEAR(t[0], 200, 1e-9); /* untouched */
  }

  /* CRLF line endings */
  {
    static const char txt[] =
      "Faul Pixs\r\n100\t0\r\n1234\t+999\r\n100\t0\r\n100\t0\r\n"
      "2000\t+999\r\n";
    int pix[3][FT_FAUL_MAX], cnt[3];
    CHECK(ft_faul_parse(txt, sizeof txt - 1, pix, cnt) == 2);
    CHECK(cnt[0] == 1 && pix[0][0] == 1234);
    CHECK(cnt[2] == 1 && pix[2][0] == 2000);
  }
  CHECK(ft_split_cal(cal, 0, PX, dark, white, &e) == -1);

  /* white-reference noise: white lines alternate +-a per channel
   * (a = 10, 20, 40 DN) over a 4000 DN span -> variance (a / 4000)^2 */
  {
    enum { WP = 3, WL = 250 };
    static unsigned short c2[WL * WP * 3];
    static const int a[3] = {10, 20, 40};
    double var[3];
    int l2, k;
    for(l2 = 0; l2 < WL; l2++)
      for(k = 0; k < WP * 3; k++)
        c2[l2 * WP * 3 + k] =
          (unsigned short)(l2 < 200 ? 300
                                    : 4300 + (l2 % 2 ? a[k % 3] : -a[k % 3]));
    CHECK(ft_white_var(c2, WL, WP, var, &e) == 0);
    CHECK_NEAR(var[0], 100.0 / 16e6, 1e-12);
    CHECK_NEAR(var[1], 400.0 / 16e6, 1e-12);
    CHECK_NEAR(var[2], 1600.0 / 16e6, 1e-12);
    CHECK(ft_white_var(c2, 0, WP, var, &e) == -1);
  }
  return TEST_DONE();
}

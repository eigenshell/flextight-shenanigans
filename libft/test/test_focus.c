/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_focus.h"
#include "test/ftest.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  long c, o, shift;
  double pk;

  /* bin: largest divisor of 12 with ppi x bin <= 6300 */
  CHECK(ft_bin_for_ppi(525) == 12);
  CHECK(ft_bin_for_ppi(1000) == 6);
  CHECK(ft_bin_for_ppi(1075) == 4);
  CHECK(ft_bin_for_ppi(1200) == 4);
  CHECK(ft_bin_for_ppi(2000) == 3);
  CHECK(ft_bin_for_ppi(2400) == 2);
  CHECK(ft_bin_for_ppi(3150) == 2);
  CHECK(ft_bin_for_ppi(4000) == 1);
  CHECK(ft_bin_for_ppi(6300) == 1);
  CHECK(ft_bin_for_ppi(6301) == -1);

  /* supported: 525..6300 (every bin's scale is >= 3150 >= FT_SCALE_MIN) */
  CHECK(FT_SCALE_MIN == 1500.0);
  CHECK(ft_ppi_supported(525));
  CHECK(ft_ppi_supported(600));
  CHECK(ft_ppi_supported(634));
  CHECK(ft_ppi_supported(3150));
  CHECK(ft_ppi_supported(3500));
  CHECK(ft_ppi_supported(3800));
  CHECK(!ft_ppi_supported(500));
  CHECK(!ft_ppi_supported(6301));

  /* thin-lens optics, this scanner's curve (values from the 2026-10-12 fit) */
  CHECK(ft_optics_for(&ft_lens_default, 3807, &c, &o) == 0);
  CHECK(c == 440053 && o == 910852);
  CHECK(ft_optics_for(&ft_lens_default, 6000, &c, &o) == 0);
  CHECK(c == 285187 && o == 998540);
  CHECK(ft_optics_for(&ft_lens_default, 6300, &c, &o) == 0);
  CHECK(c == 259254 && o == 1005789);
  /* low scales: the thin lens (checked against the scanner's FP table)
   * plus the survey's correction, tandem: -4778 at 1500, -1358 at 2000 */
  CHECK(ft_optics_for(&ft_lens_default, 1500, &c, &o) == 0);
  CHECK(c == 326230 - 4778 && o == 541866 - 4778);
  CHECK(ft_positions_ok(c, o));
  CHECK(ft_optics_for(&ft_lens_default, 2000, &c, &o) == 0);
  CHECK(c == 423152 - 1358 && o == 694091 - 1358);
  CHECK(ft_positions_ok(c, o));
  /* the correction: linear between its points, held past the ends, 0
   * from 3000 up (the high scales above stay the 2026-10-12 fit) */
  CHECK_NEAR(ft_lens_correction(&ft_lens_default, 1750), -3068, 1e-9);
  CHECK_NEAR(ft_lens_correction(&ft_lens_default, 2500), -679, 1e-9);
  CHECK(ft_lens_correction(&ft_lens_default, 3807) == 0);
  CHECK(ft_lens_correction(&ft_lens_default, 1000) == -11278);
  {
    ft_lens bare = ft_lens_default;
    bare.ncorr = 0;
    CHECK(ft_lens_correction(&bare, 1500) == 0);
    CHECK(ft_optics_for(&bare, 1500, &c, &o) == 0);
    CHECK(c == 326230 && o == 541866);
  }
  CHECK(ft_optics_for(&ft_lens_default, 1499, &c, &o) == -1);
  /* scan positions: the FP table's scanning envelope (user, 2026-10-15) */
  CHECK(FT_CCD_MIN == 56674 && FT_CCD_MAX == 466453);
  CHECK(FT_OPTIC_MIN == 217000 && FT_OPTIC_MAX == 1010000);
  CHECK(FT_SCALE_REACH_MIN == 980.0);
  CHECK(ft_positions_ok(317523, 530820)); /* FP entry 6, m 0.51 */
  CHECK(ft_positions_ok(56674, 217000));  /* FP entry 4, A4 reflective */
  CHECK(ft_positions_ok(466453, 833910)); /* FP entry 16, 1:1 */
  CHECK(!ft_positions_ok(56673, 217000));
  CHECK(!ft_positions_ok(56674, 216999));
  CHECK(!ft_positions_ok(466454, 833910));
  CHECK(ft_positions_ok(259254, 1010000));
  CHECK(!ft_positions_ok(259254, 1010001));
  CHECK(ft_optics_for(&ft_lens_default, 6400, &c, &o) == -1);

  CHECK(ft_positions_ok(285187, 998540));
  CHECK(!ft_positions_ok(1020000, 997296)); /* CCD past the lens */
  CHECK(ft_positions_ok(200000, 998540));   /* inside the FP table */
  /* carrier gap: lens - CCD >= 150000 while scanning (FP table >= 160231) */
  CHECK(FT_GAP_BOTTOM == 1000 && FT_GAP_MIN == 150000);
  CHECK(ft_positions_ok(300000, 450000));    /* gap 150000 */
  CHECK(!ft_positions_ok(300001, 450000));   /* gap 149999 */
  CHECK(!ft_positions_ok(1020000, 1021000)); /* maintenance bottom, gap 1000 */

  /* parabola peak: the 4800-scale bracket measured on hardware */
  {
    long x[5] = {378816, 379816, 380816, 381816, 382816};
    double y[5] = {.0927, .1006, .1027, .0977, .0890};
    CHECK(ft_peak(x, y, 5, &pk) == 0);
    CHECK_NEAR(pk, 380612, 1);
  }
  {
    long x[3] = {0, 1000, 2000};
    double y[3] = {.1, .2, .3}; /* still rising: edge */
    CHECK(ft_peak(x, y, 3, &pk) == 1);
    CHECK_NEAR(pk, 2000, 1e-9);
    CHECK(ft_peak(x, y, 2, &pk) == -1);
  }

  /* 3-point decision (tandem offsets): all within the margin of their
   * mean -> shift */
  {
    long off[3] = {400, 500, 620};
    CHECK(ft_focus_decide(off, &shift) == FT_FOCUS_SHIFTED);
    CHECK(shift == 507);
  }
  {
    long off[3] = {0, 100, 900}; /* bowed strip */
    CHECK(ft_focus_decide(off, &shift) == FT_FOCUS_PER_FRAME);
  }
  {
    long off[3] = {-600, -640, -580};
    CHECK(ft_focus_decide(off, &shift) == FT_FOCUS_SHIFTED);
    CHECK(shift == -607);
  }
  /* tandem focus: CCD and lens move together, the gap stays */
  {
    long c0, o0, c1, o1;
    CHECK(ft_optics_for(&ft_lens_default, 6300, &c0, &o0) == 0);
    CHECK(ft_optics_focus(&ft_lens_default, 6300, -2068, &c1, &o1) == 0);
    CHECK(c1 == c0 - 2068 && o1 == o0 - 2068);
    CHECK(ft_optics_focus(&ft_lens_default, 100, 0, &c1, &o1) == -1);
    CHECK(FT_FOCUS_MARGIN == 300 && FT_BRACKET_MAX == 4000);
    CHECK(FT_CAL_MARGIN == 1200 && FT_CAL_BRACKET_MAX == 12000);
  }
  /* sharpness: a crisp pattern beats a blurred one, flat is zero */
  {
    float crisp[128], soft[128], flat[128], ramp[128], dim[128];
    int i;
    for(i = 0; i < 128; i++) {
      crisp[i] = (i / 4) % 2 ? 0.8f : 0.2f;
      soft[i] = 0.5f + 0.15f * (float)sin(i * 3.14159 / 4);
      flat[i] = 0.5f;
      ramp[i] = 0.1f + 0.001f * (i % 16);
      dim[i] = crisp[i] * 0.01f;
    }
    CHECK(ft_sharpness(crisp, 16, 8) > ft_sharpness(soft, 16, 8));
    CHECK_NEAR(ft_sharpness(flat, 16, 8), 0, 1e-12);
    /* relative gradient per pixel: 0.001 / ~0.1075 */
    CHECK_NEAR(ft_sharpness(ramp, 16, 8), 0.001 / 0.1075, 0.0005);
    /* gain-free: a denser frame or a dimmer lamp scores the same */
    CHECK_NEAR(ft_sharpness(dim, 16, 8), ft_sharpness(crisp, 16, 8), 1e-5);
    CHECK(ft_sharpness(crisp, 4, 32) == 0); /* too small */
  }
  /* sharpness on a dense slide (hardware 2026-10-13): transmittance
   * ~0.01 with sensor noise as large as the detail. The crisp edge
   * pattern must still clearly beat the same pattern defocused; a
   * per-pixel log gradient is noise-dominated here (B2: 8 % contrast
   * over a whole focus sweep, against 73 % for this metric). */
  {
    enum { W = 96, H = 48 };
    static float crisp[W * H], soft[W * H], clean[W];
    unsigned st = 12345;
    int x, y, k;
    for(x = 0; x < W; x++)
      clean[x] = (x / 12) % 2 ? 0.014f : 0.006f;
    for(y = 0; y < H; y++)
      for(x = 0; x < W; x++) {
        double u, v, n, sm = 0;
        for(k = -5; k <= 5; k++) /* defocus: 11-px box */
          sm += clean[(x + k + W) % W];
        st = st * 1103515245u + 12345u;
        u = (((st >> 8) & 0xffffff) + 0.5) / 16777216.0;
        st = st * 1103515245u + 12345u;
        v = ((st >> 8) & 0xffffff) / 16777216.0;
        n = 0.003 * sqrt(-2 * log(u)) * cos(2 * 3.14159265 * v);
        crisp[y * W + x] = (float)(clean[x] + n);
        soft[y * W + x] = (float)(sm / 11 + n);
      }
    CHECK(ft_sharpness(crisp, W, H) > 1.3 * ft_sharpness(soft, W, H));
  }

  /* focus file round trip */
  {
    ft_lens a = ft_lens_default, b;
    ft_err e;
    FILE *f = tmpfile();
    CHECK(f != NULL);
    CHECK(ft_lens_write(&a, f) == 0);
    rewind(f);
    CHECK(ft_lens_read(&b, f, &e) == 0);
    CHECK_NEAR(b.film, a.film, 0.05);
    CHECK_NEAR(b.p, a.p, 1e-9);
    CHECK(b.ncorr == a.ncorr && b.ncorr == 5);
    CHECK(b.corr_scale[1] == 1500 && b.corr_off[1] == -4778);
    fclose(f);
    f = tmpfile();
    fputs("film = 1\nw = 2\n", f);
    rewind(f);
    CHECK(ft_lens_read(&b, f, &e) == -1); /* fl and p missing */
    fclose(f);
  }

  /* lens fit recovers the curve from the eight measured focus peaks */
  {
    double M[8] = {3807, 4000, 4309, 4787, 5384, 5678, 5947, 6230};
    long C[8] = {440254, 429917, 412759, 380637,
                 335765, 312131, 289987, 265226};
    long L[8] = {911050, 922360, 938639, 959966,
                 981011, 989872, 997296, 1004246};
    ft_lens l = {1460000.0, 270000.0, 320000.0, 3.5e-4, 0, {0}, {0}};
    double rc, rl;
    CHECK(ft_lens_fit(M, C, L, 8, &l, &rc, &rl) == 0);
    CHECK(rc < 240 && rl < 130);
    CHECK(l.ncorr == 0); /* a fit stands alone */
    { /* from the default (with its table): the fit drops it */
      ft_lens d = ft_lens_default;
      d.ncorr = 0x5a5a; /* what garbage would look like */
      d = ft_lens_default;
      CHECK(ft_lens_fit(M, C, L, 8, &d, &rc, &rl) == 0);
      CHECK(d.ncorr == 0 && ft_lens_correction(&d, 1500) == 0);
    }
    CHECK_NEAR(l.film, ft_lens_default.film, 3000);
    CHECK_NEAR(l.p, ft_lens_default.p, 2e-6);
    CHECK(ft_lens_fit(M, C, L, 3, &l, &rc, &rl) == -1);
  }
  /* hardening: optics rejects NaN scale and implausible lenses */
  {
    ft_lens bad = ft_lens_default;
    long c2 = 7, o2 = 7;
    CHECK(ft_optics_for(&ft_lens_default, NAN, &c2, &o2) == -1);
    bad.p = 0;
    CHECK(ft_optics_for(&bad, 5000, &c2, &o2) == -1);
    CHECK(c2 == 7 && o2 == 7);
  }

  /* default curve reproduces the eight measured peaks */
  {
    double M[8] = {3807, 4000, 4309, 4787, 5384, 5678, 5947, 6230};
    long C[8] = {440254, 429917, 412759, 380637,
                 335765, 312131, 289987, 265226};
    long L[8] = {911050, 922360, 938639, 959966,
                 981011, 989872, 997296, 1004246};
    int i;
    for(i = 0; i < 8; i++) {
      CHECK(ft_optics_for(&ft_lens_default, M[i], &c, &o) == 0);
      CHECK(labs(c - C[i]) < 700 && labs(o - L[i]) < 350);
    }
  }

  /* decision exactly at the margin */
  {
    long a[3] = {0, 0, 900}, b[3] = {0, 300, 600};
    CHECK(ft_focus_decide(a, &shift) == FT_FOCUS_PER_FRAME);
    CHECK(ft_focus_decide(b, &shift) == FT_FOCUS_SHIFTED);
  }

  /* focus file parsing is strict and leaves *l alone on failure */
  {
    ft_lens keep = {1, 2, 3, 4, 0, {0}, {0}}, l = keep;
    ft_err e;
    char big[400];
    static const char *bad[] = {
        "film = 1\nw = 2\nfl = 3\np = nan\n",
        "film = 1\nw = 2\nfl = 3\np = 1 junk\n",
        "film = 1\nw = 2\nfl = 3\np = 1\nbogus = 5\n",
        "film = 1\nw = 2\nfl = 3\np = 0\n",
        "film = 1\nw = 2\nfl = -3\np = 1\n",
    };
    int i;
    FILE *f;
    for(i = 0; i < 5; i++) {
      f = tmpfile();
      fputs(bad[i], f);
      rewind(f);
      CHECK(ft_lens_read(&l, f, &e) == -1);
      CHECK(l.film == 1 && l.w == 2 && l.fl == 3 && l.p == 4);
      fclose(f);
    }
    memset(big, 'x', 200);
    big[200] = 0;
    f = tmpfile();
    fprintf(f, "# %s\n  # indented\nfilm = 1\nw = 2\nfl = 3\np = 4\n", big);
    rewind(f);
    CHECK(ft_lens_read(&l, f, &e) == 0);
    CHECK(l.film == 1 && l.w == 2 && l.fl == 3 && l.p == 4);
    fclose(f);
  }

  /* focus file from a buffer (the page's planner): the stream reader's
   * rules, n bytes only (no NUL needed) */
  {
    ft_lens keep = {1, 2, 3, 4, 0, {0}, {0}}, l = keep, s;
    ft_err e;
    char line[200], text[400];
    static const char good[] = "# fit\n  # x\nfilm = 1\nw = 2\nfl = 3\n"
                               "p = 4"; /* no last '\n' */
    static const char *bad[] = {
        "film = 1\nw = 2\nfl = 3\np = nan\n",
        "film = 1\nw = 2\nfl = 3\np = 1 junk\n",
        "film = 1\nw = 2\nfl = 3\np = 1\nbogus = 5\n",
        "film = 1\nw = 2\nfl = 3\np = 0\n",
        "film = 1\nw = 2\n",
    };
    static const char *msg[] = {
        "focus file line 4: value not finite",
        "focus file line 4: expected key = number",
        "focus file line 5: unknown key bogus",
        "focus file: fl and p must be positive",
        "focus file: needs film, w, fl and p",
    };
    int i;
    FILE *f;
    CHECK(ft_lens_parse(&l, good, sizeof good - 1, &e) == 0);
    CHECK(l.film == 1 && l.w == 2 && l.fl == 3 && l.p == 4);
    /* n bytes only: "p = 4" cut to "p = " */
    l = keep;
    CHECK(ft_lens_parse(&l, good, sizeof good - 2, &e) == -1);
    CHECK(strcmp(e.msg, "focus file line 6: expected key = number") == 0);
    for(i = 0; i < 5; i++) {
      l = keep;
      CHECK(ft_lens_parse(&l, bad[i], strlen(bad[i]), &e) == -1);
      CHECK(strcmp(e.msg, msg[i]) == 0);
      CHECK(l.film == 1 && l.w == 2 && l.fl == 3 && l.p == 4);
      /* the stream reader says the same */
      f = tmpfile();
      fputs(bad[i], f);
      rewind(f);
      CHECK(ft_lens_read(&s, f, &e) == -1);
      CHECK(strcmp(e.msg, msg[i]) == 0);
      fclose(f);
    }
    /* a long comment is fine, a long key line is not (158 chars of
     * content fit fgets' 160, 159 do not) */
    memset(line, 'x', 190);
    line[190] = 0;
    snprintf(text, sizeof text, "# %s\nfilm = 1\nw = 2\nfl = 3\np = 4\n", line);
    CHECK(ft_lens_parse(&l, text, strlen(text), &e) == 0);
    for(i = 158; i <= 159; i++) {
      int k = snprintf(
          text, sizeof text,
          "film = 1%*s\nw = 2\nfl = 3\n"
          "p = 4\n",
          i - 8, ""
      );
      int rp = ft_lens_parse(&l, text, (size_t)k, &e), rs;
      f = tmpfile();
      fputs(text, f);
      rewind(f);
      rs = ft_lens_read(&s, f, &e);
      fclose(f);
      CHECK(rp == rs);
      CHECK(rp == (i == 158 ? 0 : -1));
    }
    CHECK(strcmp(e.msg, "focus file line 1: too long") == 0);
  }

  /* the 95 % depth of field: the survey's fit, K pinned; against the
   * survey's measured widths (2026-10-15/16, bin 1) within 10 % (the
   * law is a fit: 651 / 3431 / 5474 here); bin scales it */
  CHECK(FT_DOF95_K == 980.0);
  CHECK_NEAR(ft_lens_dof95(&ft_lens_default, 6300, 1), 636, 63.6);
  CHECK_NEAR(ft_lens_dof95(&ft_lens_default, 2000, 1), 3651, 365.1);
  CHECK_NEAR(ft_lens_dof95(&ft_lens_default, 1500, 1), 5653, 565.3);
  CHECK_NEAR(ft_lens_dof95(&ft_lens_default, 6300, 1), 650.9, 0.5);
  CHECK_NEAR(
      ft_lens_dof95(&ft_lens_default, 3150, 2),
      2 * ft_lens_dof95(&ft_lens_default, 3150, 1), 1e-9
  );
  CHECK(
      ft_lens_dof95(&ft_lens_default, 3150, 0) ==
      ft_lens_dof95(&ft_lens_default, 3150, 1)
  );
  {
    ft_lens l = ft_lens_default;
    double m = 2 * l.p * 3000;
    l.p *= 2; /* the lens' p, not the default's */
    CHECK_NEAR(ft_lens_dof95(&l, 3000, 1), 980 * (1 + m) / (m * m), 1e-9);
  }

  /* fit converges from a start 20 % off, and keeps *l on failure */
  {
    double M[8] = {3807, 4000, 4309, 4787, 5384, 5678, 5947, 6230};
    long C[8] = {440254, 429917, 412759, 380637,
                 335765, 312131, 289987, 265226};
    long L[8] = {911050, 922360, 938639, 959966,
                 981011, 989872, 997296, 1004246};
    ft_lens l = {1.2e6, 3.2e5, 3.8e5, 4.2e-4, 0, {0}, {0}};
    ft_lens z = {1.0e6, 3.0e5, 0, 4.0e-4, 0, {0}, {0}};
    double rc, rl, Mbad[8];
    memcpy(Mbad, M, sizeof M);
    CHECK(ft_lens_fit(M, C, L, 8, &l, &rc, &rl) == 0);
    CHECK(rc < 240 && rl < 130);
    CHECK_NEAR(l.film, ft_lens_default.film, 3000);
    CHECK(ft_lens_fit(M, C, L, 8, &z, &rc, &rl) == -1);
    CHECK(z.fl == 0 && z.film == 1.0e6);
    Mbad[3] = NAN;
    l = ft_lens_default;
    CHECK(ft_lens_fit(Mbad, C, L, 8, &l, &rc, &rl) == -1);
    CHECK(l.film == ft_lens_default.film);
  }
  return TEST_DONE();
}

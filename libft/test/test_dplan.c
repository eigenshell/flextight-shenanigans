/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pinned seconds and pixel ranges were computed by hand (Python) from the
 * models in ft_plan.h and ft_geom.h, not from this code. */
#include "ft_dplan.h"
#include "ft_plan.h"
#include "test/ftest.h"
#include <math.h>
#include <string.h>

static ft_dframe fr[FT_MAX_FRAMES];

/* n FF frames, 36 mm long, every 38 mm from 2 mm */
static void strip(int n) {
  int i;
  for(i = 0; i < n; i++) {
    fr[i].index = i + 1;
    fr[i].y0 = 2 + 38.0 * i;
    fr[i].len = 36;
    fr[i].focus = 0;
  }
}

/* the fastest in-focus grouping by trying every partition (n <= 12) */
static double brute(const ft_dframe *f, int n, double ppi, long span) {
  double best = HUGE_VAL;
  unsigned mask, lim = 1u << (n - 1);
  for(mask = 0; mask < lim; mask++) {
    double t = 0;
    int i, k, start = 0, ok = 1;
    for(i = 0; i < n && ok; i++)
      if(i == n - 1 || (mask >> i & 1)) { /* a pass ends at i */
        long lo = f[start].focus, hi = lo;
        double a = f[start].y0 - FT_DETAIL_MARGIN_MM;
        double b = f[i].y0 + f[i].len + FT_DETAIL_MARGIN_MM;
        for(k = start; k <= i; k++) {
          if(f[k].focus < lo)
            lo = f[k].focus;
          if(f[k].focus > hi)
            hi = f[k].focus;
        }
        ok = hi - lo <= span;
        if(a < 0)
          a = 0;
        if(b > 231)
          b = 231;
        t += ft_scan_seconds(ppi, 1, b - a);
        start = i + 1;
      }
    if(ok && t < best)
      best = t;
  }
  return best;
}

static unsigned rng = 99;
static unsigned rnd(void) {
  rng = rng * 1103515245u + 12345u;
  return rng >> 8;
}

int main(void) {
  ft_dplan p;
  ft_err e;
  int i, trial;

  CHECK(FT_DETAIL_DOF_SPAN == 600);

  /* --select lists */
  CHECK(ft_dplan_selected("1,3-6,8", 1, &e) == 1);
  CHECK(ft_dplan_selected("1,3-6,8", 2, &e) == 0);
  CHECK(ft_dplan_selected("1,3-6,8", 5, &e) == 1);
  CHECK(ft_dplan_selected("1,3-6,8", 8, &e) == 1);
  CHECK(ft_dplan_selected("1,3-6,8", 9, &e) == 0);
  CHECK(ft_dplan_selected("12", 12, &e) == 1);
  CHECK(ft_dplan_selected("", 1, &e) == -1);
  CHECK(ft_dplan_selected("1,,2", 1, &e) == -1);
  CHECK(ft_dplan_selected("6-3", 4, &e) == -1);
  CHECK(ft_dplan_selected("0", 1, &e) == -1);
  CHECK(ft_dplan_selected("1;2", 1, &e) == -1);
  CHECK(ft_dplan_selected("3-", 3, &e) == -1);
  CHECK(ft_dplan_mid(0, 600) == 300 && ft_dplan_mid(-101, -100) == -101);
  CHECK(ft_dplan_mid(100, 101) == 101 && ft_dplan_mid(-5, 5) == 0);

  /* 3150, six frames, one focus: one pass over the whole holder */
  strip(6);
  CHECK(ft_dplan_make(fr, 6, 3150, 231, FT_DETAIL_DOF_SPAN, &p, &e) == 0);
  CHECK(p.npass == 1);
  CHECK(p.p[0].first == 0 && p.p[0].last == 5 && p.p[0].focus == 0);
  CHECK_NEAR(p.p[0].y0, 0, 1e-12);
  CHECK_NEAR(p.p[0].len, 231, 1e-12);
  CHECK_NEAR(p.seconds, 2759.4038, 1e-3);

  /* the film height steps by 700 after frame 3: two passes */
  for(i = 3; i < 6; i++)
    fr[i].focus = 700;
  CHECK(ft_dplan_make(fr, 6, 3150, 231, FT_DETAIL_DOF_SPAN, &p, &e) == 0);
  CHECK(p.npass == 2);
  CHECK(p.p[0].last == 2 && p.p[1].first == 3 && p.p[1].focus == 700);
  CHECK_NEAR(p.p[0].len, 117, 1e-12);
  CHECK_NEAR(p.p[1].y0, 113, 1e-12);
  CHECK_NEAR(p.seconds, 2840.3864, 1e-3);

  /* the pass focus is the middle of its frames' offsets */
  fr[3].focus = 101;
  fr[4].focus = 600;
  fr[5].focus = 400;
  CHECK(ft_dplan_make(fr, 6, 3150, 231, FT_DETAIL_DOF_SPAN, &p, &e) == 0);
  CHECK(p.npass == 1 && p.p[0].focus == 300); /* (0 + 600) / 2 */
  fr[0].focus = fr[1].focus = fr[2].focus = -101;
  fr[3].focus = fr[4].focus = fr[5].focus = -100;
  CHECK(ft_dplan_make(fr, 6, 3150, 231, FT_DETAIL_DOF_SPAN, &p, &e) == 0);
  CHECK(p.p[0].focus == -101); /* -100.5: away from 0 */

  /* 6300, frame 2 not selected: two passes beat one through it */
  strip(4);
  fr[1] = fr[2];
  fr[2] = fr[3];
  CHECK(ft_dplan_make(fr, 3, 6300, 231, FT_DETAIL_DOF_SPAN, &p, &e) == 0);
  CHECK(p.npass == 2);
  CHECK(p.p[0].first == 0 && p.p[0].last == 0);
  CHECK(p.p[1].first == 1 && p.p[1].last == 2);
  CHECK_NEAR(p.p[0].seconds, 1001.3211, 1e-3);
  CHECK_NEAR(p.seconds, 2922.9809, 1e-3);

  /* span 0 and different heights: a pass per frame */
  strip(5);
  for(i = 0; i < 5; i++)
    fr[i].focus = i % 2 ? 700 : 0;
  CHECK(ft_dplan_make(fr, 5, 6300, 231, 0, &p, &e) == 0);
  CHECK(p.npass == 5);

  /* the DP equals brute force on random selections and heights */
  for(trial = 0; trial < 300; trial++) {
    int n = 0, all = 6;
    double ppi = trial % 2 ? 6300 : 3150;
    strip(all);
    for(i = 0; i < all; i++)
      if(rnd() % 3) {
        fr[n] = fr[i];
        fr[n].focus = (long)(rnd() % 1500) - 700;
        n++;
      }
    if(n == 0)
      continue;
    CHECK(ft_dplan_make(fr, n, ppi, 231, FT_DETAIL_DOF_SPAN, &p, &e) == 0);
    CHECK_NEAR(p.seconds, brute(fr, n, ppi, FT_DETAIL_DOF_SPAN), 1e-6);
    { /* the passes cover the frames in order, each in focus */
      int k, next = 0;
      double sum = 0;
      for(k = 0; k < p.npass; k++) {
        long lo = fr[p.p[k].first].focus, hi = lo;
        int m;
        CHECK(p.p[k].first == next);
        for(m = p.p[k].first; m <= p.p[k].last; m++) {
          if(fr[m].focus < lo)
            lo = fr[m].focus;
          if(fr[m].focus > hi)
            hi = fr[m].focus;
        }
        CHECK(hi - lo <= FT_DETAIL_DOF_SPAN);
        next = p.p[k].last + 1;
        sum += p.p[k].seconds;
      }
      CHECK(next == n);
      CHECK_NEAR(sum, p.seconds, 1e-6);
    }
  }

  /* refusals */
  strip(3);
  CHECK(ft_dplan_make(fr, 0, 3150, 231, 600, &p, &e) == -1);
  CHECK(ft_dplan_make(fr, 3, 1000, 231, 600, &p, &e) == -1); /* optics */
  CHECK(ft_dplan_make(fr, 3, 3150, 231, -1, &p, &e) == -1);
  fr[1].y0 = 20; /* overlaps frame 1 */
  CHECK(ft_dplan_make(fr, 3, 3150, 231, 600, &p, &e) == -1);
  CHECK(strstr(e.msg, "frame 2") != NULL);
  strip(3);
  fr[2].y0 = 200; /* ends at 236 */
  CHECK(ft_dplan_make(fr, 3, 3150, 231, 600, &p, &e) == -1);

  /* a pass scan's numbers and a frame's ranges in it (3150, bin 1) */
  {
    ft_dscan s;
    ft_axes ax;
    double lo, hi;
    int r[4];
    ft_dscan_for(3150, 1, 0.5, 31.0, 113.0, 118.0, &s);
    CHECK(s.x_out == 1950 && s.y_line == 14014);
    CHECK(s.px == 3844 && s.lines == 14634);
    /* frame 4 (116 + 36 mm along, 4.1 + 24 across) with margins */
    CHECK(ft_dscan_ranges(&s, 113, 155, 1.1, 31.1, r) == 0);
    CHECK(r[0] == 0 && r[1] == 5196 && r[2] == 74 && r[3] == 3795);
    CHECK(ft_dscan_ranges(&s, 300, 320, 1, 2, r) == -1);
    ft_dscan_reach(3150, 1, &lo, &hi);
    CHECK_NEAR(lo, -15.22105, 1e-4);
    CHECK_NEAR(hi, 49.22238, 1e-4);
    CHECK(ft_dscan_thumb(&s) == 6);
    ft_dscan_axes(&s, 6, &ax);
    CHECK_NEAR(ax.y_mm0, 113.118698, 1e-6);
    CHECK_NEAR(ax.y_mm_per_line, 0.0483810, 1e-7);
    CHECK_NEAR(ax.x_mm0, 0.5229175, 1e-6);
    CHECK_NEAR(ax.x_mm_per_px, 0.0483810, 1e-7);
    CHECK(ft_dscan_line(&s, 155) == 5196);
    CHECK(ft_dscan_px(&s, 31.1) == 3795);
    ft_dscan_for(6300, 1, 0.5, 31.0, 0, 42, &s);
    CHECK(ft_dscan_thumb(&s) == 12);
  }
  return TEST_DONE();
}

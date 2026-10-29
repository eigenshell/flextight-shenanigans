/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pinned seconds and pixel ranges were computed by hand (Python) from the
 * models in ft_plan.h and ft_geom.h, not from this code. */
#include <math.h>
#include <string.h>

#include "ft_dplan.h"
#include "ft_plan.h"
#include "test/ftest.h"

static ft_detail_frame fr[FT_MAX_FRAMES];

/* n FF frames, 36 mm long, every 38 mm from 2 mm */
static void strip(int n) {
  int i;
  for(i = 0; i < n; i++) {
    fr[i].index = i + 1;
    fr[i].start_mm = 2 + 38.0 * i;
    fr[i].length_mm = 36;
    fr[i].focus_offset_steps = 0;
  }
}

/* the fastest in-focus grouping by trying every partition (n <= 12) */
static double brute(const ft_detail_frame* f, int n, double ppi, long span) {
  double best = HUGE_VAL;
  unsigned mask, lim = 1u << (n - 1);
  for(mask = 0; mask < lim; mask++) {
    double t = 0;
    int i, k, start = 0, ok = 1;
    for(i = 0; i < n && ok; i++)
      if(i == n - 1 || (mask >> i & 1)) { /* a pass ends at i */
        long lo = f[start].focus_offset_steps, hi = lo;
        double a = f[start].start_mm - FT_DETAIL_MARGIN_MM;
        double b = f[i].start_mm + f[i].length_mm + FT_DETAIL_MARGIN_MM;
        for(k = start; k <= i; k++) {
          if(f[k].focus_offset_steps < lo) lo = f[k].focus_offset_steps;
          if(f[k].focus_offset_steps > hi) hi = f[k].focus_offset_steps;
        }
        ok = hi - lo <= span;
        if(a < 0) a = 0;
        if(b > 231) b = 231;
        t += ft_estimate_scan_seconds(ppi, 1, b - a);
        start = i + 1;
      }
    if(ok && t < best) best = t;
  }
  return best;
}

static unsigned rng = 99;
static unsigned rnd(void) {
  rng = rng * 1103515245u + 12345u;
  return rng >> 8;
}

int main(void) {
  ft_detail_plan p;
  ft_err e;
  int i, trial;

  CHECK(FT_DETAIL_FOCUS_SPAN_STEPS == 600);

  /* --select lists */
  CHECK(ft_is_frame_selected("1,3-6,8", 1, &e) == 1);
  CHECK(ft_is_frame_selected("1,3-6,8", 2, &e) == 0);
  CHECK(ft_is_frame_selected("1,3-6,8", 5, &e) == 1);
  CHECK(ft_is_frame_selected("1,3-6,8", 8, &e) == 1);
  CHECK(ft_is_frame_selected("1,3-6,8", 9, &e) == 0);
  CHECK(ft_is_frame_selected("12", 12, &e) == 1);
  CHECK(ft_is_frame_selected("", 1, &e) == -1);
  CHECK(ft_is_frame_selected("1,,2", 1, &e) == -1);
  CHECK(ft_is_frame_selected("6-3", 4, &e) == -1);
  CHECK(ft_is_frame_selected("0", 1, &e) == -1);
  CHECK(ft_is_frame_selected("1;2", 1, &e) == -1);
  CHECK(ft_is_frame_selected("3-", 3, &e) == -1);
  CHECK(ft_get_focus_middle(0, 600) == 300 &&
        ft_get_focus_middle(-101, -100) == -101);
  CHECK(ft_get_focus_middle(100, 101) == 101 &&
        ft_get_focus_middle(-5, 5) == 0);

  /* 3150, six frames, one focus: one pass over the whole holder */
  strip(6);
  CHECK(ft_make_detail_plan(fr, 6, 3150, 231, FT_DETAIL_FOCUS_SPAN_STEPS, &p,
                            &e) == 0);
  CHECK(p.pass_count == 1);
  CHECK(p.passes[0].first_frame == 0 &&
        p.passes[0].last_frame == 5 &&
        p.passes[0].focus_offset_steps == 0);
  CHECK_NEAR(p.passes[0].start_mm, 0, 1e-12);
  CHECK_NEAR(p.passes[0].length_mm, 231, 1e-12);
  CHECK_NEAR(p.seconds, 2759.4038, 1e-3);

  /* the film height steps by 700 after frame 3: two passes */
  for(i = 3; i < 6; i++) fr[i].focus_offset_steps = 700;
  CHECK(ft_make_detail_plan(fr, 6, 3150, 231, FT_DETAIL_FOCUS_SPAN_STEPS, &p,
                            &e) == 0);
  CHECK(p.pass_count == 2);
  CHECK(p.passes[0].last_frame == 2 &&
        p.passes[1].first_frame == 3 &&
        p.passes[1].focus_offset_steps == 700);
  CHECK_NEAR(p.passes[0].length_mm, 117, 1e-12);
  CHECK_NEAR(p.passes[1].start_mm, 113, 1e-12);
  CHECK_NEAR(p.seconds, 2840.3864, 1e-3);

  /* the pass focus is the middle of its frames' offsets */
  fr[3].focus_offset_steps = 101;
  fr[4].focus_offset_steps = 600;
  fr[5].focus_offset_steps = 400;
  CHECK(ft_make_detail_plan(fr, 6, 3150, 231, FT_DETAIL_FOCUS_SPAN_STEPS, &p,
                            &e) == 0);
  CHECK(p.pass_count == 1 &&
        p.passes[0].focus_offset_steps == 300); /* (0 + 600) / 2 */
  fr[0].focus_offset_steps = fr[1].focus_offset_steps =
    fr[2].focus_offset_steps = -101;
  fr[3].focus_offset_steps = fr[4].focus_offset_steps =
    fr[5].focus_offset_steps = -100;
  CHECK(ft_make_detail_plan(fr, 6, 3150, 231, FT_DETAIL_FOCUS_SPAN_STEPS, &p,
                            &e) == 0);
  CHECK(p.passes[0].focus_offset_steps == -101); /* -100.5: away from 0 */

  /* 6300, frame 2 not selected: two passes beat one through it */
  strip(4);
  fr[1] = fr[2];
  fr[2] = fr[3];
  CHECK(ft_make_detail_plan(fr, 3, 6300, 231, FT_DETAIL_FOCUS_SPAN_STEPS, &p,
                            &e) == 0);
  CHECK(p.pass_count == 2);
  CHECK(p.passes[0].first_frame == 0 && p.passes[0].last_frame == 0);
  CHECK(p.passes[1].first_frame == 1 && p.passes[1].last_frame == 2);
  CHECK_NEAR(p.passes[0].seconds, 1001.3211, 1e-3);
  CHECK_NEAR(p.seconds, 2922.9809, 1e-3);

  /* span 0 and different heights: a pass per frame */
  strip(5);
  for(i = 0; i < 5; i++) fr[i].focus_offset_steps = i % 2 ? 700 : 0;
  CHECK(ft_make_detail_plan(fr, 5, 6300, 231, 0, &p, &e) == 0);
  CHECK(p.pass_count == 5);

  /* the DP equals brute force on random selections and heights */
  for(trial = 0; trial < 300; trial++) {
    int n = 0, all = 6;
    double ppi = trial % 2 ? 6300 : 3150;
    strip(all);
    for(i = 0; i < all; i++)
      if(rnd() % 3) {
        fr[n] = fr[i];
        fr[n].focus_offset_steps = (long)(rnd() % 1500) - 700;
        n++;
      }
    if(n == 0) continue;
    CHECK(ft_make_detail_plan(fr, n, ppi, 231, FT_DETAIL_FOCUS_SPAN_STEPS, &p,
                              &e) == 0);
    CHECK_NEAR(p.seconds, brute(fr, n, ppi, FT_DETAIL_FOCUS_SPAN_STEPS), 1e-6);
    { /* the passes cover the frames in order, each in focus */
      int k, next = 0;
      double sum = 0;
      for(k = 0; k < (int)p.pass_count; k++) {
        long lo = fr[p.passes[k].first_frame].focus_offset_steps, hi = lo;
        int m;
        CHECK((int)p.passes[k].first_frame == next);
        for(m = (int)p.passes[k].first_frame; m <= (int)p.passes[k].last_frame;
            m++) {
          if(fr[m].focus_offset_steps < lo) lo = fr[m].focus_offset_steps;
          if(fr[m].focus_offset_steps > hi) hi = fr[m].focus_offset_steps;
        }
        CHECK(hi - lo <= FT_DETAIL_FOCUS_SPAN_STEPS);
        next = p.passes[k].last_frame + 1;
        sum += p.passes[k].seconds;
      }
      CHECK(next == n);
      CHECK_NEAR(sum, p.seconds, 1e-6);
    }
  }

  /* refusals */
  strip(3);
  CHECK(ft_make_detail_plan(fr, 0, 3150, 231, 600, &p, &e) == -1);
  CHECK(ft_make_detail_plan(fr, 3, 1000, 231, 600, &p, &e) == -1); /* optics */
  CHECK(ft_make_detail_plan(fr, 3, 3150, 231, -1, &p, &e) == -1);
  fr[1].start_mm = 20; /* overlaps frame 1 */
  CHECK(ft_make_detail_plan(fr, 3, 3150, 231, 600, &p, &e) == -1);
  CHECK(strstr(e.message, "frame 2") != NULL);
  strip(3);
  fr[2].start_mm = 200; /* ends at 236 */
  CHECK(ft_make_detail_plan(fr, 3, 3150, 231, 600, &p, &e) == -1);

  /* a pass scan's numbers and a frame's ranges in it (3150, bin 1) */
  {
    ft_detail_scan s;
    ft_axes ax;
    double lo, hi;
    int r[4];
    ft_make_detail_scan(3150, 1, 0.5, 31.0, 113.0, 118.0, &s);
    CHECK(s.first_pixel == 1950 && s.first_line == 14014);
    CHECK(s.pixel_count == 3844 && s.line_count == 14634);
    /* frame 4 (116 + 36 mm along, 4.1 + 24 across) with margins */
    CHECK(ft_find_scan_ranges(&s, 113, 155, 1.1, 31.1, r) == 0);
    CHECK(r[0] == 0 && r[1] == 5196 && r[2] == 74 && r[3] == 3795);
    CHECK(ft_find_scan_ranges(&s, 300, 320, 1, 2, r) == -1);
    ft_get_ccd_reach_mm(3150, 1, &lo, &hi);
    CHECK_NEAR(lo, -15.22105, 1e-4);
    CHECK_NEAR(hi, 49.22238, 1e-4);
    CHECK(ft_get_thumbnail_factor(&s) == 6);
    ft_get_detail_axes(&s, 6, &ax);
    CHECK_NEAR(ax.first_line_mm, 113.118698, 1e-6);
    CHECK_NEAR(ax.mm_per_line, 0.0483810, 1e-7);
    CHECK_NEAR(ax.first_pixel_mm, 0.5229175, 1e-6);
    CHECK_NEAR(ax.mm_per_pixel, 0.0483810, 1e-7);
    CHECK(ft_get_scan_line(&s, 155) == 5196);
    CHECK(ft_get_scan_pixel(&s, 31.1) == 3795);
    ft_make_detail_scan(6300, 1, 0.5, 31.0, 0, 42, &s);
    CHECK(ft_get_thumbnail_factor(&s) == 12);
  }
  return TEST_DONE();
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ft_plan.h"
#include "ft_share.h"
#include "test/ftest.h"

static ft_share_frame fr[FT_MAX_FRAMES];

/* n frames of type t, len along, 24 mm across, every `pitch` mm from y0 */
static void strip(int n, ft_frame_type t, double y0, double len, double pitch) {
  int i;
  for(i = 0; i < n; i++) {
    fr[i].index = i + 1;
    fr[i].type = t;
    fr[i].along_start_mm = y0 + pitch * i;
    fr[i].length_mm = len;
    fr[i].across_start_mm = 4.1;
    fr[i].width_mm = 24;
  }
}

/* keep only the frames whose index is not in skip (0-terminated) */
static int select_all_but(int n, const int* skip, ft_share_frame* out) {
  int i, k, m = 0;
  for(i = 0; i < n; i++) {
    int drop = 0;
    for(k = 0; skip[k]; k++) drop |= (int)fr[i].index == skip[k];
    if(!drop) out[m++] = fr[i];
  }
  return m;
}

/* the cheapest grouping by trying every partition (n <= 12) */
static double brute(const ft_share_frame* f, int n, const ft_share_options* o,
                    double ppi, int bin) {
  double best = HUGE_VAL;
  unsigned mask, lim = 1u << (n - 1);
  for(mask = 0; mask < lim; mask++) {
    double t = 0;
    int i, start = 0;
    for(i = 0; i < n; i++)
      if(i == n - 1 || (mask >> i & 1)) { /* window ends at i */
        double a = f[start].along_start_mm - o->margin_mm;
        double b = f[i].along_start_mm + f[i].length_mm + o->margin_mm;
        if(a < 0) a = 0;
        if(b > o->scan_length_mm) b = o->scan_length_mm;
        t += ft_estimate_scan_seconds(ppi, bin, b - a);
        start = i + 1;
      }
    if(t < best) best = t;
  }
  return best;
}

int main(void) {
  ft_share_frame sel[FT_MAX_FRAMES];
  ft_share_options o;
  ft_share_estimate est, c[6];
  ft_err e;
  int n, i, k;

  ft_init_share_options(&o, 231.0);
  CHECK(o.target_pixels == 2000 &&
        o.margin_mm == 2.0 &&
        !o.half_frames_at_full_frame_ppi);

  /* long side and target: FF along, HF across; half-res HF = FF ppi */
  strip(1, FT_FRAME_FULL, 4, 36, 38);
  CHECK_NEAR(ft_get_share_long_side_mm(&fr[0], 0), 36, 1e-12);
  CHECK_NEAR(ft_get_share_ppi_needed(fr, 1, &o), 1412, 1e-9); /* 1411.1 up */
  CHECK_NEAR(ft_get_share_target_pixels(&fr[0], &o), 2000, 1e-12);
  strip(1, FT_FRAME_HALF, 4, 18, 19);
  CHECK_NEAR(ft_get_share_long_side_mm(&fr[0], 0), 24, 1e-12);
  CHECK_NEAR(ft_get_share_ppi_needed(fr, 1, &o), 2117, 1e-9); /* 2116.7 up */
  CHECK_NEAR(ft_get_share_target_pixels(&fr[0], &o), 2000, 1e-12);
  o.half_frames_at_full_frame_ppi = 1;
  CHECK_NEAR(ft_get_share_long_side_mm(&fr[0], 1), 36, 1e-12);
  CHECK_NEAR(ft_get_share_ppi_needed(fr, 1, &o), 1412, 1e-9); /* FF's ppi */
  CHECK_NEAR(ft_get_share_target_pixels(&fr[0], &o), 2000 * 24 / 36.0, 1e-9);
  o.half_frames_at_full_frame_ppi = 0;

  /* one FF frame at 1500 bin 1: one window, margins included */
  strip(1, FT_FRAME_FULL, 4, 36, 38);
  CHECK(ft_estimate_share(fr, 1, &o, 1500, 1, &ft_lens_default, &est, &e) == 0);
  CHECK(est.window_count == 1 &&
        est.windows[0].first_frame == 0 &&
        est.windows[0].last_frame == 0);
  CHECK_NEAR(est.windows[0].start_mm, 2, 1e-12);
  CHECK_NEAR(est.windows[0].length_mm, 40, 1e-12);
  /* 10 s + (40 mm at 1500 ppi + 250 cal lines) x 7992 px / 84000 px/s */
  CHECK_NEAR(est.scan_seconds, 10 + (40 / 25.4 * 1500 + 250) * 7992 / 84000.0,
             1e-9);
  CHECK_NEAR(est.scan_seconds, 258.5, 0.1);
  /* one window: one focus bracket */
  CHECK_NEAR(est.focus_seconds, FT_SHARE_FOCUS_CROPS * FT_FOCUS_SAMPLE_SECONDS,
             1e-12);
  CHECK_NEAR(est.seconds, est.scan_seconds + est.focus_seconds, 1e-12);
  CHECK_NEAR(est.relative_light,
             ft_estimate_relative_light(&ft_lens_default, 1500), 1e-12);
  CHECK_NEAR(est.dmax, ft_estimate_dmax_for_light(est.relative_light), 1e-12);
  /* window clipped to the holder at the strip start */
  strip(1, FT_FRAME_FULL, 1, 36, 38);
  CHECK(ft_estimate_share(fr, 1, &o, 1500, 1, &ft_lens_default, &est, &e) == 0);
  CHECK_NEAR(est.windows[0].start_mm, 0, 1e-12);
  CHECK_NEAR(est.windows[0].length_mm, 39, 1e-12);

  /* 12 half frames, 2 and 7 skipped: windows {1}, {3-6}, {8-12} */
  {
    static const int skip[] = {2, 7, 0};
    strip(12, FT_FRAME_HALF, 3, 18, 19);
    n = select_all_but(12, skip, sel);
    CHECK(n == 10);
    CHECK(ft_estimate_share(sel, n, &o, 1500, 1, &ft_lens_default, &est, &e) ==
          0);
    CHECK(est.window_count == 3);
    /* one focus bracket for the strip: windows 2 and 3 carry it */
    CHECK_NEAR(est.focus_seconds,
               FT_SHARE_FOCUS_CROPS * FT_FOCUS_SAMPLE_SECONDS, 1e-12);
    CHECK(sel[est.windows[0].first_frame].index == 1 &&
          sel[est.windows[0].last_frame].index == 1);
    CHECK(sel[est.windows[1].first_frame].index == 3 &&
          sel[est.windows[1].last_frame].index == 6);
    CHECK(sel[est.windows[2].first_frame].index == 8 &&
          sel[est.windows[2].last_frame].index == 12);
    CHECK_NEAR(est.scan_seconds, brute(sel, n, &o, 1500, 1), 1e-6);
    /* cheaper than one pass through all twelve */
    CHECK(est.scan_seconds <
          ft_estimate_scan_seconds(1500, 1, 12 * 19 - 1 + 4));
  }
  /* touching frames stay one window */
  strip(6, FT_FRAME_FULL, 4, 36, 38);
  CHECK(ft_estimate_share(fr, 6, &o, 1500, 1, &ft_lens_default, &est, &e) == 0);
  CHECK(est.window_count == 1 &&
        est.windows[0].first_frame == 0 &&
        est.windows[0].last_frame == 5);

  /* the DP equals brute force on random selections, several optics */
  srand(11);
  for(k = 0; k < 300; k++) {
    static const struct {
      double ppi;
      int bin;
    } opt[] = {{1500, 1}, {1412, 2}, {1412, 4}, {2117, 1}};
    int which = k % 4;
    strip(12, k % 2 ? FT_FRAME_HALF : FT_FRAME_FULL, 3, k % 2 ? 18 : 36,
          k % 2 ? 19 : 38);
    for(i = n = 0; i < (k % 2 ? 12 : 6); i++)
      if(rand() % 3) sel[n++] = fr[i];
    if(!n) continue;
    CHECK(ft_estimate_share(sel, n, &o, opt[which].ppi, opt[which].bin,
                            &ft_lens_default, &est, &e) == 0);
    CHECK_NEAR(est.scan_seconds,
               brute(sel, n, &o, opt[which].ppi, opt[which].bin), 1e-6);
    /* windows cover every frame once, in order */
    CHECK(est.windows[0].first_frame == 0 &&
          est.windows[est.window_count - 1].last_frame == (uint32_t)n - 1);
    for(i = 1; i < (int)est.window_count; i++)
      CHECK(est.windows[i].first_frame == est.windows[i - 1].last_frame + 1);
  }

  /* candidates for FF: bins 4, 3, 2 at 1412 ppi, bin 1 at 1500 */
  strip(6, FT_FRAME_FULL, 4, 36, 38);
  n = ft_find_share_candidates(fr, 6, &o, &ft_lens_default, c, &e);
  CHECK(n == 4);
  CHECK(c[0].bin == 4 && c[1].bin == 3 && c[2].bin == 2 && c[3].bin == 1);
  CHECK_NEAR(c[0].ppi, 1412, 1e-9);
  CHECK_NEAR(c[2].ppi, 1412, 1e-9);
  CHECK_NEAR(c[3].ppi, 1500, 1e-9);
  for(i = 0; i < n; i++) {
    uint32_t b[6];
    uint32_t nb = ft_find_bins_for_ppi(c[i].ppi, b);
    int ok = 0;
    while(nb--) ok |= (int32_t)b[nb] == c[i].bin;
    CHECK(ok);                                   /* a bin ft_plan accepts */
    CHECK(i == 0 || c[i].dmax >= c[i - 1].dmax); /* less magnif. */
  }
  for(i = 0; i < n; i++) { /* one bracket a strip: later windows carry it */
    CHECK_NEAR(c[i].focus_seconds,
               FT_SHARE_FOCUS_CROPS * FT_FOCUS_SAMPLE_SECONDS, 1e-9);
  }
  CHECK(c[0].scan_seconds < c[3].scan_seconds); /* larger bin is faster */
  k = ft_pick_most_light(c, n);
  CHECK(c[k].bin == 1 && c[k].ppi == 1500);
  /* half-res half frames get the same optics as full frames */
  strip(12, FT_FRAME_HALF, 3, 18, 19);
  o.half_frames_at_full_frame_ppi = 1;
  n = ft_find_share_candidates(fr, 12, &o, &ft_lens_default, c, &e);
  k = ft_pick_most_light(c, n);
  CHECK(c[k].bin == 1 && c[k].ppi == 1500);
  o.half_frames_at_full_frame_ppi = 0;
  n = ft_find_share_candidates(fr, 12, &o, &ft_lens_default, c, &e);
  k = ft_pick_most_light(c, n);
  CHECK(c[k].bin == 1 && c[k].ppi == 2117);
  /* mixed full and half frames: the half frames set the ppi */
  strip(2, FT_FRAME_FULL, 4, 36, 38);
  fr[2] = fr[1];
  fr[2].index = 3;
  fr[2].type = FT_FRAME_HALF;
  fr[2].along_start_mm = 80;
  fr[2].length_mm = 18;
  CHECK_NEAR(ft_get_share_ppi_needed(fr, 3, &o), 2117, 1e-9);
  o.half_frames_at_full_frame_ppi = 1;
  CHECK_NEAR(ft_get_share_ppi_needed(fr, 3, &o), 1412, 1e-9);
  o.half_frames_at_full_frame_ppi = 0;
  /* ties in Dmax go to the faster candidate */
  memset(c, 0, 2 * sizeof c[0]);
  c[0].dmax = c[1].dmax = 3.73;
  c[0].seconds = 100;
  c[1].seconds = 90;
  CHECK(ft_pick_most_light(c, 2) == 1);

  /* jobs: select, apply the plan, the group's window */
  {
    uint32_t selected_count = 0;
    static ft_job j;
    double y0, len;
    memset(&j, 0, sizeof j);
    strcpy(j.holder, "24x217");
    j.frame_count = 12;
    for(i = 0; i < 12; i++) {
      j.frames[i].index = i + 1;
      j.frames[i].type = i == 11 ? FT_FRAME_PARTIAL : FT_FRAME_HALF;
      j.frames[i].is_included = 1;
      j.frames[i].ppi = 2000;
      j.frames[i].along_start_mm = 3 + 19.0 * i;
      j.frames[i].length_mm = 18;
      j.frames[i].across_start_mm = 4;
      j.frames[i].width_mm = 24;
    }
    /* default: every frame but the partial one */
    CHECK(ft_select_share_frames(&j, NULL, sel, &selected_count, &e) == 0 &&
          selected_count == 11);
    CHECK(sel[10].index == 11);
    CHECK(ft_select_share_frames(&j, "", sel, &selected_count, &e) == 0 &&
          selected_count == 11);
    CHECK(ft_select_share_frames(&j, "1,3-6,8-12", sel, &selected_count, &e) ==
          0);
    CHECK(selected_count == 10 &&
          sel[0].index == 1 &&
          sel[1].index == 3 &&
          sel[9].index == 12);
    CHECK_NEAR(sel[1].along_start_mm, 41, 1e-12);
    CHECK(sel[1].type == FT_FRAME_HALF && sel[1].width_mm == 24);
    CHECK(ft_select_share_frames(&j, "4,2", sel, &selected_count, &e) ==
          0); /* sorted */
    CHECK(selected_count == 2 && sel[0].index == 2 && sel[1].index == 4);
    CHECK(ft_select_share_frames(&j, "13", sel, &selected_count, &e) == -1);
    CHECK(strstr(e.message, "no frame 13") != NULL);
    CHECK(ft_select_share_frames(&j, "3-", sel, &selected_count, &e) == -1);
    CHECK(ft_select_share_frames(&j, "5-3", sel, &selected_count, &e) == -1);
    CHECK(ft_select_share_frames(&j, "1,,2", sel, &selected_count, &e) == -1);
    CHECK(ft_select_share_frames(&j, "1;2", sel, &selected_count, &e) == -1);
    CHECK(ft_select_share_frames(&j, " 1", sel, &selected_count, &e) == -1);
    for(i = 0; i < 12; i++) j.frames[i].type = FT_FRAME_PARTIAL;
    CHECK(ft_select_share_frames(&j, NULL, sel, &selected_count, &e) == -1);
    CHECK(strstr(e.message, "no frames") != NULL);
    for(i = 0; i < 12; i++) j.frames[i].type = FT_FRAME_HALF;

    /* plan and apply: skip 2 and 7 -> groups 1, 2, 3 */
    CHECK(ft_select_share_frames(&j, "1,3-6,8-12", sel, &selected_count, &e) ==
          0);
    o.half_frames_at_full_frame_ppi = 1;
    k = ft_find_share_candidates(sel, selected_count, &o, &ft_lens_default, c,
                                 &e);
    k = ft_pick_most_light(c, k);
    ft_apply_share_plan(&j, sel, selected_count, &o, &c[k]);
    CHECK(j.share_pixels == 2000 && j.half_frames_at_full_frame_ppi == 1);
    CHECK(!j.frames[1].is_included && j.frames[1].group == 0);
    CHECK(!j.frames[6].is_included && j.frames[6].group == 0);
    CHECK(j.frames[0].is_included && j.frames[0].group == 1);
    CHECK(j.frames[2].group == 2 && j.frames[5].group == 2);
    CHECK(j.frames[7].group == 3 && j.frames[11].group == 3);
    CHECK(j.frames[0].ppi == 1500 && j.frames[0].bin == 1);
    CHECK(ft_validate_job(&j, &ft_lens_default, &e) == 0);
    /* windows match the estimate's */
    for(i = 0; i < (int)c[k].window_count; i++) {
      CHECK(ft_get_share_group_window(&j, i + 1, &o, &y0, &len) == 0);
      CHECK_NEAR(y0, c[k].windows[i].start_mm, 1e-12);
      CHECK_NEAR(len, c[k].windows[i].length_mm, 1e-12);
    }
    CHECK(ft_get_share_group_window(&j, 4, &o, &y0, &len) == -1);
    o.half_frames_at_full_frame_ppi = 0;

    /* sizes per frame (the web page): one size is the plan above */
    {
      static ft_job s;
      double tg[FT_MAX_FRAMES];
      memset(&s, 0, sizeof s);
      strcpy(s.holder, "24x217");
      s.frame_count = 12;
      for(i = 0; i < 12; i++) {
        s.frames[i].index = i + 1;
        s.frames[i].type = FT_FRAME_HALF;
        s.frames[i].is_included = 1;
        s.frames[i].ppi = 2000;
        s.frames[i].along_start_mm = 3 + 19.0 * i;
        s.frames[i].length_mm = 18;
        s.frames[i].across_start_mm = 4;
        s.frames[i].width_mm = 24;
        tg[i] = i == 1 || i == 6 ? 0 : 2000;
      }
      s.focus_mode = FT_FOCUS_SHIFTED;
      s.focus_offset_steps = 700;
      CHECK(ft_plan_share_sizes(&s, tg, 1, &ft_lens_default, o.scan_length_mm,
                                &e) == 3);
      CHECK(s.share_pixels == j.share_pixels &&
            s.half_frames_at_full_frame_ppi == j.half_frames_at_full_frame_ppi);
      CHECK(s.focus_mode == FT_FOCUS_PER_FRAME && s.focus_offset_steps == 0);
      for(i = 0; i < 12; i++)
        CHECK(s.frames[i].is_included == j.frames[i].is_included &&
              s.frames[i].group == j.frames[i].group &&
              s.frames[i].ppi == j.frames[i].ppi &&
              s.frames[i].bin == j.frames[i].bin &&
              s.frames[i].share_pixels == 0);

      /* mixed, on full frames: 1-2 4MP, 3 6MP, 4 skipped, 5 4MP */
      s.frame_count = 5; /* 5 full frames fit */
      for(i = 0; i < 5; i++) {
        s.frames[i].type = FT_FRAME_FULL;
        s.frames[i].length_mm = 36;
        s.frames[i].along_start_mm = 4 + 38.0 * i;
      }
      tg[0] = tg[1] = FT_SHARE_4MP_PIXELS;
      tg[2] = FT_SHARE_6MP_PIXELS;
      tg[3] = 0; /* skipped */
      tg[4] = FT_SHARE_4MP_PIXELS;
      n =
        ft_plan_share_sizes(&s, tg, 0, &ft_lens_default, o.scan_length_mm, &e);
      CHECK(n >= 3);
      CHECK(s.share_pixels == FT_SHARE_4MP_PIXELS &&
            s.half_frames_at_full_frame_ppi == 0);
      CHECK(s.frames[0].share_pixels == 0 &&
            s.frames[1].share_pixels == 0 &&
            s.frames[2].share_pixels == FT_SHARE_6MP_PIXELS &&
            s.frames[4].share_pixels == 0);
      CHECK(!s.frames[3].is_included &&
            s.frames[3].group == 0 &&
            s.frames[3].share_pixels == 0);
      /* runs never share a window; groups follow the strip */
      CHECK(s.frames[0].group >= 1 && s.frames[1].group >= s.frames[0].group);
      CHECK(s.frames[2].group > s.frames[1].group &&
            s.frames[4].group > s.frames[2].group);
      CHECK(s.frames[2].ppi > s.frames[0].ppi);
      CHECK(ft_get_frame_share_pixels(&s, &s.frames[2]) == FT_SHARE_6MP_PIXELS);
      CHECK(ft_get_frame_share_pixels(&s, &s.frames[4]) == FT_SHARE_4MP_PIXELS);
      CHECK(ft_validate_job(&s, &ft_lens_default, &e) == 0);
      /* the smallest size is the strip's share */
      tg[0] = tg[1] = tg[4] = FT_SHARE_6MP_PIXELS;
      tg[2] = FT_SHARE_4MP_PIXELS;
      CHECK(ft_plan_share_sizes(&s, tg, 0, &ft_lens_default, o.scan_length_mm,
                                &e) >= 3);
      CHECK(s.share_pixels == FT_SHARE_4MP_PIXELS &&
            s.frames[2].share_pixels == 0 &&
            s.frames[0].share_pixels == FT_SHARE_6MP_PIXELS);
      /* refusals */
      for(i = 0; i < 5; i++) tg[i] = 0;
      CHECK(ft_plan_share_sizes(&s, tg, 0, &ft_lens_default, o.scan_length_mm,
                                &e) == -1);
      CHECK(strcmp(e.message, "share: no frames selected") == 0);
      tg[1] = 50;
      CHECK(ft_plan_share_sizes(&s, tg, 0, &ft_lens_default, o.scan_length_mm,
                                &e) == -1);
      CHECK(strcmp(e.message,
                   "share: frame 2: 50 px is out of range "
                   "(100..20000)") == 0);
    }
  }
  /* refusals */
  strip(2, FT_FRAME_FULL, 4, 36, 38);
  CHECK(ft_estimate_share(fr, 0, &o, 1500, 1, &ft_lens_default, &est, &e) ==
        -1);
  CHECK(ft_estimate_share(fr, 2, &o, 1000, 1, &ft_lens_default, &est, &e) ==
        -1); /* optics 1000 */
  CHECK(strstr(e.message, "optics") != NULL);
  CHECK(ft_estimate_share(fr, 2, &o, 1500, 5, &ft_lens_default, &est, &e) ==
        -1); /* 12 % 5 */
  sel[0] = fr[1];
  sel[1] = fr[0]; /* out of order */
  CHECK(ft_estimate_share(sel, 2, &o, 1500, 1, &ft_lens_default, &est, &e) ==
        -1);
  CHECK(strstr(e.message, "frame 1") != NULL);
  fr[1].along_start_mm = 220; /* past the holder */
  CHECK(ft_estimate_share(fr, 2, &o, 1500, 1, &ft_lens_default, &est, &e) ==
        -1);
  CHECK(strstr(e.message, "outside") != NULL);
  CHECK(ft_find_share_candidates(fr, 0, &o, &ft_lens_default, c, &e) == -1);
  return TEST_DONE();
}

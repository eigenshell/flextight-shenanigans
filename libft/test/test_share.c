/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "ft_plan.h"
#include "ft_share.h"
#include "test/ftest.h"

static ft_share_frame fr[FT_MAX_FRAMES];

/* n frames of type t, len along, 24 mm across, every `pitch` mm from y0 */
static void strip(int n, ft_frame_type t, double y0, double len, double pitch)
{
    int i;
    for (i = 0; i < n; i++) {
        fr[i].index = i + 1;
        fr[i].type = t;
        fr[i].y0 = y0 + pitch * i;
        fr[i].len = len;
        fr[i].x0 = 4.1;
        fr[i].width = 24;
    }
}

/* keep only the frames whose index is not in skip (0-terminated) */
static int select_all_but(int n, const int *skip, ft_share_frame *out)
{
    int i, k, m = 0;
    for (i = 0; i < n; i++) {
        int drop = 0;
        for (k = 0; skip[k]; k++)
            drop |= fr[i].index == skip[k];
        if (!drop)
            out[m++] = fr[i];
    }
    return m;
}

/* the cheapest grouping by trying every partition (n <= 12) */
static double brute(const ft_share_frame *f, int n, const ft_share_opts *o,
                    double ppi, int bin)
{
    double best = HUGE_VAL;
    unsigned mask, lim = 1u << (n - 1);
    for (mask = 0; mask < lim; mask++) {
        double t = 0;
        int i, start = 0;
        for (i = 0; i < n; i++)
            if (i == n - 1 || (mask >> i & 1)) {   /* window ends at i */
                double a = f[start].y0 - o->margin_mm;
                double b = f[i].y0 + f[i].len + o->margin_mm;
                if (a < 0) a = 0;
                if (b > o->scan_len_mm) b = o->scan_len_mm;
                t += ft_scan_seconds(ppi, bin, b - a);
                start = i + 1;
            }
        if (t < best)
            best = t;
    }
    return best;
}

int main(void)
{
    ft_share_frame sel[FT_MAX_FRAMES];
    ft_share_opts o;
    ft_share_est est, c[6];
    ft_err e;
    int n, i, k;

    ft_share_opts_default(&o, 231.0);
    CHECK(o.target_px == 2000 && o.margin_mm == 2.0 && !o.hf_half_res);

    /* long side and target: FF along, HF across; half-res HF = FF ppi */
    strip(1, FT_FRAME_FULL, 4, 36, 38);
    CHECK_NEAR(ft_share_long_mm(&fr[0], 0), 36, 1e-12);
    CHECK_NEAR(ft_share_ppi_needed(fr, 1, &o), 1412, 1e-9);   /* 1411.1 up */
    CHECK_NEAR(ft_share_target_px(&fr[0], &o), 2000, 1e-12);
    strip(1, FT_FRAME_HALF, 4, 18, 19);
    CHECK_NEAR(ft_share_long_mm(&fr[0], 0), 24, 1e-12);
    CHECK_NEAR(ft_share_ppi_needed(fr, 1, &o), 2117, 1e-9);   /* 2116.7 up */
    CHECK_NEAR(ft_share_target_px(&fr[0], &o), 2000, 1e-12);
    o.hf_half_res = 1;
    CHECK_NEAR(ft_share_long_mm(&fr[0], 1), 36, 1e-12);
    CHECK_NEAR(ft_share_ppi_needed(fr, 1, &o), 1412, 1e-9);   /* FF's ppi */
    CHECK_NEAR(ft_share_target_px(&fr[0], &o), 2000 * 24 / 36.0, 1e-9);
    o.hf_half_res = 0;

    /* one FF frame at 1500 bin 1: one window, margins included */
    strip(1, FT_FRAME_FULL, 4, 36, 38);
    CHECK(ft_share_estimate(fr, 1, &o, 1500, 1, &ft_lens_default, &est,
                            &e) == 0);
    CHECK(est.nwin == 1 && est.win[0].first == 0 && est.win[0].last == 0);
    CHECK_NEAR(est.win[0].y0, 2, 1e-12);
    CHECK_NEAR(est.win[0].len, 40, 1e-12);
    /* 10 s + (40 mm at 1500 ppi + 250 cal lines) x 7992 px / 84000 px/s */
    CHECK_NEAR(est.scan_s, 10 + (40 / 25.4 * 1500 + 250) * 7992 / 84000.0,
               1e-9);
    CHECK_NEAR(est.scan_s, 258.5, 0.1);
    /* one window: one focus bracket */
    CHECK_NEAR(est.focus_s, FT_SHARE_FOCUS_CROPS * FT_FOCUS_SAMPLE_S, 1e-12);
    CHECK_NEAR(est.seconds, est.scan_s + est.focus_s, 1e-12);
    CHECK_NEAR(est.light, ft_light_rel(&ft_lens_default, 1500), 1e-12);
    CHECK_NEAR(est.dmax, ft_dmax_est(est.light), 1e-12);
    /* window clipped to the holder at the strip start */
    strip(1, FT_FRAME_FULL, 1, 36, 38);
    CHECK(ft_share_estimate(fr, 1, &o, 1500, 1, &ft_lens_default, &est,
                            &e) == 0);
    CHECK_NEAR(est.win[0].y0, 0, 1e-12);
    CHECK_NEAR(est.win[0].len, 39, 1e-12);

    /* 12 half frames, 2 and 7 skipped: windows {1}, {3-6}, {8-12} */
    {
        static const int skip[] = { 2, 7, 0 };
        strip(12, FT_FRAME_HALF, 3, 18, 19);
        n = select_all_but(12, skip, sel);
        CHECK(n == 10);
        CHECK(ft_share_estimate(sel, n, &o, 1500, 1, &ft_lens_default,
                                &est, &e) == 0);
        CHECK(est.nwin == 3);
        /* one focus bracket for the strip: windows 2 and 3 carry it */
        CHECK_NEAR(est.focus_s, FT_SHARE_FOCUS_CROPS * FT_FOCUS_SAMPLE_S,
                   1e-12);
        CHECK(sel[est.win[0].first].index == 1 &&
              sel[est.win[0].last].index == 1);
        CHECK(sel[est.win[1].first].index == 3 &&
              sel[est.win[1].last].index == 6);
        CHECK(sel[est.win[2].first].index == 8 &&
              sel[est.win[2].last].index == 12);
        CHECK_NEAR(est.scan_s, brute(sel, n, &o, 1500, 1), 1e-6);
        /* cheaper than one pass through all twelve */
        CHECK(est.scan_s < ft_scan_seconds(1500, 1, 12 * 19 - 1 + 4));
    }
    /* touching frames stay one window */
    strip(6, FT_FRAME_FULL, 4, 36, 38);
    CHECK(ft_share_estimate(fr, 6, &o, 1500, 1, &ft_lens_default, &est,
                            &e) == 0);
    CHECK(est.nwin == 1 && est.win[0].first == 0 && est.win[0].last == 5);

    /* the DP equals brute force on random selections, several optics */
    srand(11);
    for (k = 0; k < 300; k++) {
        static const struct { double ppi; int bin; } opt[] = {
            { 1500, 1 }, { 1412, 2 }, { 1412, 4 }, { 2117, 1 } };
        int which = k % 4;
        strip(12, k % 2 ? FT_FRAME_HALF : FT_FRAME_FULL, 3,
              k % 2 ? 18 : 36, k % 2 ? 19 : 38);
        for (i = n = 0; i < (k % 2 ? 12 : 6); i++)
            if (rand() % 3)
                sel[n++] = fr[i];
        if (!n)
            continue;
        CHECK(ft_share_estimate(sel, n, &o, opt[which].ppi, opt[which].bin,
                                &ft_lens_default, &est, &e) == 0);
        CHECK_NEAR(est.scan_s, brute(sel, n, &o, opt[which].ppi,
                                     opt[which].bin), 1e-6);
        /* windows cover every frame once, in order */
        CHECK(est.win[0].first == 0 && est.win[est.nwin - 1].last == n - 1);
        for (i = 1; i < est.nwin; i++)
            CHECK(est.win[i].first == est.win[i - 1].last + 1);
    }

    /* candidates for FF: bins 4, 3, 2 at 1412 ppi, bin 1 at 1500 */
    strip(6, FT_FRAME_FULL, 4, 36, 38);
    n = ft_share_candidates(fr, 6, &o, &ft_lens_default, c, &e);
    CHECK(n == 4);
    CHECK(c[0].bin == 4 && c[1].bin == 3 && c[2].bin == 2 && c[3].bin == 1);
    CHECK_NEAR(c[0].ppi, 1412, 1e-9);
    CHECK_NEAR(c[2].ppi, 1412, 1e-9);
    CHECK_NEAR(c[3].ppi, 1500, 1e-9);
    for (i = 0; i < n; i++) {
        int b[6], nb = ft_bins_for_ppi(c[i].ppi, b), ok = 0;
        while (nb--)
            ok |= b[nb] == c[i].bin;
        CHECK(ok);                              /* a bin ft_plan accepts */
        CHECK(i == 0 || c[i].dmax >= c[i - 1].dmax);   /* less magnif. */
    }
    for (i = 0; i < n; i++) {   /* one bracket a strip: later windows carry it */
        CHECK_NEAR(c[i].focus_s, FT_SHARE_FOCUS_CROPS * FT_FOCUS_SAMPLE_S,
                   1e-9);
    }
    CHECK(c[0].scan_s < c[3].scan_s);           /* larger bin is faster */
    k = ft_share_pick_max_dmax(c, n);
    CHECK(c[k].bin == 1 && c[k].ppi == 1500);
    /* half-res half frames get the same optics as full frames */
    strip(12, FT_FRAME_HALF, 3, 18, 19);
    o.hf_half_res = 1;
    n = ft_share_candidates(fr, 12, &o, &ft_lens_default, c, &e);
    k = ft_share_pick_max_dmax(c, n);
    CHECK(c[k].bin == 1 && c[k].ppi == 1500);
    o.hf_half_res = 0;
    n = ft_share_candidates(fr, 12, &o, &ft_lens_default, c, &e);
    k = ft_share_pick_max_dmax(c, n);
    CHECK(c[k].bin == 1 && c[k].ppi == 2117);
    /* mixed full and half frames: the half frames set the ppi */
    strip(2, FT_FRAME_FULL, 4, 36, 38);
    fr[2] = fr[1];
    fr[2].index = 3;
    fr[2].type = FT_FRAME_HALF;
    fr[2].y0 = 80;
    fr[2].len = 18;
    CHECK_NEAR(ft_share_ppi_needed(fr, 3, &o), 2117, 1e-9);
    o.hf_half_res = 1;
    CHECK_NEAR(ft_share_ppi_needed(fr, 3, &o), 1412, 1e-9);
    o.hf_half_res = 0;
    /* ties in Dmax go to the faster candidate */
    memset(c, 0, 2 * sizeof c[0]);
    c[0].dmax = c[1].dmax = 3.73;
    c[0].seconds = 100;
    c[1].seconds = 90;
    CHECK(ft_share_pick_max_dmax(c, 2) == 1);

    /* jobs: select, apply the plan, the group's window */
    {
        static ft_job j;
        double y0, len;
        memset(&j, 0, sizeof j);
        strcpy(j.holder, "24x217");
        j.nframes = 12;
        for (i = 0; i < 12; i++) {
            j.f[i].index = i + 1;
            j.f[i].type = i == 11 ? FT_FRAME_PARTIAL : FT_FRAME_HALF;
            j.f[i].include = 1;
            j.f[i].ppi = 2000;
            j.f[i].y0 = 3 + 19.0 * i;
            j.f[i].len = 18;
            j.f[i].x0 = 4;
            j.f[i].width = 24;
        }
        /* default: every frame but the partial one */
        CHECK(ft_share_select(&j, NULL, sel, &n, &e) == 0 && n == 11);
        CHECK(sel[10].index == 11);
        CHECK(ft_share_select(&j, "", sel, &n, &e) == 0 && n == 11);
        CHECK(ft_share_select(&j, "1,3-6,8-12", sel, &n, &e) == 0);
        CHECK(n == 10 && sel[0].index == 1 && sel[1].index == 3 &&
              sel[9].index == 12);
        CHECK_NEAR(sel[1].y0, 41, 1e-12);
        CHECK(sel[1].type == FT_FRAME_HALF && sel[1].width == 24);
        CHECK(ft_share_select(&j, "4,2", sel, &n, &e) == 0);    /* sorted */
        CHECK(n == 2 && sel[0].index == 2 && sel[1].index == 4);
        CHECK(ft_share_select(&j, "13", sel, &n, &e) == -1);
        CHECK(strstr(e.msg, "no frame 13") != NULL);
        CHECK(ft_share_select(&j, "3-", sel, &n, &e) == -1);
        CHECK(ft_share_select(&j, "5-3", sel, &n, &e) == -1);
        CHECK(ft_share_select(&j, "1,,2", sel, &n, &e) == -1);
        CHECK(ft_share_select(&j, "1;2", sel, &n, &e) == -1);
        CHECK(ft_share_select(&j, " 1", sel, &n, &e) == -1);
        for (i = 0; i < 12; i++)
            j.f[i].type = FT_FRAME_PARTIAL;
        CHECK(ft_share_select(&j, NULL, sel, &n, &e) == -1);
        CHECK(strstr(e.msg, "no frames") != NULL);
        for (i = 0; i < 12; i++)
            j.f[i].type = FT_FRAME_HALF;

        /* plan and apply: skip 2 and 7 -> groups 1, 2, 3 */
        CHECK(ft_share_select(&j, "1,3-6,8-12", sel, &n, &e) == 0);
        o.hf_half_res = 1;
        k = ft_share_candidates(sel, n, &o, &ft_lens_default, c, &e);
        k = ft_share_pick_max_dmax(c, k);
        ft_share_apply(&j, sel, n, &o, &c[k]);
        CHECK(j.share == 2000 && j.hf_half_res == 1);
        CHECK(!j.f[1].include && j.f[1].group == 0);
        CHECK(!j.f[6].include && j.f[6].group == 0);
        CHECK(j.f[0].include && j.f[0].group == 1);
        CHECK(j.f[2].group == 2 && j.f[5].group == 2);
        CHECK(j.f[7].group == 3 && j.f[11].group == 3);
        CHECK(j.f[0].ppi == 1500 && j.f[0].bin == 1);
        CHECK(ft_job_validate(&j, &ft_lens_default, &e) == 0);
        /* windows match the estimate's */
        for (i = 0; i < c[k].nwin; i++) {
            CHECK(ft_share_group_window(&j, i + 1, &o, &y0, &len) == 0);
            CHECK_NEAR(y0, c[k].win[i].y0, 1e-12);
            CHECK_NEAR(len, c[k].win[i].len, 1e-12);
        }
        CHECK(ft_share_group_window(&j, 4, &o, &y0, &len) == -1);
        o.hf_half_res = 0;

        /* sizes per frame (the web page): one size is the plan above */
        {
            static ft_job s;
            double tg[FT_MAX_FRAMES];
            memset(&s, 0, sizeof s);
            strcpy(s.holder, "24x217");
            s.nframes = 12;
            for (i = 0; i < 12; i++) {
                s.f[i].index = i + 1;
                s.f[i].type = FT_FRAME_HALF;
                s.f[i].include = 1;
                s.f[i].ppi = 2000;
                s.f[i].y0 = 3 + 19.0 * i;
                s.f[i].len = 18;
                s.f[i].x0 = 4;
                s.f[i].width = 24;
                tg[i] = i == 1 || i == 6 ? 0 : 2000;
            }
            s.focus = FT_FOCUS_SHIFTED;
            s.focus_offset = 700;
            CHECK(ft_share_plan_sizes(&s, tg, 1, &ft_lens_default,
                                      o.scan_len_mm, &e) == 3);
            CHECK(s.share == j.share && s.hf_half_res == j.hf_half_res);
            CHECK(s.focus == FT_FOCUS_PER_FRAME && s.focus_offset == 0);
            for (i = 0; i < 12; i++)
                CHECK(s.f[i].include == j.f[i].include &&
                      s.f[i].group == j.f[i].group &&
                      s.f[i].ppi == j.f[i].ppi && s.f[i].bin == j.f[i].bin &&
                      s.f[i].share_px == 0);

            /* mixed, on full frames: 1-2 4MP, 3 6MP, 4 skipped, 5 4MP */
            s.nframes = 5;                          /* 5 full frames fit */
            for (i = 0; i < 5; i++) {
                s.f[i].type = FT_FRAME_FULL;
                s.f[i].len = 36;
                s.f[i].y0 = 4 + 38.0 * i;
            }
            tg[0] = tg[1] = FT_SHARE_4MP;
            tg[2] = FT_SHARE_6MP;
            tg[3] = 0;                              /* skipped */
            tg[4] = FT_SHARE_4MP;
            n = ft_share_plan_sizes(&s, tg, 0, &ft_lens_default,
                                    o.scan_len_mm, &e);
            CHECK(n >= 3);
            CHECK(s.share == FT_SHARE_4MP && s.hf_half_res == 0);
            CHECK(s.f[0].share_px == 0 && s.f[1].share_px == 0 &&
                  s.f[2].share_px == FT_SHARE_6MP && s.f[4].share_px == 0);
            CHECK(!s.f[3].include && s.f[3].group == 0 &&
                  s.f[3].share_px == 0);
            /* runs never share a window; groups follow the strip */
            CHECK(s.f[0].group >= 1 && s.f[1].group >= s.f[0].group);
            CHECK(s.f[2].group > s.f[1].group && s.f[4].group > s.f[2].group);
            CHECK(s.f[2].ppi > s.f[0].ppi);
            CHECK(ft_job_share_px(&s, &s.f[2]) == FT_SHARE_6MP);
            CHECK(ft_job_share_px(&s, &s.f[4]) == FT_SHARE_4MP);
            CHECK(ft_job_validate(&s, &ft_lens_default, &e) == 0);
            /* the smallest size is the strip's share */
            tg[0] = tg[1] = tg[4] = FT_SHARE_6MP;
            tg[2] = FT_SHARE_4MP;
            CHECK(ft_share_plan_sizes(&s, tg, 0, &ft_lens_default,
                                      o.scan_len_mm, &e) >= 3);
            CHECK(s.share == FT_SHARE_4MP && s.f[2].share_px == 0 &&
                  s.f[0].share_px == FT_SHARE_6MP);
            /* refusals */
            for (i = 0; i < 5; i++)
                tg[i] = 0;
            CHECK(ft_share_plan_sizes(&s, tg, 0, &ft_lens_default,
                                      o.scan_len_mm, &e) == -1);
            CHECK(strcmp(e.msg, "share: no frames selected") == 0);
            tg[1] = 50;
            CHECK(ft_share_plan_sizes(&s, tg, 0, &ft_lens_default,
                                      o.scan_len_mm, &e) == -1);
            CHECK(strcmp(e.msg, "share: frame 2: 50 px is out of range "
                                "(100..20000)") == 0);
        }
    }
    /* refusals */
    strip(2, FT_FRAME_FULL, 4, 36, 38);
    CHECK(ft_share_estimate(fr, 0, &o, 1500, 1, &ft_lens_default, &est,
                            &e) == -1);
    CHECK(ft_share_estimate(fr, 2, &o, 1000, 1, &ft_lens_default, &est,
                            &e) == -1);                 /* optics 1000 */
    CHECK(strstr(e.msg, "optics") != NULL);
    CHECK(ft_share_estimate(fr, 2, &o, 1500, 5, &ft_lens_default, &est,
                            &e) == -1);                 /* 12 % 5 */
    sel[0] = fr[1];
    sel[1] = fr[0];                                     /* out of order */
    CHECK(ft_share_estimate(sel, 2, &o, 1500, 1, &ft_lens_default, &est,
                            &e) == -1);
    CHECK(strstr(e.msg, "frame 1") != NULL);
    fr[1].y0 = 220;                                     /* past the holder */
    CHECK(ft_share_estimate(fr, 2, &o, 1500, 1, &ft_lens_default, &est,
                            &e) == -1);
    CHECK(strstr(e.msg, "outside") != NULL);
    CHECK(ft_share_candidates(fr, 0, &o, &ft_lens_default, c, &e) == -1);
    return TEST_DONE();
}

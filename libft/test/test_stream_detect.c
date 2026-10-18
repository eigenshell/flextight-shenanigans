/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The streaming frame detector (ft_stream_detect.h) on synthetic strips:
 * rows fed one at a time and in random chunks give the same frames. */
#include <stdlib.h>
#include <string.h>
#include "ft_holder.h"
#include "ft_stream_detect.h"
#include "test/ftest.h"
#include "test/synth.h"

static ft_sdet_frame F[FT_MAX_FRAMES];
static int NF, GRID, OPEN_AFTER;    /* frames, grid flag, sealed only at end */
static float BASE[3];
static int HAS_BASE;

static void axes(const synth_spec *s, ft_axes *ax)
{
    ax->y_mm0 = 0;
    ax->y_mm_per_line = s->y_mm_per_line;
    ax->x_mm0 = s->x_mm0;
    ax->x_mm_per_px = s->x_mm_per_px;
}

/* feed s's preview: chunk 0 one line a call (ft_sdet_line), else chunks
 * of 1..chunk lines (ft_sdet_feed), the sizes from seed */
static void run_chunked(const synth_spec *s, int chunk, unsigned seed,
                        int end)
{
    static ft_sdet d;
    ft_axes ax;
    ft_err e;
    float *t = synth_preview(s);
    long l = 0, row = (long)s->px * 3;
    int got = 0, before;
    CHECK(t != NULL);
    axes(s, &ax);
    CHECK(ft_sdet_init(&d, &ax, s->px, s->film, ft_holder_find("24x217"),
                       &e) == 0);
    while (l < s->lines) {
        int n = 1, k;
        if (chunk) {
            seed = seed * 1103515245u + 12345u;
            n = 1 + (int)((seed >> 16) % (unsigned)chunk);
            if (l + n > s->lines)
                n = (int)(s->lines - l);
            k = ft_sdet_feed(&d, t + l * row, n);
        } else {
            k = ft_sdet_line(&d, t + l * row);
        }
        CHECK(k >= 0);
        got += k;
        CHECK(got == d.nframes);          /* the count is what was sealed */
        l += n;
    }
    before = d.nframes;
    if (end)
        got += ft_sdet_end(&d);
    CHECK(got == d.nframes);
    NF = d.nframes;
    OPEN_AFTER = d.nframes - before;
    memcpy(F, d.f, sizeof F);
    GRID = d.grid;
    HAS_BASE = ft_sdet_base(&d, BASE);
    ft_sdet_free(&d);
    free(t);
}

/* one line at a time, then random chunks of up to 300 and of up to 7
 * lines: every frame the same, bit for bit */
static void run(const synth_spec *s)
{
    static ft_sdet_frame a[FT_MAX_FRAMES];
    int na, ga, i;
    unsigned seeds[2] = { 7, 12345 };
    int chunks[2] = { 300, 7 };
    run_chunked(s, 0, 0, 1);
    memcpy(a, F, sizeof a);
    na = NF;
    ga = GRID;
    for (i = 0; i < 2; i++) {
        run_chunked(s, chunks[i], seeds[i], 1);
        CHECK(NF == na && GRID == ga);
        CHECK(memcmp(a, F, sizeof a) == 0);
    }
}

static void expect_types(const char *want)     /* F H p per frame */
{
    int i;
    CHECK(NF == (int)strlen(want));
    for (i = 0; want[i]; i++) {
        ft_frame_type t = want[i] == 'F' ? FT_FRAME_FULL :
                          want[i] == 'H' ? FT_FRAME_HALF : FT_FRAME_PARTIAL;
        CHECK(F[i].type == t);
        CHECK(F[i].index == i + 1);
    }
}

/* the box: the frame padded by FT_SDET_PAD_MM along and across */
static void expect_box(int i, double y0, double len, const synth_spec *s,
                       double tol)
{
    CHECK_NEAR(F[i].y0, y0 - FT_SDET_PAD_MM, tol);
    CHECK_NEAR(F[i].y1, y0 + len + FT_SDET_PAD_MM, tol);
    CHECK_NEAR(F[i].x0, s->img_x0 - FT_SDET_PAD_MM, tol);
    CHECK_NEAR(F[i].x1, s->img_x1 + FT_SDET_PAD_MM, tol);
}

int main(void)
{
    synth_spec s;
    const ft_holder *h = ft_holder_find("24x217");
    int i;

    /* the holder knows where the image may lie across */
    CHECK(h && h->band_x0_mm > 0 && h->band_x1_mm > h->band_x0_mm + 15);
    CHECK(h->img_x0_mm < h->band_x0_mm && h->img_x1_mm > h->band_x1_mm);

    /* six full frames, colour negative */
    synth_default(&s, FT_FILM_NEGATIVE);
    for (i = 0; i < 6; i++)
        synth_add(&s, 4 + 38.0 * i, 36, 0);
    run(&s);
    expect_types("FFFFFF");
    for (i = 0; i < 6; i++) {
        expect_box(i, 4 + 38.0 * i, 36, &s, 0.15);
        CHECK(!F[i].grid);
    }
    CHECK(!GRID && OPEN_AFTER == 0);
    CHECK(HAS_BASE);
    CHECK_NEAR(BASE[0], 0.62, 0.01);
    CHECK_NEAR(BASE[1], 0.40, 0.01);
    CHECK_NEAR(BASE[2], 0.25, 0.01);

    /* the same with grain, noise, blur and a 0.3 degree tilt; the image
     * where the real 24x217 strips put it (hardware 2026-10-18: 2.0 and
     * 4.6 mm across on two loads) */
    {
        double at[2] = { 2.1, 4.6 };
        int k;
        for (k = 0; k < 2; k++) {
            synth_default(&s, FT_FILM_NEGATIVE);
            s.img_x0 = at[k];
            s.img_x1 = at[k] + 24;
            s.film_x0 = at[k] - 5.5;
            s.film_x1 = at[k] + 29.5;
            s.grain = 0.02;
            s.noise = 0.002;
            s.blur_mm = 0.1;
            s.tilt_deg = 0.3;
            for (i = 0; i < 6; i++)
                synth_add(&s, 4 + 38.0 * i, 36, 0);
            run(&s);
            expect_types("FFFFFF");
            for (i = 0; i < 6; i++) {
                /* tilt: 24 mm x tan 0.3 deg = 0.13 mm along */
                CHECK(F[i].y0 <= 4 + 38.0 * i - FT_SDET_PAD_MM + 0.1);
                CHECK(F[i].y1 >= 40 + 38.0 * i + FT_SDET_PAD_MM - 0.1);
                CHECK(F[i].y1 - F[i].y0 < 36 + 2 * FT_SDET_PAD_MM + 0.6);
                CHECK_NEAR(F[i].x0, s.img_x0 - FT_SDET_PAD_MM, 0.3);
                CHECK_NEAR(F[i].x1, s.img_x1 + FT_SDET_PAD_MM, 0.3);
            }
        }
    }

    /* twelve half frames, 1 mm gaps */
    synth_default(&s, FT_FILM_NEGATIVE);
    for (i = 0; i < 12; i++)
        synth_add(&s, 4 + 19.0 * i, 18, 0);
    run(&s);
    expect_types("HHHHHHHHHHHH");
    for (i = 0; i < 11; i++)
        expect_box(i, 4 + 19.0 * i, 18, &s, 0.15);
    CHECK(F[11].y1 == h->scan_len_mm);    /* the box stays in the holder */

    /* full and half mixed on one strip */
    synth_default(&s, FT_FILM_NEGATIVE);
    synth_add(&s, 4, 36, 0);
    synth_add(&s, 42, 18, 0);
    synth_add(&s, 62, 18, 0);
    synth_add(&s, 82, 36, 0);
    synth_add(&s, 120, 36, 0);
    synth_add(&s, 158, 18, 0);
    synth_add(&s, 178, 36, 0);
    run(&s);
    expect_types("FHHFFHF");
    expect_box(2, 62, 18, &s, 0.15);
    expect_box(6, 178, 36, &s, 0.15);

    /* no gap anywhere: frames touch; the holder grid cuts every 36 mm
     * from the last trusted edge (the base before frame 1) */
    synth_default(&s, FT_FILM_NEGATIVE);
    for (i = 0; i < 6; i++)
        synth_add(&s, 4 + 36.0 * i, 36, 0);
    run(&s);
    expect_types("FFFFFF");
    CHECK(GRID);
    for (i = 0; i < 6; i++) {
        CHECK_NEAR(F[i].y0, 4 + 36.0 * i - FT_SDET_PAD_MM, 0.15);
        CHECK_NEAR(F[i].y1, 40 + 36.0 * i + FT_SDET_PAD_MM, 0.15);
        CHECK(F[i].grid == (i < 5));      /* the last ends at a gap */
    }

    /* a ~36 mm frame with a weak base-like dip at 18 mm (0.2 mm, under
     * the gap's length) stays one full frame: the user may split it */
    synth_default(&s, FT_FILM_NEGATIVE);
    for (i = 0; i < 6; i++)
        synth_add(&s, 4 + 38.0 * i, 36, 0);
    s.npatch = 1;
    s.patch[0].y0 = 42 + 18;
    s.patch[0].y1 = 42 + 18.2;
    s.patch[0].x0 = 0;
    s.patch[0].x1 = 40;
    run(&s);
    expect_types("FFFFFF");
    expect_box(1, 42, 36, &s, 0.15);

    /* slide: dark gaps, a dark (Dmax) leader */
    synth_default(&s, FT_FILM_SLIDE);
    for (i = 0; i < 6; i++)
        synth_add(&s, 4 + 38.0 * i, 36, 0);
    run(&s);
    expect_types("FFFFFF");
    for (i = 0; i < 6; i++)
        expect_box(i, 4 + 38.0 * i, 36, &s, 0.15);
    CHECK(HAS_BASE);
    CHECK_NEAR(BASE[0], 0.003, 0.001);    /* the slide's black */

    /* partial frames at both ends: one cut by the film's start, one by
     * its end (holder metal after), full frames between */
    synth_default(&s, FT_FILM_NEGATIVE);
    s.film_y0 = 10;
    s.film_y1 = 200;
    synth_add(&s, 0, 30, 0);              /* 10..30 on the film: 20 mm */
    synth_add(&s, 32, 36, 0);
    synth_add(&s, 70, 36, 0);
    synth_add(&s, 108, 36, 0);
    synth_add(&s, 146, 36, 0);
    synth_add(&s, 184, 36, 0);            /* 184..200: 16 mm */
    run(&s);
    expect_types("pFFFFp");
    CHECK_NEAR(F[0].y0, 10 - FT_SDET_PAD_MM, 0.15);
    CHECK_NEAR(F[0].y1, 30 + FT_SDET_PAD_MM, 0.15);
    CHECK_NEAR(F[5].y0, 184 - FT_SDET_PAD_MM, 0.15);
    CHECK_NEAR(F[5].y1, 200 + FT_SDET_PAD_MM, 0.15);

    /* open holder at both ends (the 24x217 holder is longer than a
     * strip: hardware 2026-10-18); the last frame cut 0.5 mm short by
     * the film's end: still full (within 1.5 mm of 36 at an end) */
    synth_default(&s, FT_FILM_NEGATIVE);
    s.open_outside = 1;
    s.film_y0 = 20;
    s.film_y1 = 208.5;
    for (i = 0; i < 5; i++)
        synth_add(&s, 21 + 38.0 * i, 36, 0);
    run(&s);
    expect_types("FFFFF");
    for (i = 0; i < 4; i++)
        expect_box(i, 21 + 38.0 * i, 36, &s, 0.15);
    CHECK_NEAR(F[4].y1, 208.5 + FT_SDET_PAD_MM, 0.15);

    /* the strip's end seals the open run: not sealed before ft_sdet_end
     * (a stopped preview drops it) */
    synth_default(&s, FT_FILM_NEGATIVE);
    s.film_y1 = 240;
    for (i = 0; i < 5; i++)
        synth_add(&s, 4 + 38.0 * i, 36, 0);
    synth_add(&s, 194, 40, 0);            /* past the strip's end (232) */
    run(&s);
    expect_types("FFFFFp");
    CHECK(OPEN_AFTER == 1);
    run_chunked(&s, 0, 0, 0);
    CHECK(NF == 5);

    /* no sprocket holes in view (as 120 film): the same frames */
    synth_default(&s, FT_FILM_NEGATIVE);
    s.film_x0 = -6;
    s.film_x1 = 36;
    for (i = 0; i < 6; i++)
        synth_add(&s, 4 + 38.0 * i, 36, 0);
    run(&s);
    expect_types("FFFFFF");
    for (i = 0; i < 6; i++)
        expect_box(i, 4 + 38.0 * i, 36, &s, 0.15);

    /* a blank (unexposed) frame between frames is gap, not a frame */
    synth_default(&s, FT_FILM_NEGATIVE);
    for (i = 0; i < 6; i++)
        synth_add(&s, 4 + 38.0 * i, 36, i == 2);
    run(&s);
    expect_types("FFFFF");
    expect_box(2, 4 + 38.0 * 3, 36, &s, 0.15);
    CHECK(F[2].index == 3);

    /* refusals */
    {
        static ft_sdet d;
        ft_axes ax;
        ft_err e;
        axes(&s, &ax);
        CHECK(ft_sdet_init(&d, &ax, 0, FT_FILM_NEGATIVE, h, &e) == -1);
        CHECK(ft_sdet_init(&d, &ax, 666, FT_FILM_NEGATIVE, NULL, &e) == -1);
        ax.x_mm0 = 100;                   /* the band off the line */
        CHECK(ft_sdet_init(&d, &ax, 666, FT_FILM_NEGATIVE, h, &e) == -1);
    }
    return TEST_DONE();
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdlib.h>
#include <string.h>

#include "ft_detect.h"
#include "ft_holder.h"
#include "ft_job.h"
#include "test/ftest.h"
#include "test/synth.h"

static ft_layout L;

static int run(const synth_spec* s) {
  ft_axes ax;
  ft_err e;
  float* t = synth_preview(s);
  int rc;
  ax.y_mm0 = 0;
  ax.y_mm_per_line = s->y_mm_per_line;
  ax.x_mm0 = s->x_mm0;
  ax.x_mm_per_px = s->x_mm_per_px;
  CHECK(t != NULL);
  rc = ft_detect(t, s->lines, s->px, &ax, s->film, ft_holder_find("24x217"), &L,
                 &e);
  free(t);
  return rc;
}

static void ff6(synth_spec* s, ft_film film) {
  int i;
  synth_default(s, film);
  for(i = 0; i < 6; i++) synth_add(s, 4 + 38.0 * i, 36, 0);
}

static void expect_types(const char* want) /* F H P p per frame */
{
  int i;
  CHECK(L.nframes == (int)strlen(want));
  for(i = 0; want[i]; i++) {
    ft_frame_type t = want[i] == 'F'   ? FT_FRAME_FULL
                      : want[i] == 'H' ? FT_FRAME_HALF
                      : want[i] == 'P' ? FT_FRAME_PANORAMA
                                       : FT_FRAME_PARTIAL;
    CHECK(L.f[i].type == t);
    CHECK(L.f[i].index == i + 1);
  }
}

static int nlow(void) {
  int i, n = 0;
  for(i = 0; i < L.nframes; i++) n += L.f[i].confidence_low;
  return n;
}

static void hf12(synth_spec* s, double pitch) {
  int i;
  synth_default(s, FT_FILM_NEGATIVE);
  for(i = 0; i < 12; i++) synth_add(s, 4 + pitch * i, 18, 0);
}

int main(void) {
  synth_spec s;
  int i;

  /* six full frames, colour negative */
  ff6(&s, FT_FILM_NEGATIVE);
  CHECK(run(&s) == 0);
  expect_types("FFFFFF");
  for(i = 0; i < 6; i++) {
    CHECK_NEAR(L.f[i].y0, 4 + 38.0 * i, 0.3);
    CHECK_NEAR(L.f[i].y1, 40 + 38.0 * i, 0.3);
    CHECK_NEAR(L.f[i].x0, 4.1, 0.3);
    CHECK_NEAR(L.f[i].x1, 28.1, 0.3);
    CHECK(!L.f[i].confidence_low);
  }
  CHECK(L.has_holes);
  CHECK_NEAR(L.holes_x0, 0.6, 0.3);
  CHECK_NEAR(L.holes_x1, 31.6, 0.3);
  CHECK_NEAR(L.film_y0, 3.0, 0.1);
  CHECK_NEAR(L.base[0], 0.62, 0.01);
  CHECK(L.nblank == 0 && L.nblank_end == 0 && !L.fallback);

  /* job: sprockets widen the window across to the hole rows */
  {
    ft_job j;
    ft_layout_to_job(&L, ft_holder_find("24x217"), FT_FILM_NEGATIVE, 2000, 1,
                     &j);
    CHECK(j.nframes == 6 && j.f[0].sprockets && j.f[0].include);
    CHECK_NEAR(j.f[0].x0, L.holes_x0, 1e-9);
    CHECK_NEAR(j.f[0].width, L.holes_x1 - L.holes_x0, 1e-9);
    CHECK_NEAR(j.f[0].len, L.f[0].y1 - L.f[0].y0, 1e-9);
    ft_layout_to_job(&L, ft_holder_find("24x217"), FT_FILM_NEGATIVE, 2000, 0,
                     &j);
    CHECK_NEAR(j.f[0].x0, L.f[0].x0, 1e-9);
    CHECK(j.focus == FT_FOCUS_SHIFTED && j.f[0].ppi == 2000);
    /* the film base goes to the job as density */
    CHECK(j.has_film_base);
    CHECK_NEAR(j.film_base[0], -log10(L.base[0]), 1e-9);
    CHECK_NEAR(j.film_base[2], -log10(L.base[2]), 1e-9);
    /* a base above 1 or at the floor stays within what the reader takes */
    {
      ft_layout M = L;
      ft_job k;
      ft_err e2;
      FILE* g = tmpfile();
      M.base[0] = 1.02;
      M.base[1] = 0.3;
      M.base[2] = 0.1;
      ft_layout_to_job(&M, ft_holder_find("24x217"), FT_FILM_NEGATIVE, 2000, 0,
                       &k);
      CHECK(k.film_base[0] == 0);
      CHECK(ft_job_write(&k, g) == 0);
      rewind(g);
      CHECK(ft_job_read(&k, g, &e2) == 0 && k.has_film_base);
      fclose(g);
      M.base[0] = FT_T_FLOOR / 2;
      M.base[1] = FT_T_FLOOR / 10;
      M.base[2] = 0;
      ft_layout_to_job(&M, ft_holder_find("24x217"), FT_FILM_NEGATIVE, 2000, 0,
                       &k);
      CHECK(k.film_base[0] == 4.5 &&
            k.film_base[1] == 4.5 &&
            k.film_base[2] == 4.5);
    }
  }

  /* real preview axes (x 0.92..33.14 mm) with the film shifted so the
   * far hole row ends at 32.56 mm: the sprockets-in window is clamped
   * to the holder's 32 mm crop width and the job validates */
  {
    const ft_holder* h = ft_holder_find("24x217");
    ft_job j;
    ft_err e;
    ff6(&s, FT_FILM_NEGATIVE);
    s.x_mm0 = 0.92;
    s.film_x0 = -0.44;
    s.film_x1 = 34.56;
    s.img_x0 = 5.06;
    s.img_x1 = 29.06;
    CHECK(run(&s) == 0);
    expect_types("FFFFFF");
    CHECK(L.has_holes);
    CHECK_NEAR(L.holes_x1, 32.56, 0.3);
    CHECK(L.holes_x1 > h->scan_width_mm);
    ft_layout_to_job(&L, h, FT_FILM_NEGATIVE, 2000, 1, &j);
    for(i = 0; i < j.nframes; i++) {
      CHECK_NEAR(j.f[i].x0, L.holes_x0, 1e-9);
      CHECK_NEAR(j.f[i].x0 + j.f[i].width, h->scan_width_mm, 1e-9);
    }
    CHECK(ft_job_validate(&j, &ft_lens_default, &e) == 0);

    /* last frame cut by the strip end past 231 mm: clamped along */
    synth_default(&s, FT_FILM_NEGATIVE);
    s.x_mm0 = 0.92;
    for(i = 0; i < 6; i++)
      synth_add(&s, 6 + 38.0 * i, 36, 0); /* last: 196..232 */
    CHECK(run(&s) == 0);
    CHECK(L.nframes == 6);
    CHECK(L.f[5].y1 > h->scan_len_mm);
    ft_layout_to_job(&L, h, FT_FILM_NEGATIVE, 2000, 0, &j);
    CHECK_NEAR(j.f[5].y0, L.f[5].y0, 1e-9);
    CHECK_NEAR(j.f[5].y0 + j.f[5].len, h->scan_len_mm, 1e-9);
    CHECK_NEAR(j.f[0].len, L.f[0].y1 - L.f[0].y0, 1e-9);
    CHECK(ft_job_validate(&j, &ft_lens_default, &e) == 0);
  }

  /* twelve half frames */
  synth_default(&s, FT_FILM_NEGATIVE);
  for(i = 0; i < 12; i++) synth_add(&s, 4 + 19.0 * i, 18, 0);
  CHECK(run(&s) == 0);
  expect_types("HHHHHHHHHHHH");
  for(i = 0; i < 12; i++) CHECK_NEAR(L.f[i].y0, 4 + 19.0 * i, 0.3);

  /* Autoreflex: full/half switching mid-roll, wider switch gaps */
  synth_default(&s, FT_FILM_NEGATIVE);
  synth_add(&s, 4, 36, 0);
  synth_add(&s, 42, 36, 0);
  synth_add(&s, 81, 18, 0);
  synth_add(&s, 100, 18, 0);
  synth_add(&s, 119, 18, 0);
  synth_add(&s, 140, 36, 0);
  CHECK(run(&s) == 0);
  expect_types("FFHHHF");
  CHECK_NEAR(L.f[2].y0, 81, 0.3);
  CHECK_NEAR(L.f[5].y0, 140, 0.3);

  /* panorama: 100 mm is longer than any real 35 mm panorama (55-70 mm),
   * so it is kept whole but flagged */
  synth_default(&s, FT_FILM_NEGATIVE);
  synth_add(&s, 4, 100, 0);
  synth_add(&s, 106, 36, 0);
  CHECK(run(&s) == 0);
  expect_types("PF");
  CHECK(L.f[0].confidence_low && !L.f[1].confidence_low);
  CHECK(L.nblank == 0 && L.nblank_end == 2); /* 89 mm unexposed tail */

  /* XPan-size panorama (65 mm): trusted */
  synth_default(&s, FT_FILM_NEGATIVE);
  synth_add(&s, 4, 65, 0);
  synth_add(&s, 71, 36, 0);
  CHECK(run(&s) == 0);
  expect_types("PF");
  CHECK(nlow() == 0);

  /* two frames touching mid-strip: one run of 74 mm, a flagged panorama */
  ff6(&s, FT_FILM_NEGATIVE);
  s.fr[1].len = 38;
  CHECK(run(&s) == 0);
  expect_types("FPFFF");
  CHECK(L.f[1].confidence_low && nlow() == 1);

  /* light leak across 20 % of one gap's columns: the gap is lost, the
   * merged run is a flagged panorama */
  ff6(&s, FT_FILM_NEGATIVE);
  s.nleak = 1;
  s.leak[0].y0 = 39.5;
  s.leak[0].y1 = 42.5;
  s.leak[0].x0 = 4.1;
  s.leak[0].x1 = 8.9;
  CHECK(run(&s) == 0);
  expect_types("PFFFF");
  CHECK(L.f[0].confidence_low && nlow() == 1);

  /* partial: the film starts inside frame 1 */
  ff6(&s, FT_FILM_NEGATIVE);
  s.film_y0 = 20;
  CHECK(run(&s) == 0);
  expect_types("pFFFFF");

  /* blank frame 3 is dropped and counted */
  ff6(&s, FT_FILM_NEGATIVE);
  s.fr[2].blank = 1;
  CHECK(run(&s) == 0);
  expect_types("FFFFF");
  CHECK(L.nblank == 1 && L.nblank_end == 0);
  CHECK_NEAR(L.f[2].y0, 118, 0.3);

  /* tilted strip */
  ff6(&s, FT_FILM_NEGATIVE);
  s.tilt_deg = 0.3;
  CHECK(run(&s) == 0);
  expect_types("FFFFFF");
  for(i = 0; i < 6; i++) {
    CHECK_NEAR(L.f[i].tilt_deg, 0.3, 0.05);
    CHECK_NEAR(L.f[i].y0, 4 + 38.0 * i, 0.3);
  }

  /* slide film: gaps are the darkest film */
  ff6(&s, FT_FILM_SLIDE);
  CHECK(run(&s) == 0);
  expect_types("FFFFFF");
  CHECK_NEAR(L.f[3].y0, 4 + 38.0 * 3, 0.3);

  /* a round shadow spanning the image width is not a frame edge */
  synth_default(&s, FT_FILM_NEGATIVE);
  synth_add(&s, 4, 100, 0);
  synth_add(&s, 106, 36, 0);
  s.blob = 1;
  s.blob_y = 54;
  s.blob_x = 16.1;
  s.blob_r = 14;
  CHECK(run(&s) == 0);
  expect_types("PF");

  /* dark scene content against one edge of a real gap */
  ff6(&s, FT_FILM_NEGATIVE);
  s.npatch = 1;
  s.patch[0].y0 = 42;
  s.patch[0].y1 = 45;
  s.patch[0].x0 = 4.1;
  s.patch[0].x1 = 10.1;
  CHECK(run(&s) == 0);
  expect_types("FFFFFF");
  CHECK_NEAR(L.f[1].y0, 42, 0.3);

  /* frames touching each other: no edge anywhere -> grid, flagged */
  synth_default(&s, FT_FILM_NEGATIVE);
  s.film_y1 = 231.0;
  for(i = 0; i < 6; i++) synth_add(&s, 3 + 38.0 * i, 38, 0);
  CHECK(run(&s) == 0);
  CHECK(L.fallback && L.nframes == 6);
  CHECK(L.f[0].confidence_low && L.f[5].confidence_low);

  /* --- realistic degradations --- */

  /* soft edges: 0.1 mm blur */
  ff6(&s, FT_FILM_NEGATIVE);
  s.blur_mm = 0.1;
  CHECK(run(&s) == 0);
  expect_types("FFFFFF");
  CHECK(!L.fallback && nlow() == 0);
  CHECK_NEAR(L.base[0], 0.62, 0.02 * 0.62);
  for(i = 0; i < 6; i++) {
    CHECK_NEAR(L.f[i].y0, 4 + 38.0 * i, 0.3);
    CHECK_NEAR(L.f[i].y1, 40 + 38.0 * i, 0.3);
  }

  /* base gain drifting +-10 % along the roll */
  ff6(&s, FT_FILM_NEGATIVE);
  s.drift = 0.10;
  CHECK(run(&s) == 0);
  expect_types("FFFFFF");
  CHECK(nlow() == 0);

  /* half frames with 0.5 mm gaps, 3 % grain, 0.1 mm blur */
  hf12(&s, 18.5);
  s.grain = 0.03;
  s.blur_mm = 0.1;
  CHECK(run(&s) == 0);
  expect_types("HHHHHHHHHHHH");
  for(i = 0; i < 12; i++) CHECK_NEAR(L.f[i].y0, 4 + 18.5 * i, 0.3);

  /* same with grain only (no blur to average it away) */
  hf12(&s, 18.5);
  s.grain = 0.03;
  CHECK(run(&s) == 0);
  expect_types("HHHHHHHHHHHH");

  /* half frames with 0.5 mm gaps on a strip tilted 0.5 degrees */
  hf12(&s, 18.5);
  s.tilt_deg = 0.5;
  CHECK(run(&s) == 0);
  expect_types("HHHHHHHHHHHH");
  for(i = 0; i < 12; i++) {
    CHECK_NEAR(L.f[i].y0, 4 + 18.5 * i, 0.3);
    CHECK_NEAR(L.f[i].tilt_deg, 0.5, 0.05);
  }

  /* slide at a realistic Dmax with sensor noise; frame 1 black: blank at
   * the strip start, counted with the ends (could be leader) */
  ff6(&s, FT_FILM_SLIDE);
  s.base[0] = s.base[1] = s.base[2] = 0.0005f;
  s.noise = 2e-4;
  s.npatch = 1;
  s.patch[0].y0 = 4;
  s.patch[0].y1 = 40;
  s.patch[0].x0 = 4.1;
  s.patch[0].x1 = 28.1;
  CHECK(run(&s) == 0);
  expect_types("FFFFF");
  CHECK(!L.fallback && L.nblank == 0 && L.nblank_end == 1);
  CHECK_NEAR(L.f[0].y0, 42, 0.3);

  /* thin base-like content against both edges of one gap */
  ff6(&s, FT_FILM_NEGATIVE);
  s.npatch = 2;
  s.patch[0].y0 = 37.5;
  s.patch[0].y1 = 40;
  s.patch[0].x0 = 10;
  s.patch[0].x1 = 10.4;
  s.patch[1].y0 = 42;
  s.patch[1].y1 = 44.5;
  s.patch[1].x0 = 20;
  s.patch[1].x1 = 20.4;
  CHECK(run(&s) == 0);
  expect_types("FFFFFF");
  CHECK_NEAR(L.f[0].y1, 40, 0.3);
  CHECK_NEAR(L.f[1].y0, 42, 0.3);

  /* open-beam specks (dust holes) scattered over the strip */
  ff6(&s, FT_FILM_NEGATIVE);
  s.nspeck = 40;
  s.speck_r = 0.15;
  CHECK(run(&s) == 0);
  expect_types("FFFFFF");
  CHECK(nlow() == 0);

  /* short strip, holder open beyond the film */
  synth_default(&s, FT_FILM_NEGATIVE);
  s.film_y1 = 117;
  s.open_outside = 1;
  for(i = 0; i < 3; i++) synth_add(&s, 4 + 38.0 * i, 36, 0);
  CHECK(run(&s) == 0);
  expect_types("FFF");
  CHECK(L.has_holes && !L.fallback);
  CHECK_NEAR(L.film_y1, 117, 0.1);
  CHECK_NEAR(L.holes_x0, 0.6, 0.3);

  /* touching frames after a leader: the stretch is gridded and flagged,
   * never one long unflagged run */
  synth_default(&s, FT_FILM_NEGATIVE);
  for(i = 0; i < 6; i++) synth_add(&s, 6 + 37.5 * i, 37.5, 0);
  CHECK(run(&s) == 0);
  CHECK(L.nframes == 6 && nlow() == 6);
  CHECK_NEAR(L.f[0].y0, 6, 0.3);
  for(i = 0; i < L.nframes; i++) CHECK(L.f[i].y1 - L.f[i].y0 <= 39.5);

  /* hole rows outside the scanned width: global base, tilt from edges */
  ff6(&s, FT_FILM_SLIDE);
  s.film_x0 = -6;
  s.film_x1 = 40;
  s.tilt_deg = -0.3;
  CHECK(run(&s) == 0);
  expect_types("FFFFFF");
  CHECK(!L.has_holes && nlow() == 0);
  for(i = 0; i < 6; i++) {
    CHECK_NEAR(L.f[i].y0, 4 + 38.0 * i, 0.3);
    CHECK_NEAR(L.f[i].tilt_deg, -0.3, 0.05);
  }

  /* hardware 2026-10-12, negative strip: holder metal at a noisy ~0.001
   * (only ~50 % of its pixels below 0.002), hole rows outside the scanned
   * width, and the strip start cut on a diagonal with open beam up to
   * the image of frame 1 (no base between them for a gap). Frame 1 is full
   * and starts at its image, frame 6 is cut where the film ends. */
  synth_default(&s, FT_FILM_NEGATIVE);
  s.metal = 0.0012;
  s.metal_noise = 0.0008;
  s.film_x0 = -6;
  s.film_x1 = 40;
  s.film_y0 = 2.2;
  s.film_y1 = 227.2;
  s.blur_mm = 0.15; /* the metal edge is ~0.3 mm soft */
  s.cut_mm = 2.0;   /* open to 3.68 mm at 28.1 mm across */
  for(i = 0; i < 6; i++) synth_add(&s, 3.55 + 38.0 * i, 36, 0);
  CHECK(run(&s) == 0);
  CHECK(!L.has_holes);
  expect_types("FFFFFp");
  CHECK(!L.fallback && nlow() == 0);
  CHECK(L.film_y0 >= 1.9 && L.film_y0 < 3.0); /* at the soft metal edge */
  CHECK_NEAR(L.film_y1, 227.2, 0.3);
  CHECK_NEAR(L.f[0].y0, 3.55, 0.3);
  CHECK_NEAR(L.f[5].y1, 227.2, 0.3);

  /* hardware 2026-10-12: thin (base-like) content against half of an
   * edge on both sides of two gaps running. The outer half of each lead
   * edge is a clean line; content only pulls the found edge into the
   * frame, never into the gap. No grid, no flags. */
  ff6(&s, FT_FILM_NEGATIVE);
  s.npatch = 4;
  s.patch[0].y0 = 76.8;
  s.patch[0].y1 = 78; /* frame 2 end */
  s.patch[1].y0 = 80;
  s.patch[1].y1 = 83.5; /* frame 3 start */
  s.patch[2].y0 = 114.8;
  s.patch[2].y1 = 116; /* frame 3 end */
  s.patch[3].y0 = 118;
  s.patch[3].y1 = 121.5; /* frame 4 start */
  for(i = 0; i < 4; i++) {
    s.patch[i].x0 = i % 2 ? 4.1 : 16;
    s.patch[i].x1 = i % 2 ? 16 : 28.1;
  }
  CHECK(run(&s) == 0);
  expect_types("FFFFFF");
  CHECK(!L.fallback && nlow() == 0);
  for(i = 0; i < 6; i++) {
    CHECK_NEAR(L.f[i].y0, 4 + 38.0 * i, 0.3);
    CHECK_NEAR(L.f[i].y1, 40 + 38.0 * i, 0.3);
  }

  /* hardware 2026-10-12, slides: two loose pieces with open holder
   * before and between them. Open holder is no film: never a frame,
   * never a blank frame, and it separates the pieces. */
  synth_default(&s, FT_FILM_SLIDE);
  s.film_y0 = 2.2;
  s.film_y1 = 227.3;
  s.nopen = 2;
  s.open[0].y0 = 2.2;
  s.open[0].y1 = 27.6;
  s.open[1].y0 = 102.3;
  s.open[1].y1 = 107.5;
  synth_add(&s, 28.5, 36, 0);
  synth_add(&s, 66.5, 35.5, 0);
  for(i = 0; i < 4; i++)
    synth_add(&s, 108.5 + 38.0 * i, 36, 0); /* last cut at 227.3 */
  CHECK(run(&s) == 0);
  expect_types("FFFFFp");
  CHECK(!L.fallback && nlow() == 0);
  CHECK(L.nblank == 0 && L.nblank_end == 0);
  CHECK_NEAR(L.f[0].y0, 28.5, 0.3);
  CHECK_NEAR(L.f[1].y1, 102, 0.3);
  CHECK_NEAR(L.f[2].y0, 108.5, 0.3);

  /* hardware 2026-10-13, slides (pv3): the holder window hides most of
   * each hole row, leaving 0.7-1.6 mm of them in view. The narrow row
   * still has to count, or the base falls back to the darkest band
   * pixels (slide Dmax ~0, no noise floor) and every gap is lost. */
  synth_default(&s, FT_FILM_SLIDE);
  s.base[0] = s.base[1] = s.base[2] = 0.0005f;
  s.noise = 2e-4;
  s.metal = 0.0012;
  s.metal_noise = 0.0008;
  s.win_x0 = 2.6;  /* left row: 0.8 mm in view */
  s.win_x1 = 30.4; /* right row: 1.6 mm */
  s.film_y0 = 2.2;
  s.film_y1 = 227.3;
  s.nopen = 2;
  s.open[0].y0 = 2.2;
  s.open[0].y1 = 27.6;
  s.open[1].y0 = 102.3;
  s.open[1].y1 = 107.5;
  synth_add(&s, 28.5, 36, 0);
  synth_add(&s, 66.5, 35.5, 0);
  for(i = 0; i < 4; i++) synth_add(&s, 108.5 + 38.0 * i, 36, 0);
  CHECK(run(&s) == 0);
  CHECK(L.has_holes);
  CHECK_NEAR(L.holes_x0, 2.6, 0.3);
  CHECK_NEAR(L.holes_x1, 30.4, 0.3);
  expect_types("FFFFFp");
  CHECK(!L.fallback && nlow() == 0);
  CHECK_NEAR(L.f[0].y0, 28.5, 0.3);
  CHECK_NEAR(L.f[1].y1, 102, 0.3);
  CHECK_NEAR(L.f[2].y0, 108.5, 0.3);
  CHECK_NEAR(L.f[3].y0, 146.5, 0.3);

  /* hardware 2026-10-12, slides: dark (Dmax-like) content against half
   * of both edges of two gaps running, deeper than the edge window, so
   * no edge is trusted. Gap lines across the whole band still mark
   * frame ends when they cut off a frame-sized piece: no grid. */
  ff6(&s, FT_FILM_SLIDE);
  s.npatch = 4;
  s.patch[0].y0 = 74.5;
  s.patch[0].y1 = 78; /* frame 2 end */
  s.patch[1].y0 = 80;
  s.patch[1].y1 = 83.5; /* frame 3 start */
  s.patch[2].y0 = 112.5;
  s.patch[2].y1 = 116; /* frame 3 end */
  s.patch[3].y0 = 118;
  s.patch[3].y1 = 121.5; /* frame 4 start */
  for(i = 0; i < 4; i++) {
    s.patch[i].x0 = i % 2 ? 4.1 : 16;
    s.patch[i].x1 = i % 2 ? 16 : 28.1;
  }
  CHECK(run(&s) == 0);
  expect_types("FFFFFF");
  CHECK(!L.fallback && nlow() == 0);
  for(i = 0; i < 6; i++) {
    CHECK_NEAR(L.f[i].y0, 4 + 38.0 * i, 0.3);
    CHECK_NEAR(L.f[i].y1, 40 + 38.0 * i, 0.3);
  }

  /* bad arguments */
  {
    ft_axes ax = {0, 25.4 / 525, 0, 0};
    ft_err e;
    float px1[3 * 64] = {0};
    CHECK(ft_detect(px1, 8, 8, &ax, FT_FILM_NEGATIVE, ft_holder_find("24x217"),
                    &L, &e) == -1);
    ax.x_mm_per_px = 0.048;
    CHECK(ft_detect(NULL, 8, 8, &ax, FT_FILM_NEGATIVE, ft_holder_find("24x217"),
                    &L, &e) == -1);
    CHECK(ft_detect(px1, 0, 8, &ax, FT_FILM_NEGATIVE, ft_holder_find("24x217"),
                    &L, &e) == -1);
    CHECK(ft_detect(px1, 8, 8, &ax, FT_FILM_NEGATIVE, NULL, &L, &e) == -1);
    ax.y_mm_per_line = NAN;
    CHECK(ft_detect(px1, 8, 8, &ax, FT_FILM_NEGATIVE, ft_holder_find("24x217"),
                    &L, &e) == -1);
  }

  /* empty holder */
  synth_default(&s, FT_FILM_NEGATIVE);
  s.film_y0 = s.film_y1 = 0;
  CHECK(run(&s) == -1);
  return TEST_DONE();
}

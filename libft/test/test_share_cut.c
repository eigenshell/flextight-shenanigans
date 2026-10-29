/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_share_cut.h"
#include "test/ftest.h"
#include "test/synth.h"

static void meta_for(const synth_win* w, const synth_win_scan* s, double share,
                     ft_share_metadata* m) {
  memset(m, 0, sizeof *m);
  m->ppi = w->ppi;
  m->bin = w->bin;
  m->pixel_count = s->px;
  m->first_pixel = s->x;
  m->first_line = s->y;
  m->film = FT_FILM_NEGATIVE;
  strcpy(m->holder, "24x217");
  m->share_pixels = share;
}

static void add_frame(ft_share_metadata* m, int idx, ft_frame_type t, double y0,
                      double len) {
  ft_share_frame* f = &m->frames[m->frame_count++];
  f->index = idx;
  f->type = t;
  f->along_start_mm = y0;
  f->length_mm = len;
  f->across_start_mm = 4.1;
  f->width_mm = 24;
}

/* mean G over a column (col >= 0) or a row (col < 0, row used) segment of the
 * cut image, from a to b exclusive */
static double mean_g(const ft_image* im, int col, int row, int a, int b) {
  double s = 0;
  int i;
  for(i = a; i < b; i++)
    s += col >= 0 ? im->pixels[((size_t)i * im->width + col) * 3 + 1]
                  : im->pixels[((size_t)row * im->width + i) * 3 + 1];
  return s / (b - a);
}

/* the box edges sit within a few px of the true frame edge in both axes:
 * base just outside, exposed just inside (base G is 0.40) */
static void edge_probes(const ft_image* im, const int* bx) {
  int x0 = bx[0], y0 = bx[1], x1 = bx[0] + bx[2], y1 = bx[1] + bx[3];
  CHECK_NEAR(mean_g(im, x0 - 3, 0, y0, y1), 0.40, 0.03);
  CHECK(mean_g(im, x0 + 3, 0, y0, y1) < 0.37);
  if(x1 + 2 < (int)im->width)
    CHECK_NEAR(mean_g(im, x1 + 2, 0, y0, y1), 0.40, 0.03);
  CHECK(mean_g(im, x1 - 4, 0, y0, y1) < 0.37);
  CHECK_NEAR(mean_g(im, -1, y0 - 3, x0, x1), 0.40, 0.03);
  CHECK(mean_g(im, -1, y0 + 3, x0, x1) < 0.37);
  if(y1 + 2 < (int)im->height)
    CHECK_NEAR(mean_g(im, -1, y1 + 2, x0, x1), 0.40, 0.03);
  CHECK(mean_g(im, -1, y1 - 4, x0, x1) < 0.37);
}

int main(void) {
  static ft_share_cut_result r;
  ft_share_metadata m;
  synth_win w;
  synth_win_scan s;
  ft_err e;
  int i;

  /* two half frames, crept +1.5 mm since the preview; 1050 ppi bin 2
   * (detection copy f = 2); a third preview box where the scan has
   * only the partial neighbour: estimated with the others' shift */
  memset(&w, 0, sizeof w);
  w.ppi = 1050;
  w.bin = 2;
  w.y0_mm = 38;
  w.len_mm = 41;
  w.x0_mm = 1.0;
  w.w_mm = 31;
  w.nfr = 3;
  w.fy0[0] = 22.5;
  w.flen[0] = 18; /* neighbour, partly in view */
  w.fy0[1] = 41.5;
  w.flen[1] = 18;
  w.fy0[2] = 60.5;
  w.flen[2] = 18;
  synth_window(&w, &s);
  CHECK(s.raw != NULL);
  meta_for(&w, &s, 800, &m);
  add_frame(&m, 4, FT_FRAME_HALF, 40, 18);
  add_frame(&m, 5, FT_FRAME_HALF, 59, 18);
  add_frame(&m, 3, FT_FRAME_HALF, 21, 18);
  CHECK(ft_cut_share_frames(s.raw, s.lines, s.cal, s.cal_lines, NULL, 0, NULL,
                            &m, &r, &e) == 0);
  CHECK(r.was_detected && r.frame_count == 3);
  /* the synthetic .cal is noiseless: a clamped profile (S 0, O at the
   * 0.25 DN^2 floor over the 4700 DN span), on every frame */
  CHECK(r.noise_result == 1);
  for(i = 0; i < (int)r.frame_count; i++) {
    CHECK(r.frames[i].image.has_noise_profile);
    CHECK(r.frames[i].image.noise_profile[0] == 0);
    CHECK_NEAR(r.frames[i].image.noise_profile[1], 0.25 / (4700.0 * 4700.0),
               1e-15);
  }

  for(i = 0; i < 2; i++) {
    ft_share_cut_frame* c = &r.frames[i];
    double px_mm = w.ppi / 25.4;
    CHECK((int)c->index == 4 + i && !c->is_estimated);
    CHECK_NEAR(c->shift_mm, 1.5, 0.1);
    /* along 18 mm (image x), across the 24 mm image area (image y) */
    CHECK_NEAR(c->box[2], 18 * px_mm, 0.15 * px_mm);
    CHECK_NEAR(c->box[3], 24 * px_mm, 0.3 * px_mm);
    /* 0.5 mm bleed on every side */
    CHECK_NEAR(c->box[0], 0.5 * px_mm, 0.15 * px_mm);
    CHECK_NEAR(c->box[1], 0.5 * px_mm, 0.3 * px_mm);
    CHECK_NEAR(c->image.width, c->box[2] + px_mm, 0.3 * px_mm);
    CHECK_NEAR(c->image.height, c->box[3] + px_mm, 0.6 * px_mm);
    /* 800 px on the long (across) side */
    CHECK_NEAR(c->export_scale, 800.0 / c->box[3], 1e-9);
    CHECK(c->image.ppi == 1050 && c->image.frame_number == c->index);
    /* inside the box: exposed (denser than base); bleed along: base */
    {
      const float* mid = c->image.pixels +
                         ((size_t)(c->box[1] + c->box[3] / 2) * c->image.width +
                          c->box[0] +
                          c->box[2] / 2) *
                           3;
      const float* gap =
        c->image.pixels +
        ((size_t)(c->box[1] + c->box[3] / 2) * c->image.width + 2) * 3;
      CHECK(mid[1] < 0.35f); /* base G is 0.40 */
      CHECK_NEAR(gap[1], 0.40, 0.04);
    }
    edge_probes(&c->image, c->box);
  }
  CHECK(r.frames[2].index == 3 && r.frames[2].is_estimated);
  CHECK_NEAR(r.frames[2].shift_mm, 1.5, 0.1); /* the median shift */
  /* clipped at the window start: 21 + 1.5 + 18 - 38 = 2.5 mm left */
  CHECK_NEAR(r.frames[2].box[2], 2.5 * w.ppi / 25.4, 0.2 * w.ppi / 25.4);
  ft_free_share_cut(&r);
  CHECK(r.frame_count == 0);

  /* a box entirely outside the window is an error */
  m.frames[2].along_start_mm = 150;
  CHECK(ft_cut_share_frames(s.raw, s.lines, s.cal, s.cal_lines, NULL, 0, NULL,
                            &m, &r, &e) == -1);
  CHECK(strstr(e.message, "frame 3") != NULL);
  CHECK(r.frame_count == 0);
  synth_window_free(&s);

  /* one full frame at 1500 ppi bin 1 (f = 3), crept -1 mm */
  memset(&w, 0, sizeof w);
  w.ppi = 1500;
  w.bin = 1;
  w.y0_mm = 98;
  w.len_mm = 40;
  w.x0_mm = 1.0;
  w.w_mm = 31;
  w.nfr = 1;
  w.fy0[0] = 99;
  w.flen[0] = 36;
  synth_window(&w, &s);
  CHECK(s.raw != NULL);
  meta_for(&w, &s, 2000, &m);
  add_frame(&m, 2, FT_FRAME_FULL, 100, 36);
  CHECK(ft_cut_share_frames(s.raw, s.lines, s.cal, s.cal_lines, NULL, 0, NULL,
                            &m, &r, &e) == 0);
  CHECK(r.frame_count == 1 && !r.frames[0].is_estimated);
  CHECK_NEAR(r.frames[0].shift_mm, -1.0, 0.1);
  CHECK_NEAR(r.frames[0].box[2], 36 * 1500 / 25.4, 0.15 * 1500 / 25.4);
  CHECK(r.frames[0].box[2] > r.frames[0].box[3]); /* landscape */
  edge_probes(&r.frames[0].image, r.frames[0].box);
  CHECK_NEAR(r.frames[0].export_scale, 2000.0 / r.frames[0].box[2], 1e-9);
  ft_free_share_cut(&r);
  synth_window_free(&s);

  /* nothing to detect (open beam): every frame estimated, shift 0 */
  w.open = 1;
  synth_window(&w, &s);
  CHECK(s.raw != NULL);
  CHECK(ft_cut_share_frames(s.raw, s.lines, s.cal, s.cal_lines, NULL, 0, NULL,
                            &m, &r, &e) == 0);
  CHECK(r.frame_count == 1 &&
        r.frames[0].is_estimated &&
        r.frames[0].shift_mm == 0);
  ft_free_share_cut(&r);
  synth_window_free(&s);

  /* meta: parse, required keys, write from a job */
  {
    static const char txt[] =
      "preset=crop\nppi=1500\npx=1890\nx=12\nlines=2400\ny=5787\n"
      "bin=1\nkept=2400\nfilm=slide\nholder=24x217\nshare=2000\n"
      "hf_half_res=1\nfilm_base=0.4120 0.6810 0.9929\n"
      "share_frame=3 half 41.000 18.000 4.100 24.000\n"
      "share_frame=4 half 60.000 18.000 4.100 24.000\n";
    static ft_job j;
    char buf[2048], *p;
    FILE* f;
    size_t n;
    CHECK(ft_parse_share_metadata(txt, sizeof txt - 1, &m, &e) == 0);
    CHECK(m.ppi == 1500 &&
          m.pixel_count == 1890 &&
          m.first_pixel == 12 &&
          m.first_line == 5787);
    CHECK(m.bin == 1 && m.film == FT_FILM_SLIDE && m.share_pixels == 2000);
    CHECK(m.half_frames_at_full_frame_ppi == 1 &&
          m.has_film_base &&
          m.frame_count == 2);
    CHECK_NEAR(m.film_base_density[2], 0.9929, 1e-9);
    CHECK(m.frames[1].index == 4 && m.frames[1].type == FT_FRAME_HALF);
    CHECK_NEAR(m.frames[1].along_start_mm, 60, 1e-9);
    CHECK_NEAR(m.frames[1].width_mm, 24, 1e-9);
    /* each required key */
    {
      static const char* req[] = {
        "ppi=", "px=", "\nx=", "\ny=", "bin=", "share=", "share_frame="};
      for(i = 0; i < 7; i++) {
        strcpy(buf, txt);
        while((p = strstr(buf, req[i])) != NULL)
          p[req[i][0] == '\n' ? 1 : 0] = '#'; /* key renamed */
        CHECK(ft_parse_share_metadata(buf, strlen(buf), &m, &e) == -1);
        CHECK(strstr(e.message, "missing") || strstr(e.message, "no share"));
      }
    }
    strcpy(buf, txt);
    strcat(buf, "share_frame=5 sideways 1 2 3 4\n");
    CHECK(ft_parse_share_metadata(buf, strlen(buf), &m, &e) == -1);
    strcpy(buf, txt);
    strcat(buf, "share_frame=3 half 1.000 18.000 4.100 24.000\n");
    CHECK(ft_parse_share_metadata(buf, strlen(buf), &m, &e) == -1);
    CHECK(strstr(e.message, "listed twice") != NULL);
    strcpy(buf, txt);
    strcat(buf, "bin=5\n");
    CHECK(ft_parse_share_metadata(buf, strlen(buf), &m, &e) == -1);
    /* write: only group 2's included frames */
    memset(&j, 0, sizeof j);
    strcpy(j.holder, "24x217");
    j.share_pixels = 2000;
    j.frame_count = 3;
    for(i = 0; i < 3; i++) {
      j.frames[i].index = i + 1;
      j.frames[i].type = FT_FRAME_FULL;
      j.frames[i].is_included = i != 2;
      j.frames[i].group = i == 0 ? 1 : 2;
      j.frames[i].along_start_mm = 4 + 38.0 * i;
      j.frames[i].length_mm = 36;
      j.frames[i].across_start_mm = 4.1;
      j.frames[i].width_mm = 24;
    }
    f = tmpfile();
    CHECK(ft_write_share_metadata(f, &j, 2) == 0);
    rewind(f);
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    CHECK(strstr(buf, "share_frame=2 full 42.000 36.000 4.100 24.000\n"));
    CHECK(!strstr(buf, "share_frame=1") && !strstr(buf, "share_frame=3"));
    CHECK(strstr(buf, "film=negative\n") && strstr(buf, "share=2000\n"));
    CHECK(!strstr(buf, "film_base"));
    /* round trip through the parser with the scan's own keys */
    {
      char all[2100];
      snprintf(all, sizeof all,
               "ppi=1500\npx=1890\nx=12\ny=5787\n"
               "bin=1\n%s",
               buf);
      CHECK(ft_parse_share_metadata(all, strlen(all), &m, &e) == 0);
    }
    CHECK(m.frame_count == 1 &&
          m.frames[0].index == 2 &&
          m.frames[0].length_mm == 36);
    /* a group with its own target (share_px): the window's share is
     * that target, so ftdev cut sizes its frames by it */
    j.frames[1].share_pixels = 3000;
    f = tmpfile();
    CHECK(ft_write_share_metadata(f, &j, 2) == 0);
    rewind(f);
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    CHECK(strstr(buf, "share=3000\n") && !strstr(buf, "share=2000\n"));
    f = tmpfile(); /* group 1 keeps the strip's */
    CHECK(ft_write_share_metadata(f, &j, 1) == 0);
    rewind(f);
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    CHECK(strstr(buf, "share=2000\n") != NULL);
  }

  /* sidecar round trip */
  {
    ft_share_cut_frame c;
    ft_share_sidecar sc;
    char buf[1024];
    FILE* f;
    size_t n;
    memset(&c, 0, sizeof c);
    c.index = 7;
    c.type = FT_FRAME_HALF;
    c.is_estimated = 1;
    c.shift_mm = 1.25;
    c.box[0] = 30;
    c.box[1] = 29;
    c.box[2] = 1063;
    c.box[3] = 1417;
    c.export_scale = 0.940720;
    m.film = FT_FILM_NEGATIVE;
    m.has_film_base = 1;
    m.film_base_density[0] = 0.41;
    m.film_base_density[1] = 0.68;
    m.film_base_density[2] = 0.99;
    m.ppi = 1500;
    m.share_pixels = 2000;
    m.half_frames_at_full_frame_ppi = 1;
    f = tmpfile();
    CHECK(ft_write_share_sidecar(f, &m, &c) == 0);
    rewind(f);
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    CHECK(ft_parse_share_sidecar(buf, n, &sc, &e) == 0);
    CHECK(sc.frame == 7 && sc.type == FT_FRAME_HALF && sc.is_estimated);
    CHECK(sc.box[0] == 30 &&
          sc.box[3] == 1417 &&
          sc.half_frames_at_full_frame_ppi == 1);
    CHECK_NEAR(sc.export_scale, 0.94072, 1e-6);
    CHECK_NEAR(sc.shift_mm, 1.25, 1e-9);
    CHECK(sc.has_film_base);
    CHECK_NEAR(sc.film_base_density[1], 0.68, 1e-9);
    CHECK(sc.ppi == 1500 && sc.share_pixels == 2000);
    CHECK(ft_parse_share_sidecar("box=1 2 3 4\n", 12, &sc, &e) == -1);
    CHECK(strstr(e.message, "export_scale") != NULL);
    CHECK(ft_parse_share_sidecar("export_scale=0.5\n", 17, &sc, &e) == -1);
    CHECK(strstr(e.message, "box") != NULL);
    CHECK(ft_parse_share_sidecar("box=1 2 3 4\nexport_scale=1.5\n", 29, &sc,
                                 &e) == -1);
    CHECK(!sc.has_noise_profile);

    /* noise_profile: round trip; not six numbers is an error; a
     * negative value reads as none */
    c.image.has_noise_profile = 1;
    c.image.noise_profile[0] = 1.234567891e-4;
    c.image.noise_profile[1] = 2.5e-8;
    c.image.noise_profile[2] = c.image.noise_profile[4] = 1e-4;
    c.image.noise_profile[3] = c.image.noise_profile[5] = 3e-8;
    f = tmpfile();
    CHECK(ft_write_share_sidecar(f, &m, &c) == 0);
    rewind(f);
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    CHECK(strstr(buf, "noise_profile=") != NULL);
    CHECK(ft_parse_share_sidecar(buf, n, &sc, &e) == 0);
    CHECK(sc.has_noise_profile);
    CHECK(sc.noise_profile[0] == 1.234567891e-4 &&
          sc.noise_profile[1] == 2.5e-8);
    {
      static const char few[] =
        "box=1 2 3 4\nexport_scale=0.5\nnoise_profile=1 2 3\n";
      static const char neg[] =
        "box=1 2 3 4\nexport_scale=0.5\n"
        "noise_profile=1e-4 -1 1e-4 1 1e-4 1\n";
      CHECK(ft_parse_share_sidecar(few, sizeof few - 1, &sc, &e) == -1);
      CHECK(strstr(e.message, "noise_profile") != NULL);
      CHECK(ft_parse_share_sidecar(neg, sizeof neg - 1, &sc, &e) == 0);
      CHECK(!sc.has_noise_profile);
    }
    /* the buffer writer: the FILE writer's text, snprintf's contract */
    {
      char b2[1024];
      int len = ft_format_share_sidecar(b2, sizeof b2, &m, &c);
      f = tmpfile();
      CHECK(ft_write_share_sidecar(f, &m, &c) == 0);
      rewind(f);
      n = fread(buf, 1, sizeof buf - 1, f);
      fclose(f);
      CHECK(len == (int)n && memcmp(b2, buf, n) == 0 && b2[n] == 0);
      CHECK(ft_format_share_sidecar(b2, 10, &m, &c) == len);
      CHECK(strlen(b2) == 9 && memcmp(b2, buf, 9) == 0);
      CHECK(ft_format_share_sidecar(NULL, 0, &m, &c) == len);
    }
  }
  return TEST_DONE();
}

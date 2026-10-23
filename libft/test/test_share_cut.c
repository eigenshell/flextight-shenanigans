/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_share_cut.h"
#include "test/ftest.h"
#include "test/synth.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void meta_for(
    const synth_win *w, const synth_win_scan *s, double share, ft_share_meta *m
) {
  memset(m, 0, sizeof *m);
  m->ppi = w->ppi;
  m->bin = w->bin;
  m->px = s->px;
  m->x = s->x;
  m->y = s->y;
  m->film = FT_FILM_NEGATIVE;
  strcpy(m->holder, "24x217");
  m->share = share;
}

static void add_frame(
    ft_share_meta *m, int idx, ft_frame_type t, double y0, double len
) {
  ft_share_frame *f = &m->f[m->nframes++];
  f->index = idx;
  f->type = t;
  f->y0 = y0;
  f->len = len;
  f->x0 = 4.1;
  f->width = 24;
}

/* mean G over a column (col >= 0) or a row (col < 0, row used) segment of the
 * cut image, from a to b exclusive */
static double mean_g(const ft_image *im, int col, int row, int a, int b) {
  double s = 0;
  int i;
  for(i = a; i < b; i++)
    s += col >= 0 ? im->rgb[((size_t)i * im->w + col) * 3 + 1]
                  : im->rgb[((size_t)row * im->w + i) * 3 + 1];
  return s / (b - a);
}

/* the box edges sit within a few px of the true frame edge in both axes:
 * base just outside, exposed just inside (base G is 0.40) */
static void edge_probes(const ft_image *im, const int *bx) {
  int x0 = bx[0], y0 = bx[1], x1 = bx[0] + bx[2], y1 = bx[1] + bx[3];
  CHECK_NEAR(mean_g(im, x0 - 3, 0, y0, y1), 0.40, 0.03);
  CHECK(mean_g(im, x0 + 3, 0, y0, y1) < 0.37);
  if(x1 + 2 < im->w)
    CHECK_NEAR(mean_g(im, x1 + 2, 0, y0, y1), 0.40, 0.03);
  CHECK(mean_g(im, x1 - 4, 0, y0, y1) < 0.37);
  CHECK_NEAR(mean_g(im, -1, y0 - 3, x0, x1), 0.40, 0.03);
  CHECK(mean_g(im, -1, y0 + 3, x0, x1) < 0.37);
  if(y1 + 2 < im->h)
    CHECK_NEAR(mean_g(im, -1, y1 + 2, x0, x1), 0.40, 0.03);
  CHECK(mean_g(im, -1, y1 - 4, x0, x1) < 0.37);
}

int main(void) {
  static ft_share_cut_result r;
  ft_share_meta m;
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
  CHECK(
      ft_share_cut(
          s.raw, s.lines, s.cal, s.cal_lines, NULL, 0, NULL, &m, &r, &e
      ) == 0
  );
  CHECK(r.detect_ok && r.n == 3);
  /* the synthetic .cal is noiseless: a clamped profile (S 0, O at the
   * 0.25 DN^2 floor over the 4700 DN span), on every frame */
  CHECK(r.noise_rc == 1);
  for(i = 0; i < r.n; i++) {
    CHECK(r.f[i].im.has_noise);
    CHECK(r.f[i].im.noise[0] == 0);
    CHECK_NEAR(r.f[i].im.noise[1], 0.25 / (4700.0 * 4700.0), 1e-15);
  }

  for(i = 0; i < 2; i++) {
    ft_share_cut_frame *c = &r.f[i];
    double px_mm = w.ppi / 25.4;
    CHECK(c->index == 4 + i && !c->estimated);
    CHECK_NEAR(c->shift_mm, 1.5, 0.1);
    /* along 18 mm (image x), across the 24 mm image area (image y) */
    CHECK_NEAR(c->box[2], 18 * px_mm, 0.15 * px_mm);
    CHECK_NEAR(c->box[3], 24 * px_mm, 0.3 * px_mm);
    /* 0.5 mm bleed on every side */
    CHECK_NEAR(c->box[0], 0.5 * px_mm, 0.15 * px_mm);
    CHECK_NEAR(c->box[1], 0.5 * px_mm, 0.3 * px_mm);
    CHECK_NEAR(c->im.w, c->box[2] + px_mm, 0.3 * px_mm);
    CHECK_NEAR(c->im.h, c->box[3] + px_mm, 0.6 * px_mm);
    /* 800 px on the long (across) side */
    CHECK_NEAR(c->export_scale, 800.0 / c->box[3], 1e-9);
    CHECK(c->im.ppi == 1050 && c->im.frame == c->index);
    /* inside the box: exposed (denser than base); bleed along: base */
    {
      const float *mid =
          c->im.rgb + ((size_t)(c->box[1] + c->box[3] / 2) * c->im.w +
                       c->box[0] + c->box[2] / 2) *
                          3;
      const float *gap =
          c->im.rgb + ((size_t)(c->box[1] + c->box[3] / 2) * c->im.w + 2) * 3;
      CHECK(mid[1] < 0.35f); /* base G is 0.40 */
      CHECK_NEAR(gap[1], 0.40, 0.04);
    }
    edge_probes(&c->im, c->box);
  }
  CHECK(r.f[2].index == 3 && r.f[2].estimated);
  CHECK_NEAR(r.f[2].shift_mm, 1.5, 0.1); /* the median shift */
  /* clipped at the window start: 21 + 1.5 + 18 - 38 = 2.5 mm left */
  CHECK_NEAR(r.f[2].box[2], 2.5 * w.ppi / 25.4, 0.2 * w.ppi / 25.4);
  ft_share_cut_free(&r);
  CHECK(r.n == 0);

  /* a box entirely outside the window is an error */
  m.f[2].y0 = 150;
  CHECK(
      ft_share_cut(
          s.raw, s.lines, s.cal, s.cal_lines, NULL, 0, NULL, &m, &r, &e
      ) == -1
  );
  CHECK(strstr(e.msg, "frame 3") != NULL);
  CHECK(r.n == 0);
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
  CHECK(
      ft_share_cut(
          s.raw, s.lines, s.cal, s.cal_lines, NULL, 0, NULL, &m, &r, &e
      ) == 0
  );
  CHECK(r.n == 1 && !r.f[0].estimated);
  CHECK_NEAR(r.f[0].shift_mm, -1.0, 0.1);
  CHECK_NEAR(r.f[0].box[2], 36 * 1500 / 25.4, 0.15 * 1500 / 25.4);
  CHECK(r.f[0].box[2] > r.f[0].box[3]); /* landscape */
  edge_probes(&r.f[0].im, r.f[0].box);
  CHECK_NEAR(r.f[0].export_scale, 2000.0 / r.f[0].box[2], 1e-9);
  ft_share_cut_free(&r);
  synth_window_free(&s);

  /* nothing to detect (open beam): every frame estimated, shift 0 */
  w.open = 1;
  synth_window(&w, &s);
  CHECK(s.raw != NULL);
  CHECK(
      ft_share_cut(
          s.raw, s.lines, s.cal, s.cal_lines, NULL, 0, NULL, &m, &r, &e
      ) == 0
  );
  CHECK(r.n == 1 && r.f[0].estimated && r.f[0].shift_mm == 0);
  ft_share_cut_free(&r);
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
    FILE *f;
    size_t n;
    CHECK(ft_share_meta_parse(txt, sizeof txt - 1, &m, &e) == 0);
    CHECK(m.ppi == 1500 && m.px == 1890 && m.x == 12 && m.y == 5787);
    CHECK(m.bin == 1 && m.film == FT_FILM_SLIDE && m.share == 2000);
    CHECK(m.hf_half_res == 1 && m.has_film_base && m.nframes == 2);
    CHECK_NEAR(m.film_base[2], 0.9929, 1e-9);
    CHECK(m.f[1].index == 4 && m.f[1].type == FT_FRAME_HALF);
    CHECK_NEAR(m.f[1].y0, 60, 1e-9);
    CHECK_NEAR(m.f[1].width, 24, 1e-9);
    /* each required key */
    {
      static const char *req[] = {"ppi=", "px=",    "\nx=",        "\ny=",
                                  "bin=", "share=", "share_frame="};
      for(i = 0; i < 7; i++) {
        strcpy(buf, txt);
        while((p = strstr(buf, req[i])) != NULL)
          p[req[i][0] == '\n' ? 1 : 0] = '#'; /* key renamed */
        CHECK(ft_share_meta_parse(buf, strlen(buf), &m, &e) == -1);
        CHECK(strstr(e.msg, "missing") || strstr(e.msg, "no share"));
      }
    }
    strcpy(buf, txt);
    strcat(buf, "share_frame=5 sideways 1 2 3 4\n");
    CHECK(ft_share_meta_parse(buf, strlen(buf), &m, &e) == -1);
    strcpy(buf, txt);
    strcat(buf, "share_frame=3 half 1.000 18.000 4.100 24.000\n");
    CHECK(ft_share_meta_parse(buf, strlen(buf), &m, &e) == -1);
    CHECK(strstr(e.msg, "listed twice") != NULL);
    strcpy(buf, txt);
    strcat(buf, "bin=5\n");
    CHECK(ft_share_meta_parse(buf, strlen(buf), &m, &e) == -1);
    /* write: only group 2's included frames */
    memset(&j, 0, sizeof j);
    strcpy(j.holder, "24x217");
    j.share = 2000;
    j.nframes = 3;
    for(i = 0; i < 3; i++) {
      j.f[i].index = i + 1;
      j.f[i].type = FT_FRAME_FULL;
      j.f[i].include = i != 2;
      j.f[i].group = i == 0 ? 1 : 2;
      j.f[i].y0 = 4 + 38.0 * i;
      j.f[i].len = 36;
      j.f[i].x0 = 4.1;
      j.f[i].width = 24;
    }
    f = tmpfile();
    CHECK(ft_share_meta_write(f, &j, 2) == 0);
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
      snprintf(
          all, sizeof all,
          "ppi=1500\npx=1890\nx=12\ny=5787\n"
          "bin=1\n%s",
          buf
      );
      CHECK(ft_share_meta_parse(all, strlen(all), &m, &e) == 0);
    }
    CHECK(m.nframes == 1 && m.f[0].index == 2 && m.f[0].len == 36);
    /* a group with its own target (share_px): the window's share is
     * that target, so ftdev cut sizes its frames by it */
    j.f[1].share_px = 3000;
    f = tmpfile();
    CHECK(ft_share_meta_write(f, &j, 2) == 0);
    rewind(f);
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    CHECK(strstr(buf, "share=3000\n") && !strstr(buf, "share=2000\n"));
    f = tmpfile(); /* group 1 keeps the strip's */
    CHECK(ft_share_meta_write(f, &j, 1) == 0);
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
    FILE *f;
    size_t n;
    memset(&c, 0, sizeof c);
    c.index = 7;
    c.type = FT_FRAME_HALF;
    c.estimated = 1;
    c.shift_mm = 1.25;
    c.box[0] = 30;
    c.box[1] = 29;
    c.box[2] = 1063;
    c.box[3] = 1417;
    c.export_scale = 0.940720;
    m.film = FT_FILM_NEGATIVE;
    m.has_film_base = 1;
    m.film_base[0] = 0.41;
    m.film_base[1] = 0.68;
    m.film_base[2] = 0.99;
    m.ppi = 1500;
    m.share = 2000;
    m.hf_half_res = 1;
    f = tmpfile();
    CHECK(ft_share_sidecar_write(f, &m, &c) == 0);
    rewind(f);
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    CHECK(ft_share_sidecar_parse(buf, n, &sc, &e) == 0);
    CHECK(sc.frame == 7 && sc.type == FT_FRAME_HALF && sc.estimated);
    CHECK(sc.box[0] == 30 && sc.box[3] == 1417 && sc.hf_half_res == 1);
    CHECK_NEAR(sc.export_scale, 0.94072, 1e-6);
    CHECK_NEAR(sc.shift_mm, 1.25, 1e-9);
    CHECK(sc.has_film_base);
    CHECK_NEAR(sc.film_base[1], 0.68, 1e-9);
    CHECK(sc.ppi == 1500 && sc.share == 2000);
    CHECK(ft_share_sidecar_parse("box=1 2 3 4\n", 12, &sc, &e) == -1);
    CHECK(strstr(e.msg, "export_scale") != NULL);
    CHECK(ft_share_sidecar_parse("export_scale=0.5\n", 17, &sc, &e) == -1);
    CHECK(strstr(e.msg, "box") != NULL);
    CHECK(
        ft_share_sidecar_parse(
            "box=1 2 3 4\nexport_scale=1.5\n", 29, &sc, &e
        ) == -1
    );
    CHECK(!sc.has_noise);

    /* noise_profile: round trip; not six numbers is an error; a
     * negative value reads as none */
    c.im.has_noise = 1;
    c.im.noise[0] = 1.234567891e-4;
    c.im.noise[1] = 2.5e-8;
    c.im.noise[2] = c.im.noise[4] = 1e-4;
    c.im.noise[3] = c.im.noise[5] = 3e-8;
    f = tmpfile();
    CHECK(ft_share_sidecar_write(f, &m, &c) == 0);
    rewind(f);
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    CHECK(strstr(buf, "noise_profile=") != NULL);
    CHECK(ft_share_sidecar_parse(buf, n, &sc, &e) == 0);
    CHECK(sc.has_noise);
    CHECK(sc.noise[0] == 1.234567891e-4 && sc.noise[1] == 2.5e-8);
    {
      static const char few[] =
          "box=1 2 3 4\nexport_scale=0.5\nnoise_profile=1 2 3\n";
      static const char neg[] = "box=1 2 3 4\nexport_scale=0.5\n"
                                "noise_profile=1e-4 -1 1e-4 1 1e-4 1\n";
      CHECK(ft_share_sidecar_parse(few, sizeof few - 1, &sc, &e) == -1);
      CHECK(strstr(e.msg, "noise_profile") != NULL);
      CHECK(ft_share_sidecar_parse(neg, sizeof neg - 1, &sc, &e) == 0);
      CHECK(!sc.has_noise);
    }
    /* the buffer writer: the FILE writer's text, snprintf's contract */
    {
      char b2[1024];
      int len = ft_share_sidecar_write_buf(b2, sizeof b2, &m, &c);
      f = tmpfile();
      CHECK(ft_share_sidecar_write(f, &m, &c) == 0);
      rewind(f);
      n = fread(buf, 1, sizeof buf - 1, f);
      fclose(f);
      CHECK(len == (int)n && memcmp(b2, buf, n) == 0 && b2[n] == 0);
      CHECK(ft_share_sidecar_write_buf(b2, 10, &m, &c) == len);
      CHECK(strlen(b2) == 9 && memcmp(b2, buf, 9) == 0);
      CHECK(ft_share_sidecar_write_buf(NULL, 0, &m, &c) == len);
    }
  }
  return TEST_DONE();
}

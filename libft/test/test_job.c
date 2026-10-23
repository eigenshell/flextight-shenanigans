/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_focus.h"
#include "ft_job.h"
#include "test/ftest.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static FILE *text(const char *s) {
  FILE *f = tmpfile();
  fputs(s, f);
  rewind(f);
  return f;
}

int main(void) {
  ft_job a, b;
  ft_err e;
  FILE *f;

  memset(&a, 0, sizeof a);
  strcpy(a.holder, "24x217");
  a.film = FT_FILM_SLIDE;
  a.focus = FT_FOCUS_PER_FRAME;
  a.focus_offset = 2400;
  a.nframes = 2;
  a.f[0].index = 1;
  a.f[0].type = FT_FRAME_FULL;
  a.f[0].include = 1;
  a.f[0].ppi = 3150;
  a.f[0].y0 = 4.1;
  a.f[0].x0 = 1.6;
  a.f[0].len = 36.2;
  a.f[0].width = 24.4;
  a.f[0].tilt_deg = 0.12;
  a.f[1].index = 2;
  a.f[1].type = FT_FRAME_PARTIAL;
  a.f[1].include = 0;
  a.f[1].ppi = 2000;
  a.f[1].y0 = 42;
  a.f[1].x0 = 4.1;
  a.f[1].len = 20;
  a.f[1].width = 24;
  a.f[1].sprockets = 1;
  a.f[1].straighten = 1;
  a.f[1].confidence_low = 1;
  a.f[1].focus_offset = -800;
  a.f[1].done = 1;

  /* round trip */
  f = tmpfile();
  CHECK(ft_job_write(&a, f) == 0);
  rewind(f);
  CHECK(ft_job_read(&b, f, &e) == 0);
  fclose(f);
  CHECK(strcmp(b.holder, "24x217") == 0);
  CHECK(b.film == FT_FILM_SLIDE && b.focus == FT_FOCUS_PER_FRAME);
  CHECK(b.focus_offset == 2400 && b.nframes == 2);
  CHECK(b.f[0].index == 1 && b.f[0].type == FT_FRAME_FULL && b.f[0].include);
  CHECK_NEAR(b.f[0].ppi, 3150, 1e-9);
  CHECK_NEAR(b.f[0].len, 36.2, 0.006);
  CHECK_NEAR(b.f[0].tilt_deg, 0.12, 0.006);
  CHECK(b.f[1].type == FT_FRAME_PARTIAL && !b.f[1].include);
  CHECK(b.f[1].sprockets && b.f[1].straighten && b.f[1].confidence_low);
  CHECK(b.f[1].focus_offset == -800 && b.f[1].done);
  CHECK(b.f[0].line > 0 && b.f[1].line > b.f[0].line);
  CHECK(b.f[0].bin == 0 && ft_job_bin(&b.f[0]) == 2); /* fastest */

  /* bin: per-frame optics; written, read back, options listed */
  {
    char buf[4096];
    size_t n;
    ft_job c = a;
    c.f[1].bin = 1;
    f = tmpfile();
    CHECK(ft_job_write(&c, f) == 0);
    rewind(f);
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    CHECK(strstr(buf, "bin = 0") != NULL && strstr(buf, "bin = 1") != NULL);
    /* 3150: bins 2 and 1; 2000: 3, 2 and 1 */
    CHECK(strstr(buf, "# bin options at 3150 ppi") != NULL);
    CHECK(
        strstr(
            buf, "# bin options at 2000 ppi (time, light, Dmax~): "
                 "3: "
        ) != NULL
    );
    rewind(f);
    CHECK(ft_job_read(&b, f, &e) == 0);
    fclose(f);
    CHECK(b.f[0].bin == 0 && b.f[1].bin == 1 && ft_job_bin(&b.f[1]) == 1);
  }
  /* detailed scans: strip keys and the frame's pass, written only for a
   * detail job (older jobs read and write as before) */
  {
    ft_job c = a, d2;
    int k;
    f = tmpfile();
    CHECK(ft_job_write(&c, f) == 0);
    rewind(f);
    {
      char buf[4096];
      size_t n = fread(buf, 1, sizeof buf - 1, f);
      buf[n] = 0;
      CHECK(strstr(buf, "detail") == NULL && strstr(buf, "pass") == NULL);
    }
    fclose(f);
    c.film = FT_FILM_NEGATIVE;
    c.detail = FT_DETAIL_POS;
    c.detail_fmt = FT_DFMT_TIFF;
    c.detail_bw = FT_BW_MONO;
    c.keep_raw = 1;
    c.ring_mb = 256;
    c.f[0].pass = 1;
    c.f[1].pass = 2;
    f = tmpfile();
    CHECK(ft_job_write(&c, f) == 0);
    rewind(f);
    CHECK(ft_job_read(&d2, f, &e) == 0);
    fclose(f);
    CHECK(d2.detail == FT_DETAIL_POS && d2.detail_fmt == FT_DFMT_TIFF);
    CHECK(d2.detail_bw == FT_BW_MONO && d2.keep_raw && d2.ring_mb == 256);
    CHECK(d2.f[0].pass == 1 && d2.f[1].pass == 2);

    f = text("[strip]\nholder = 24x217\n");
    CHECK(ft_job_read(&d2, f, &e) == 0);
    fclose(f);
    CHECK(d2.detail == FT_DETAIL_NONE && d2.ring_mb == FT_RING_MB_DEFAULT);
    {
      static const char *bad[] = {
          "detail = both\n",    "detail = none\n",  "detail_fmt = jpg\n",
          "detail_bw = grey\n", "keep_raw = 1\n",   "ring_mb = 4\n",
          "ring_mb = 1025\n",   "ring_mb = 4096\n", NULL
      };
      for(k = 0; bad[k]; k++) {
        char t2[128];
        snprintf(t2, sizeof t2, "[strip]\nholder = 24x217\n%s", bad[k]);
        f = text(t2);
        CHECK(ft_job_read(&d2, f, &e) == -1);
        fclose(f);
      }
    }
    /* the largest ring a 32-bit process can still allocate */
    f = text("[strip]\nholder = 24x217\nring_mb = 1024\n");
    CHECK(ft_job_read(&d2, f, &e) == 0 && d2.ring_mb == 1024);
    fclose(f);
    CHECK((double)FT_RING_MB_MAX * 1048576 < 2147483648.0);

    /* frames still to scan in a pass: a resumed job skips the passes
     * it finished (done) and those without an included frame */
    d2 = c;
    d2.f[1].include = 1;
    d2.f[1].done = 0;
    CHECK(ft_job_pass_todo(&d2, 1) == 1 && ft_job_pass_todo(&d2, 2) == 1);
    d2.f[0].done = 1;
    CHECK(ft_job_pass_todo(&d2, 1) == 0 && ft_job_pass_todo(&d2, 2) == 1);
    d2.f[1].include = 0;
    CHECK(ft_job_pass_todo(&d2, 2) == 0 && ft_job_pass_todo(&d2, 3) == 0);
    d2.f[0].done = 0;
    d2.f[1].include = 1;
    d2.f[1].pass = 1;
    CHECK(ft_job_pass_todo(&d2, 1) == 2);
    d2.f[1].done = 1;
    CHECK(ft_job_pass_todo(&d2, 1) == 1); /* one of two left */

    f = text(
        "[strip]\nholder = 24x217\n[frame 1]\nppi = 3150\n"
        "window = 4 4 36 24\npass = -1\n"
    );
    CHECK(ft_job_read(&d2, f, &e) == -1);
    fclose(f);

    /* validation */
    c.has_film_base = 1;
    c.film_base[0] = 0.3;
    c.film_base[1] = 0.6;
    c.film_base[2] = 0.9;
    c.f[0].bin = 1;
    c.f[1].include = 0;
    c.focus = FT_FOCUS_SHIFTED;
    c.focus_offset = 0;
    CHECK(ft_job_validate(&c, &ft_lens_default, &e) == 0);
    d2 = c;
    d2.film = FT_FILM_SLIDE;
    CHECK(ft_job_validate(&d2, &ft_lens_default, &e) == -1);
    CHECK(strstr(e.msg, "negative") != NULL);
    d2 = c;
    d2.detail_fmt = FT_DFMT_DNG;
    CHECK(ft_job_validate(&d2, &ft_lens_default, &e) == -1);
    d2 = c;
    d2.detail = FT_DETAIL_NEG;
    CHECK(ft_job_validate(&d2, &ft_lens_default, &e) == -1); /* bw */
    d2.detail_bw = FT_BW_OFF;
    d2.film = FT_FILM_SLIDE;
    CHECK(ft_job_validate(&d2, &ft_lens_default, &e) == 0);
    d2 = c;
    d2.has_film_base = 0;
    CHECK(ft_job_validate(&d2, &ft_lens_default, &e) == -1);
    d2 = c;
    d2.f[0].pass = 0;
    CHECK(ft_job_validate(&d2, &ft_lens_default, &e) == -1);
    d2 = c;
    d2.f[0].bin = 2; /* 3150 x 2 = 6300 optics */
    CHECK(ft_job_validate(&d2, &ft_lens_default, &e) == -1);
    CHECK(strstr(e.msg, "bin 1") != NULL);
    d2 = c;
    d2.f[1].include = 1;
    d2.f[1].pass = 1;
    d2.f[1].ppi = 6300;
    d2.f[1].bin = 1;
    d2.f[1].type = FT_FRAME_FULL;
    CHECK(ft_job_validate(&d2, &ft_lens_default, &e) == -1);
    CHECK(strstr(e.msg, "mixes") != NULL);
  }

  /* film_base: written with the strip, read back; absent = not set */
  {
    ft_job c = a;
    c.has_film_base = 1;
    c.film_base[0] = 0.2512;
    c.film_base[1] = 0.6;
    c.film_base[2] = 0.9177;
    f = tmpfile();
    CHECK(ft_job_write(&c, f) == 0);
    rewind(f);
    CHECK(ft_job_read(&b, f, &e) == 0);
    fclose(f);
    CHECK(b.has_film_base);
    CHECK_NEAR(b.film_base[0], 0.2512, 1e-9);
    CHECK_NEAR(b.film_base[2], 0.9177, 1e-9);
    f = tmpfile();
    CHECK(ft_job_write(&a, f) == 0); /* a has none */
    rewind(f);
    CHECK(ft_job_read(&b, f, &e) == 0);
    fclose(f);
    CHECK(!b.has_film_base);
    f = text("[strip]\nholder = 24x217\nfilm_base = 0.2 0.5\n");
    CHECK(ft_job_read(&b, f, &e) == -1); /* two values */
    fclose(f);
    f = text("[strip]\nholder = 24x217\nfilm_base = 0.2 0.5 5\n");
    CHECK(ft_job_read(&b, f, &e) == -1); /* above 4.5 D */
    fclose(f);
    f = text("[strip]\nholder = 24x217\nfilm_base = 0.2 0.5 0.9 1\n");
    CHECK(ft_job_read(&b, f, &e) == -1); /* four values */
    fclose(f);
    f = text("[strip]\nholder = 24x217\nfilm_base = 0.2 0.5 0.9   # comment\n");
    CHECK(ft_job_read(&b, f, &e) == 0 && b.has_film_base);
    fclose(f);
    f = text("[strip]\nholder = 24x217\nfilm_base = -0.1 0.5 0.9\n");
    CHECK(ft_job_read(&b, f, &e) == -1); /* negative D */
    fclose(f);
  }
  /* share: strip keys and per-frame groups round trip; groups are
   * written only in a share job */
  {
    ft_job c;
    char buf[4096];
    size_t n;
    memset(&c, 0, sizeof c);
    strcpy(c.holder, "24x217");
    c.share = 2000;
    c.hf_half_res = 1;
    c.nframes = 3;
    for(n = 0; n < 3; n++) {
      c.f[n].index = (int)n + 1;
      c.f[n].type = FT_FRAME_HALF;
      c.f[n].include = n != 1;
      c.f[n].ppi = 1500;
      c.f[n].bin = 1;
      c.f[n].y0 = 3 + 19.0 * n;
      c.f[n].x0 = 4;
      c.f[n].len = 18;
      c.f[n].width = 24;
      c.f[n].group = n == 0 ? 1 : n == 2 ? 2 : 0;
    }
    f = tmpfile();
    CHECK(ft_job_write(&c, f) == 0);
    rewind(f);
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    CHECK(strstr(buf, "share = 2000") && strstr(buf, "hf_half_res = yes"));
    CHECK(strstr(buf, "group = 2") != NULL);
    rewind(f);
    CHECK(ft_job_read(&b, f, &e) == 0);
    fclose(f);
    CHECK(b.share == 2000 && b.hf_half_res == 1);
    CHECK(b.f[0].group == 1 && b.f[1].group == 0 && b.f[2].group == 2);
    CHECK(ft_job_validate(&b, &ft_lens_default, &e) == 0);
    /* an ordinary job writes no share keys and no groups */
    f = tmpfile();
    CHECK(ft_job_write(&a, f) == 0);
    rewind(f);
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    CHECK(!strstr(buf, "share") && !strstr(buf, "group"));
    /* share rules */
    b.f[2].group = 0; /* included, no group */
    CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
    CHECK(strstr(e.msg, "without a group") != NULL);
    b.f[2].group = 2;
    b.f[2].bin = 2; /* own group: fine */
    CHECK(ft_job_validate(&b, &ft_lens_default, &e) == 0);
    b.f[2].group = 1; /* same group, other bin */
    CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
    CHECK(strstr(e.msg, "differ") != NULL);
    b.f[2].bin = 1;
    CHECK(ft_job_validate(&b, &ft_lens_default, &e) == 0);
    b.f[1].include = 1; /* 1, 2, 1: split */
    b.f[1].group = 2;
    CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
    CHECK(strstr(e.msg, "split") != NULL);
    b.f[1].include = 0;
    b.f[0].y0 = 50; /* out of strip order */
    CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
    CHECK(strstr(e.msg, "follow the strip") != NULL);
    b.f[0].y0 = 3;
    b.share = 0; /* groups without share */
    CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
    CHECK(strstr(e.msg, "group without share") != NULL);
    /* reader limits */
    f = text("[strip]\nholder = 24x217\nshare = 50\n");
    CHECK(ft_job_read(&b, f, &e) == -1);
    fclose(f);
    f = text("[strip]\nholder = 24x217\nhf_half_res = maybe\n");
    CHECK(ft_job_read(&b, f, &e) == -1);
    fclose(f);
    f = text(
        "[strip]\nholder = 24x217\n[frame 1]\nppi = 1500\n"
        "window = 50 4 36 24\ngroup = -1\n"
    );
    CHECK(ft_job_read(&b, f, &e) == -1);
    fclose(f);

    /* share_px: a frame's own target (the web page's 4MP / 6MP);
     * written only when set, one target per group */
    b = c;
    CHECK(ft_job_share_px(&b, &b.f[0]) == 2000);
    b.f[2].share_px = 3000;
    CHECK(ft_job_share_px(&b, &b.f[2]) == 3000);
    n = ft_job_write_buf(&b, buf, sizeof buf);
    CHECK(n < sizeof buf);
    CHECK(
        strstr(
            buf, "group = 2   # window: frames of one group scan "
                 "together\nshare_px = 3000   # this frame's "
                 "target, px on its long side\n"
        ) != NULL
    );
    CHECK(strstr(strstr(buf, "share_px") + 1, "share_px") == NULL);
    {
      ft_job d;
      CHECK(ft_job_parse(&d, buf, n, &e) == 0);
      CHECK(d.f[0].share_px == 0 && d.f[2].share_px == 3000);
      CHECK(ft_job_validate(&d, &ft_lens_default, &e) == 0);
      d.f[0].group = 2; /* one window, two targets */
      CHECK(ft_job_validate(&d, &ft_lens_default, &e) == -1);
      CHECK(
          strstr(
              e.msg, "(frame 3): frames of group 2 differ in "
                     "share_px"
          ) != NULL
      );
      d.f[0].share_px = 3000; /* same target: fine */
      CHECK(ft_job_validate(&d, &ft_lens_default, &e) == 0);
      d.share = 0; /* share_px needs share */
      d.f[0].group = d.f[2].group = 0;
      CHECK(ft_job_validate(&d, &ft_lens_default, &e) == -1);
      CHECK(
          strstr(
              e.msg, "(frame 1): share_px without share in "
                     "[strip]"
          ) != NULL
      );
    }
    f = text(
        "[strip]\nholder = 24x217\nshare = 2000\n[frame 1]\n"
        "ppi = 1500\nwindow = 50 4 36 24\nshare_px = 50\n"
    );
    CHECK(
        ft_job_read(&b, f, &e) == -1 &&
        strcmp(e.msg, "line 7: bad value for share_px: 50") == 0
    );
    fclose(f);
  }
  /* a job without bin keys (older files) scans with the fastest bin */
  f = text(
      "[strip]\nholder = 24x217\n[frame 1]\nppi = 2000\n"
      "window = 50 4 36 24\n"
  );
  CHECK(ft_job_read(&b, f, &e) == 0);
  fclose(f);
  CHECK(b.f[0].bin == 0 && ft_job_bin(&b.f[0]) == 3);
  CHECK(ft_job_validate(&b, &ft_lens_default, &e) == 0);
  b.f[0].bin = 1; /* optics 2000: usable */
  CHECK(ft_job_validate(&b, &ft_lens_default, &e) == 0);
  b.f[0].bin = 6; /* optics 12000: no */
  CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
  CHECK(strstr(e.msg, "bin 6") != NULL);
  f = text(
      "[strip]\nholder = 24x217\n[frame 1]\nppi = 2000\nbin = 13\n"
      "window = 50 4 36 24\n"
  );
  CHECK(ft_job_read(&b, f, &e) == -1);
  fclose(f);

  /* parse errors carry the line number */
  f = text("[strip]\nholder = 24x217\nbogus = 1\n");
  CHECK(ft_job_read(&b, f, &e) == -1);
  CHECK(strstr(e.msg, "line 3") != NULL);
  fclose(f);
  f = text("[strip]\n[frame 1]\nppi = fast\n");
  CHECK(ft_job_read(&b, f, &e) == -1);
  CHECK(strstr(e.msg, "line 3") != NULL);
  fclose(f);
  f = text("ppi = 2000\n");
  CHECK(ft_job_read(&b, f, &e) == -1);
  fclose(f);

  /* validation: holder, ppi, window, optics */
  CHECK(ft_job_validate(&a, &ft_lens_default, &e) == 0);
  b = a;
  b.f[0].ppi = 6400;
  CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
  CHECK(strstr(e.msg, "frame 1") != NULL);
  b = a;
  b.f[0].y0 = 200; /* runs past 231 mm */
  CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
  b = a;
  b.f[1].ppi = 6400; /* excluded frames are skipped */
  CHECK(ft_job_validate(&b, &ft_lens_default, &e) == 0);
  b = a;
  strcpy(b.holder, "60x180");
  CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
  b = a;
  b.focus = FT_FOCUS_SHIFTED;
  b.focus_offset = 900000; /* CCD/lens past their range */
  CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
  /* hardening */
  f = text(
      "[strip]\r\nholder = 24x217\r\n[frame 1]\r\nppi = 2000\r\n"
      "window = 1 2 3 4\r\n"
  );
  CHECK(ft_job_read(&b, f, &e) == 0 && b.nframes == 1);
  fclose(f);
  {
    char big[400] = "[strip]\nholder = 24x217\n# ";
    memset(big + 26, 'x', 300);
    strcpy(big + 326, "\n");
    f = text(big);
    CHECK(ft_job_read(&b, f, &e) == -1);
    CHECK(strstr(e.msg, "line 3") && strstr(e.msg, "too long"));
    fclose(f);
  }
#define GOOD "[strip]\nholder = 24x217\n"
#define FR "[frame 1]\nppi = 2000\nwindow = 1 2 3 4\n"
  f = text(GOOD FR "[frame 1]\nppi = 2000\nwindow = 1 2 3 4\n");
  CHECK(ft_job_read(&b, f, &e) == -1 && strstr(e.msg, "repeated"));
  fclose(f);
  f = text(GOOD "[frame 1 junk]\nppi = 2000\nwindow = 1 2 3 4\n");
  CHECK(ft_job_read(&b, f, &e) == -1);
  fclose(f);
  f = text(GOOD "[frame 0]\nppi = 2000\nwindow = 1 2 3 4\n");
  CHECK(ft_job_read(&b, f, &e) == -1);
  fclose(f);
  f = text(GOOD "[frame 1]\nppi = 2000\nwindow = nan 2 3 4\n");
  CHECK(ft_job_read(&b, f, &e) == -1 && strstr(e.msg, "line 5"));
  fclose(f);
  f = text(GOOD "[frame 1]\nppi = inf\nwindow = 1 2 3 4\n");
  CHECK(ft_job_read(&b, f, &e) == -1);
  fclose(f);
  f = text(GOOD "[frame 1]\nppi = 2000\nwindow = 1 2 3 4 5\n");
  CHECK(ft_job_read(&b, f, &e) == -1);
  fclose(f);
  f = text(GOOD "[frame 1]\nppi = 2000\n");
  CHECK(ft_job_read(&b, f, &e) == -1 && strstr(e.msg, "no window"));
  fclose(f);
  f = text(GOOD "[frame 1]\nwindow = 1 2 3 4\n");
  CHECK(ft_job_read(&b, f, &e) == -1 && strstr(e.msg, "no ppi"));
  fclose(f);
  f = text("[strip]\n" FR);
  CHECK(ft_job_read(&b, f, &e) == -1 && strstr(e.msg, "no holder"));
  fclose(f);
  f = text(FR);
  CHECK(ft_job_read(&b, f, &e) == -1);
  fclose(f);
  f = text("");
  CHECK(ft_job_read(&b, f, &e) == -1 && strstr(e.msg, "[strip]"));
  fclose(f);
  f = text(GOOD "[strip]\n");
  CHECK(ft_job_read(&b, f, &e) == -1 && strstr(e.msg, "line 3"));
  fclose(f);
  f = text(GOOD "film = slide\nfilm = slide\n");
  CHECK(ft_job_read(&b, f, &e) == -1 && strstr(e.msg, "duplicate key"));
  fclose(f);
  f = text(GOOD "focus_offset = 99999999999999999999\n");
  CHECK(ft_job_read(&b, f, &e) == -1);
  fclose(f);
  f = text(GOOD "film = slide  # comment\n" FR);
  CHECK(ft_job_read(&b, f, &e) == 0 && b.film == FT_FILM_SLIDE);
  fclose(f);

  /* validation hardening */
  b = a;
  b.focus = FT_FOCUS_PER_FRAME;
  b.f[0].focus_offset = 900000;
  CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
  b = a;
  b.f[0].focus_offset = -2000000000L;
  CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
  b = a;
  b.f[0].tilt_deg = 60;
  CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
  b = a;
  b.f[0].len = NAN;
  CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
  b = a;
  b.nframes = 0;
  CHECK(
      ft_job_validate(&b, &ft_lens_default, &e) == -1 &&
      strstr(e.msg, "no frames")
  );

  /* buffers (the web page): the text is the FILE writer's, byte for byte */
  {
    static const char golden[] =
        "[strip]\n"
        "holder = 24x217\n"
        "film = negative\n"
        "focus = shifted\n"
        "focus_offset = -1200\n"
        "film_base = 0.2512 0.6000 0.9123   # D r g b: unexposed film\n"
        "detail = neg   # neg: digital negative, pos: archive positive\n"
        "detail_fmt = tiff\n"
        "detail_bw = off\n"
        "keep_raw = yes\n"
        "ring_mb = 64\n"
        "share = 2450   # scan to share: px on each frame's long side\n"
        "hf_half_res = yes\n"
        "\n"
        "[frame 1]\n"
        "type = full\n"
        "include = yes\n"
        "ppi = 1729.5\n"
        "bin = 2   # 0 = fastest; smaller = more light, slower\n"
        "# bin options at 1729.5 ppi (time, light, Dmax~): 3: 96 s 1.3x 3.11 "
        "| "
        "2: 139 s 2.1x 3.32 | 1: 268 s 4.0x 3.60\n"
        "window = 4.100 1.600 36.200 24.400   # y0 x0 length width, mm\n"
        "tilt = 0.12\n"
        "sprockets = no\n"
        "straighten = no\n"
        "confidence = high\n"
        "focus_offset = 0\n"
        "done = no\n"
        "pass = 1\n"
        "group = 1   # window: frames of one group scan together\n"
        "\n"
        "[frame 7]\n"
        "type = half\n"
        "include = no\n"
        "ppi = 6300\n"
        "bin = 0   # 0 = fastest; smaller = more light, slower\n"
        "window = 42.000 4.100 18.000 24.000   # y0 x0 length width, mm\n"
        "tilt = 0.00\n"
        "sprockets = yes\n"
        "straighten = yes\n"
        "confidence = low\n"
        "focus_offset = -800\n"
        "done = yes\n"
        "pass = 0\n"
        "group = 2   # window: frames of one group scan together\n";
    static char buf[8192], buf2[8192], small[16];
    ft_job g;
    size_t n, n2;

    memset(&g, 0, sizeof g);
    strcpy(g.holder, "24x217");
    g.film = FT_FILM_NEGATIVE;
    g.focus = FT_FOCUS_SHIFTED;
    g.focus_offset = -1200;
    g.has_film_base = 1;
    g.film_base[0] = 0.2512;
    g.film_base[1] = 0.6;
    g.film_base[2] = 0.91234;
    g.detail = FT_DETAIL_NEG;
    g.detail_fmt = FT_DFMT_TIFF;
    g.ring_mb = 64;
    g.keep_raw = 1;
    g.share = 2450;
    g.hf_half_res = 1;
    g.nframes = 2;
    g.f[0].index = 1;
    g.f[0].type = FT_FRAME_FULL;
    g.f[0].include = 1;
    g.f[0].ppi = 1729.5;
    g.f[0].bin = 2;
    g.f[0].y0 = 4.1;
    g.f[0].x0 = 1.6;
    g.f[0].len = 36.2;
    g.f[0].width = 24.4;
    g.f[0].tilt_deg = 0.12;
    g.f[0].pass = 1;
    g.f[0].group = 1;
    g.f[1].index = 7;
    g.f[1].type = FT_FRAME_HALF;
    g.f[1].include = 0;
    g.f[1].ppi = 6300;
    g.f[1].y0 = 42;
    g.f[1].x0 = 4.1;
    g.f[1].len = 18;
    g.f[1].width = 24;
    g.f[1].sprockets = 1;
    g.f[1].straighten = 1;
    g.f[1].confidence_low = 1;
    g.f[1].focus_offset = -800;
    g.f[1].done = 1;
    g.f[1].group = 2;

    n = ft_job_write_buf(&g, buf, sizeof buf);
    CHECK(n == sizeof golden - 1 && strcmp(buf, golden) == 0);
    /* the FILE writer is the same text */
    f = tmpfile();
    CHECK(ft_job_write(&g, f) == 0);
    rewind(f);
    n2 = fread(buf2, 1, sizeof buf2 - 1, f);
    buf2[n2] = 0;
    fclose(f);
    CHECK(n2 == n && strcmp(buf2, golden) == 0);

    /* too small: the length needed, cut text NUL-terminated */
    CHECK(ft_job_write_buf(&g, small, sizeof small) == n);
    CHECK(
        strlen(small) == sizeof small - 1 &&
        memcmp(small, golden, sizeof small - 1) == 0
    );
    CHECK(ft_job_write_buf(&g, NULL, 0) == n);

    /* a bad enum writes nothing */
    b = g;
    b.f[1].type = (ft_frame_type)9;
    CHECK(ft_job_write_buf(&b, buf2, sizeof buf2) == FT_JOB_BAD);
    f = tmpfile();
    CHECK(ft_job_write(&b, f) == -1);
    fclose(f);

    /* parse: exactly n bytes, no NUL needed; byte-stable round trip */
    memcpy(buf2, golden, n);
    buf2[n] = 'X'; /* past the end: never read */
    CHECK(ft_job_parse(&b, buf2, n, &e) == 0);
    CHECK(
        b.nframes == 2 && b.f[1].index == 7 && b.f[1].group == 2 &&
        b.f[0].pass == 1 && b.share == 2450 && b.hf_half_res
    );
    CHECK(
        ft_job_write_buf(&b, buf2, sizeof buf2) == n &&
        strcmp(buf2, golden) == 0
    );
    /* the FILE reader parses the same way */
    f = text(golden);
    CHECK(ft_job_read(&b, f, &e) == 0 && b.f[1].done);
    fclose(f);

    /* parse errors name the line, as the FILE reader did */
    CHECK(
        ft_job_parse(&b, "[strip]\nholder = 24x217\nbogus = 1\n", 34, &e) ==
            -1 &&
        strcmp(e.msg, "line 3: unknown key bogus") == 0
    );
    CHECK(
        ft_job_parse(&b, "", 0, &e) == -1 &&
        strcmp(e.msg, "no [strip] section") == 0
    );
    /* a last line without '\n' is read */
    {
      const char *t = "[strip]\nholder = 24x217\nfilm = slide";
      CHECK(ft_job_parse(&b, t, strlen(t), &e) == 0 && b.film == FT_FILM_SLIDE);
    }
    /* lines of 255 characters or more are refused, 254 is fine */
    {
      static char t[600];
      size_t k;
      strcpy(t, "[strip]\nholder = 24x217\n");
      k = strlen(t);
      memset(t + k, ' ', 254 - 12);
      memcpy(t + k + 254 - 12, "film = slide", 12);
      t[k + 254] = '\n';
      CHECK(ft_job_parse(&b, t, k + 255, &e) == 0);
      memmove(t + k + 1, t + k, 255);
      t[k] = ' ';
      CHECK(
          ft_job_parse(&b, t, k + 256, &e) == -1 &&
          strcmp(e.msg, "line 3: too long") == 0
      );
    }
    /* a NUL inside the text is refused */
    {
      static const char t[] = "[strip]\nholder = 24x217\0junk\n";
      CHECK(
          ft_job_parse(&b, t, sizeof t - 1, &e) == -1 &&
          strcmp(e.msg, "line 2: NUL byte") == 0
      );
    }
  }
  return TEST_DONE();
}

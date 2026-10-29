/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ft_focus.h"
#include "ft_job.h"
#include "test/ftest.h"

static FILE* text(const char* s) {
  FILE* f = tmpfile();
  fputs(s, f);
  rewind(f);
  return f;
}

int main(void) {
  ft_job a, b;
  ft_err e;
  FILE* f;

  memset(&a, 0, sizeof a);
  strcpy(a.holder, "24x217");
  a.film = FT_FILM_SLIDE;
  a.focus_mode = FT_FOCUS_PER_FRAME;
  a.focus_offset_steps = 2400;
  a.frame_count = 2;
  a.frames[0].index = 1;
  a.frames[0].type = FT_FRAME_FULL;
  a.frames[0].is_included = 1;
  a.frames[0].ppi = 3150;
  a.frames[0].along_start_mm = 4.1;
  a.frames[0].across_start_mm = 1.6;
  a.frames[0].length_mm = 36.2;
  a.frames[0].width_mm = 24.4;
  a.frames[0].tilt_degrees = 0.12;
  a.frames[1].index = 2;
  a.frames[1].type = FT_FRAME_PARTIAL;
  a.frames[1].is_included = 0;
  a.frames[1].ppi = 2000;
  a.frames[1].along_start_mm = 42;
  a.frames[1].across_start_mm = 4.1;
  a.frames[1].length_mm = 20;
  a.frames[1].width_mm = 24;
  a.frames[1].has_sprockets = 1;
  a.frames[1].should_straighten = 1;
  a.frames[1].is_confidence_low = 1;
  a.frames[1].focus_offset_steps = -800;
  a.frames[1].is_done = 1;

  /* round trip */
  f = tmpfile();
  CHECK(ft_write_job(&a, f) == 0);
  rewind(f);
  CHECK(ft_read_job(&b, f, &e) == 0);
  fclose(f);
  CHECK(strcmp(b.holder, "24x217") == 0);
  CHECK(b.film == FT_FILM_SLIDE && b.focus_mode == FT_FOCUS_PER_FRAME);
  CHECK(b.focus_offset_steps == 2400 && b.frame_count == 2);
  CHECK(b.frames[0].index == 1 &&
        b.frames[0].type == FT_FRAME_FULL &&
        b.frames[0].is_included);
  CHECK_NEAR(b.frames[0].ppi, 3150, 1e-9);
  CHECK_NEAR(b.frames[0].length_mm, 36.2, 0.006);
  CHECK_NEAR(b.frames[0].tilt_degrees, 0.12, 0.006);
  CHECK(b.frames[1].type == FT_FRAME_PARTIAL && !b.frames[1].is_included);
  CHECK(b.frames[1].has_sprockets &&
        b.frames[1].should_straighten &&
        b.frames[1].is_confidence_low);
  CHECK(b.frames[1].focus_offset_steps == -800 && b.frames[1].is_done);
  CHECK(b.frames[0].source_line > 0 &&
        b.frames[1].source_line > b.frames[0].source_line);
  CHECK(b.frames[0].bin == 0 &&
        ft_get_frame_bin(&b.frames[0]) == 2); /* fastest */

  /* bin: per-frame optics; written, read back, options listed */
  {
    char buf[4096];
    size_t n;
    ft_job c = a;
    c.frames[1].bin = 1;
    f = tmpfile();
    CHECK(ft_write_job(&c, f) == 0);
    rewind(f);
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    CHECK(strstr(buf, "bin = 0") != NULL && strstr(buf, "bin = 1") != NULL);
    /* 3150: bins 2 and 1; 2000: 3, 2 and 1 */
    CHECK(strstr(buf, "# bin options at 3150 ppi") != NULL);
    CHECK(strstr(buf,
                 "# bin options at 2000 ppi (time, light, Dmax~): "
                 "3: ") != NULL);
    rewind(f);
    CHECK(ft_read_job(&b, f, &e) == 0);
    fclose(f);
    CHECK(b.frames[0].bin == 0 &&
          b.frames[1].bin == 1 &&
          ft_get_frame_bin(&b.frames[1]) == 1);
  }
  /* detailed scans: strip keys and the frame's pass, written only for a
   * detail job (older jobs read and write as before) */
  {
    ft_job c = a, d2;
    int k;
    f = tmpfile();
    CHECK(ft_write_job(&c, f) == 0);
    rewind(f);
    {
      char buf[4096];
      size_t n = fread(buf, 1, sizeof buf - 1, f);
      buf[n] = 0;
      CHECK(strstr(buf, "detail") == NULL && strstr(buf, "pass") == NULL);
    }
    fclose(f);
    c.film = FT_FILM_NEGATIVE;
    c.detail = FT_DETAIL_POSITIVE;
    c.detail_format = FT_DETAIL_FORMAT_TIFF;
    c.detail_bw = FT_BW_MONO;
    c.keep_raw = 1;
    c.ring_megabytes = 256;
    c.frames[0].pass = 1;
    c.frames[1].pass = 2;
    f = tmpfile();
    CHECK(ft_write_job(&c, f) == 0);
    rewind(f);
    CHECK(ft_read_job(&d2, f, &e) == 0);
    fclose(f);
    CHECK(d2.detail == FT_DETAIL_POSITIVE &&
          d2.detail_format == FT_DETAIL_FORMAT_TIFF);
    CHECK(
      d2.detail_bw == FT_BW_MONO && d2.keep_raw && d2.ring_megabytes == 256);
    CHECK(d2.frames[0].pass == 1 && d2.frames[1].pass == 2);

    f = text("[strip]\nholder = 24x217\n");
    CHECK(ft_read_job(&d2, f, &e) == 0);
    fclose(f);
    CHECK(d2.detail == FT_DETAIL_NONE &&
          d2.ring_megabytes == FT_RING_MEGABYTES_DEFAULT);
    {
      static const char* bad[] = {
        "detail = both\n",    "detail = none\n",  "detail_fmt = jpg\n",
        "detail_bw = grey\n", "keep_raw = 1\n",   "ring_mb = 4\n",
        "ring_mb = 1025\n",   "ring_mb = 4096\n", NULL};
      for(k = 0; bad[k]; k++) {
        char t2[128];
        snprintf(t2, sizeof t2, "[strip]\nholder = 24x217\n%s", bad[k]);
        f = text(t2);
        CHECK(ft_read_job(&d2, f, &e) == -1);
        fclose(f);
      }
    }
    /* the largest ring a 32-bit process can still allocate */
    f = text("[strip]\nholder = 24x217\nring_mb = 1024\n");
    CHECK(ft_read_job(&d2, f, &e) == 0 && d2.ring_megabytes == 1024);
    fclose(f);
    CHECK((double)FT_RING_MEGABYTES_MAX * 1048576 < 2147483648.0);

    /* frames still to scan in a pass: a resumed job skips the passes
     * it finished (done) and those without an included frame */
    d2 = c;
    d2.frames[1].is_included = 1;
    d2.frames[1].is_done = 0;
    CHECK(ft_count_pass_frames_to_do(&d2, 1) == 1 &&
          ft_count_pass_frames_to_do(&d2, 2) == 1);
    d2.frames[0].is_done = 1;
    CHECK(ft_count_pass_frames_to_do(&d2, 1) == 0 &&
          ft_count_pass_frames_to_do(&d2, 2) == 1);
    d2.frames[1].is_included = 0;
    CHECK(ft_count_pass_frames_to_do(&d2, 2) == 0 &&
          ft_count_pass_frames_to_do(&d2, 3) == 0);
    d2.frames[0].is_done = 0;
    d2.frames[1].is_included = 1;
    d2.frames[1].pass = 1;
    CHECK(ft_count_pass_frames_to_do(&d2, 1) == 2);
    d2.frames[1].is_done = 1;
    CHECK(ft_count_pass_frames_to_do(&d2, 1) == 1); /* one of two left */

    f = text(
      "[strip]\nholder = 24x217\n[frame 1]\nppi = 3150\n"
      "window = 4 4 36 24\npass = -1\n");
    CHECK(ft_read_job(&d2, f, &e) == -1);
    fclose(f);

    /* validation */
    c.has_film_base = 1;
    c.film_base_density[0] = 0.3;
    c.film_base_density[1] = 0.6;
    c.film_base_density[2] = 0.9;
    c.frames[0].bin = 1;
    c.frames[1].is_included = 0;
    c.focus_mode = FT_FOCUS_SHIFTED;
    c.focus_offset_steps = 0;
    CHECK(ft_validate_job(&c, &ft_lens_default, &e) == 0);
    d2 = c;
    d2.film = FT_FILM_SLIDE;
    CHECK(ft_validate_job(&d2, &ft_lens_default, &e) == -1);
    CHECK(strstr(e.message, "negative") != NULL);
    d2 = c;
    d2.detail_format = FT_DETAIL_FORMAT_DNG;
    CHECK(ft_validate_job(&d2, &ft_lens_default, &e) == -1);
    d2 = c;
    d2.detail = FT_DETAIL_NEGATIVE;
    CHECK(ft_validate_job(&d2, &ft_lens_default, &e) == -1); /* bw */
    d2.detail_bw = FT_BW_OFF;
    d2.film = FT_FILM_SLIDE;
    CHECK(ft_validate_job(&d2, &ft_lens_default, &e) == 0);
    d2 = c;
    d2.has_film_base = 0;
    CHECK(ft_validate_job(&d2, &ft_lens_default, &e) == -1);
    d2 = c;
    d2.frames[0].pass = 0;
    CHECK(ft_validate_job(&d2, &ft_lens_default, &e) == -1);
    d2 = c;
    d2.frames[0].bin = 2; /* 3150 x 2 = 6300 optics */
    CHECK(ft_validate_job(&d2, &ft_lens_default, &e) == -1);
    CHECK(strstr(e.message, "bin 1") != NULL);
    d2 = c;
    d2.frames[1].is_included = 1;
    d2.frames[1].pass = 1;
    d2.frames[1].ppi = 6300;
    d2.frames[1].bin = 1;
    d2.frames[1].type = FT_FRAME_FULL;
    CHECK(ft_validate_job(&d2, &ft_lens_default, &e) == -1);
    CHECK(strstr(e.message, "mixes") != NULL);
  }

  /* film_base: written with the strip, read back; absent = not set */
  {
    ft_job c = a;
    c.has_film_base = 1;
    c.film_base_density[0] = 0.2512;
    c.film_base_density[1] = 0.6;
    c.film_base_density[2] = 0.9177;
    f = tmpfile();
    CHECK(ft_write_job(&c, f) == 0);
    rewind(f);
    CHECK(ft_read_job(&b, f, &e) == 0);
    fclose(f);
    CHECK(b.has_film_base);
    CHECK_NEAR(b.film_base_density[0], 0.2512, 1e-9);
    CHECK_NEAR(b.film_base_density[2], 0.9177, 1e-9);
    f = tmpfile();
    CHECK(ft_write_job(&a, f) == 0); /* a has none */
    rewind(f);
    CHECK(ft_read_job(&b, f, &e) == 0);
    fclose(f);
    CHECK(!b.has_film_base);
    f = text("[strip]\nholder = 24x217\nfilm_base = 0.2 0.5\n");
    CHECK(ft_read_job(&b, f, &e) == -1); /* two values */
    fclose(f);
    f = text("[strip]\nholder = 24x217\nfilm_base = 0.2 0.5 5\n");
    CHECK(ft_read_job(&b, f, &e) == -1); /* above 4.5 D */
    fclose(f);
    f = text("[strip]\nholder = 24x217\nfilm_base = 0.2 0.5 0.9 1\n");
    CHECK(ft_read_job(&b, f, &e) == -1); /* four values */
    fclose(f);
    f = text("[strip]\nholder = 24x217\nfilm_base = 0.2 0.5 0.9   # comment\n");
    CHECK(ft_read_job(&b, f, &e) == 0 && b.has_film_base);
    fclose(f);
    f = text("[strip]\nholder = 24x217\nfilm_base = -0.1 0.5 0.9\n");
    CHECK(ft_read_job(&b, f, &e) == -1); /* negative D */
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
    c.share_pixels = 2000;
    c.half_frames_at_full_frame_ppi = 1;
    c.frame_count = 3;
    for(n = 0; n < 3; n++) {
      c.frames[n].index = (int)n + 1;
      c.frames[n].type = FT_FRAME_HALF;
      c.frames[n].is_included = n != 1;
      c.frames[n].ppi = 1500;
      c.frames[n].bin = 1;
      c.frames[n].along_start_mm = 3 + 19.0 * n;
      c.frames[n].across_start_mm = 4;
      c.frames[n].length_mm = 18;
      c.frames[n].width_mm = 24;
      c.frames[n].group = n == 0 ? 1 : n == 2 ? 2 : 0;
    }
    f = tmpfile();
    CHECK(ft_write_job(&c, f) == 0);
    rewind(f);
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    CHECK(strstr(buf, "share = 2000") && strstr(buf, "hf_half_res = yes"));
    CHECK(strstr(buf, "group = 2") != NULL);
    rewind(f);
    CHECK(ft_read_job(&b, f, &e) == 0);
    fclose(f);
    CHECK(b.share_pixels == 2000 && b.half_frames_at_full_frame_ppi == 1);
    CHECK(b.frames[0].group == 1 &&
          b.frames[1].group == 0 &&
          b.frames[2].group == 2);
    CHECK(ft_validate_job(&b, &ft_lens_default, &e) == 0);
    /* an ordinary job writes no share keys and no groups */
    f = tmpfile();
    CHECK(ft_write_job(&a, f) == 0);
    rewind(f);
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    CHECK(!strstr(buf, "share") && !strstr(buf, "group"));
    /* share rules */
    b.frames[2].group = 0; /* included, no group */
    CHECK(ft_validate_job(&b, &ft_lens_default, &e) == -1);
    CHECK(strstr(e.message, "without a group") != NULL);
    b.frames[2].group = 2;
    b.frames[2].bin = 2; /* own group: fine */
    CHECK(ft_validate_job(&b, &ft_lens_default, &e) == 0);
    b.frames[2].group = 1; /* same group, other bin */
    CHECK(ft_validate_job(&b, &ft_lens_default, &e) == -1);
    CHECK(strstr(e.message, "differ") != NULL);
    b.frames[2].bin = 1;
    CHECK(ft_validate_job(&b, &ft_lens_default, &e) == 0);
    b.frames[1].is_included = 1; /* 1, 2, 1: split */
    b.frames[1].group = 2;
    CHECK(ft_validate_job(&b, &ft_lens_default, &e) == -1);
    CHECK(strstr(e.message, "split") != NULL);
    b.frames[1].is_included = 0;
    b.frames[0].along_start_mm = 50; /* out of strip order */
    CHECK(ft_validate_job(&b, &ft_lens_default, &e) == -1);
    CHECK(strstr(e.message, "follow the strip") != NULL);
    b.frames[0].along_start_mm = 3;
    b.share_pixels = 0; /* groups without share */
    CHECK(ft_validate_job(&b, &ft_lens_default, &e) == -1);
    CHECK(strstr(e.message, "group without share") != NULL);
    /* reader limits */
    f = text("[strip]\nholder = 24x217\nshare = 50\n");
    CHECK(ft_read_job(&b, f, &e) == -1);
    fclose(f);
    f = text("[strip]\nholder = 24x217\nhf_half_res = maybe\n");
    CHECK(ft_read_job(&b, f, &e) == -1);
    fclose(f);
    f = text(
      "[strip]\nholder = 24x217\n[frame 1]\nppi = 1500\n"
      "window = 50 4 36 24\ngroup = -1\n");
    CHECK(ft_read_job(&b, f, &e) == -1);
    fclose(f);

    /* share_px: a frame's own target (the web page's 4MP / 6MP);
     * written only when set, one target per group */
    b = c;
    CHECK(ft_get_frame_share_pixels(&b, &b.frames[0]) == 2000);
    b.frames[2].share_pixels = 3000;
    CHECK(ft_get_frame_share_pixels(&b, &b.frames[2]) == 3000);
    n = ft_format_job(&b, buf, sizeof buf);
    CHECK(n < sizeof buf);
    CHECK(strstr(buf,
                 "group = 2   # window: frames of one group scan "
                 "together\nshare_px = 3000   # this frame's "
                 "target, px on its long side\n") != NULL);
    CHECK(strstr(strstr(buf, "share_px") + 1, "share_px") == NULL);
    {
      ft_job d;
      CHECK(ft_parse_job(&d, buf, n, &e) == 0);
      CHECK(d.frames[0].share_pixels == 0 && d.frames[2].share_pixels == 3000);
      CHECK(ft_validate_job(&d, &ft_lens_default, &e) == 0);
      d.frames[0].group = 2; /* one window, two targets */
      CHECK(ft_validate_job(&d, &ft_lens_default, &e) == -1);
      CHECK(strstr(e.message,
                   "(frame 3): frames of group 2 differ in "
                   "share_px") != NULL);
      d.frames[0].share_pixels = 3000; /* same target: fine */
      CHECK(ft_validate_job(&d, &ft_lens_default, &e) == 0);
      d.share_pixels = 0; /* share_px needs share */
      d.frames[0].group = d.frames[2].group = 0;
      CHECK(ft_validate_job(&d, &ft_lens_default, &e) == -1);
      CHECK(strstr(e.message,
                   "(frame 1): share_px without share in "
                   "[strip]") != NULL);
    }
    f = text(
      "[strip]\nholder = 24x217\nshare = 2000\n[frame 1]\n"
      "ppi = 1500\nwindow = 50 4 36 24\nshare_px = 50\n");
    CHECK(ft_read_job(&b, f, &e) == -1 &&
          strcmp(e.message, "line 7: bad value for share_px: 50") == 0);
    fclose(f);
  }
  /* a job without bin keys (older files) scans with the fastest bin */
  f = text(
    "[strip]\nholder = 24x217\n[frame 1]\nppi = 2000\n"
    "window = 50 4 36 24\n");
  CHECK(ft_read_job(&b, f, &e) == 0);
  fclose(f);
  CHECK(b.frames[0].bin == 0 && ft_get_frame_bin(&b.frames[0]) == 3);
  CHECK(ft_validate_job(&b, &ft_lens_default, &e) == 0);
  b.frames[0].bin = 1; /* optics 2000: usable */
  CHECK(ft_validate_job(&b, &ft_lens_default, &e) == 0);
  b.frames[0].bin = 6; /* optics 12000: no */
  CHECK(ft_validate_job(&b, &ft_lens_default, &e) == -1);
  CHECK(strstr(e.message, "bin 6") != NULL);
  f = text(
    "[strip]\nholder = 24x217\n[frame 1]\nppi = 2000\nbin = 13\n"
    "window = 50 4 36 24\n");
  CHECK(ft_read_job(&b, f, &e) == -1);
  fclose(f);

  /* parse errors carry the line number */
  f = text("[strip]\nholder = 24x217\nbogus = 1\n");
  CHECK(ft_read_job(&b, f, &e) == -1);
  CHECK(strstr(e.message, "line 3") != NULL);
  fclose(f);
  f = text("[strip]\n[frame 1]\nppi = fast\n");
  CHECK(ft_read_job(&b, f, &e) == -1);
  CHECK(strstr(e.message, "line 3") != NULL);
  fclose(f);
  f = text("ppi = 2000\n");
  CHECK(ft_read_job(&b, f, &e) == -1);
  fclose(f);

  /* validation: holder, ppi, window, optics */
  CHECK(ft_validate_job(&a, &ft_lens_default, &e) == 0);
  b = a;
  b.frames[0].ppi = 6400;
  CHECK(ft_validate_job(&b, &ft_lens_default, &e) == -1);
  CHECK(strstr(e.message, "frame 1") != NULL);
  b = a;
  b.frames[0].along_start_mm = 200; /* runs past 231 mm */
  CHECK(ft_validate_job(&b, &ft_lens_default, &e) == -1);
  b = a;
  b.frames[1].ppi = 6400; /* excluded frames are skipped */
  CHECK(ft_validate_job(&b, &ft_lens_default, &e) == 0);
  b = a;
  strcpy(b.holder, "60x180");
  CHECK(ft_validate_job(&b, &ft_lens_default, &e) == -1);
  b = a;
  b.focus_mode = FT_FOCUS_SHIFTED;
  b.focus_offset_steps = 900000; /* CCD/lens past their range */
  CHECK(ft_validate_job(&b, &ft_lens_default, &e) == -1);
  /* hardening */
  f = text(
    "[strip]\r\nholder = 24x217\r\n[frame 1]\r\nppi = 2000\r\n"
    "window = 1 2 3 4\r\n");
  CHECK(ft_read_job(&b, f, &e) == 0 && b.frame_count == 1);
  fclose(f);
  {
    char big[400] = "[strip]\nholder = 24x217\n# ";
    memset(big + 26, 'x', 300);
    strcpy(big + 326, "\n");
    f = text(big);
    CHECK(ft_read_job(&b, f, &e) == -1);
    CHECK(strstr(e.message, "line 3") && strstr(e.message, "too long"));
    fclose(f);
  }
#define GOOD "[strip]\nholder = 24x217\n"
#define FR "[frame 1]\nppi = 2000\nwindow = 1 2 3 4\n"
  f = text(GOOD FR "[frame 1]\nppi = 2000\nwindow = 1 2 3 4\n");
  CHECK(ft_read_job(&b, f, &e) == -1 && strstr(e.message, "repeated"));
  fclose(f);
  f = text(GOOD "[frame 1 junk]\nppi = 2000\nwindow = 1 2 3 4\n");
  CHECK(ft_read_job(&b, f, &e) == -1);
  fclose(f);
  f = text(GOOD "[frame 0]\nppi = 2000\nwindow = 1 2 3 4\n");
  CHECK(ft_read_job(&b, f, &e) == -1);
  fclose(f);
  f = text(GOOD "[frame 1]\nppi = 2000\nwindow = nan 2 3 4\n");
  CHECK(ft_read_job(&b, f, &e) == -1 && strstr(e.message, "line 5"));
  fclose(f);
  f = text(GOOD "[frame 1]\nppi = inf\nwindow = 1 2 3 4\n");
  CHECK(ft_read_job(&b, f, &e) == -1);
  fclose(f);
  f = text(GOOD "[frame 1]\nppi = 2000\nwindow = 1 2 3 4 5\n");
  CHECK(ft_read_job(&b, f, &e) == -1);
  fclose(f);
  f = text(GOOD "[frame 1]\nppi = 2000\n");
  CHECK(ft_read_job(&b, f, &e) == -1 && strstr(e.message, "no window"));
  fclose(f);
  f = text(GOOD "[frame 1]\nwindow = 1 2 3 4\n");
  CHECK(ft_read_job(&b, f, &e) == -1 && strstr(e.message, "no ppi"));
  fclose(f);
  f = text("[strip]\n" FR);
  CHECK(ft_read_job(&b, f, &e) == -1 && strstr(e.message, "no holder"));
  fclose(f);
  f = text(FR);
  CHECK(ft_read_job(&b, f, &e) == -1);
  fclose(f);
  f = text("");
  CHECK(ft_read_job(&b, f, &e) == -1 && strstr(e.message, "[strip]"));
  fclose(f);
  f = text(GOOD "[strip]\n");
  CHECK(ft_read_job(&b, f, &e) == -1 && strstr(e.message, "line 3"));
  fclose(f);
  f = text(GOOD "film = slide\nfilm = slide\n");
  CHECK(ft_read_job(&b, f, &e) == -1 && strstr(e.message, "duplicate key"));
  fclose(f);
  f = text(GOOD "focus_offset = 99999999999999999999\n");
  CHECK(ft_read_job(&b, f, &e) == -1);
  fclose(f);
  f = text(GOOD "film = slide  # comment\n" FR);
  CHECK(ft_read_job(&b, f, &e) == 0 && b.film == FT_FILM_SLIDE);
  fclose(f);

  /* validation hardening */
  b = a;
  b.focus_mode = FT_FOCUS_PER_FRAME;
  b.frames[0].focus_offset_steps = 900000;
  CHECK(ft_validate_job(&b, &ft_lens_default, &e) == -1);
  b = a;
  b.frames[0].focus_offset_steps = -2000000000L;
  CHECK(ft_validate_job(&b, &ft_lens_default, &e) == -1);
  b = a;
  b.frames[0].tilt_degrees = 60;
  CHECK(ft_validate_job(&b, &ft_lens_default, &e) == -1);
  b = a;
  b.frames[0].length_mm = NAN;
  CHECK(ft_validate_job(&b, &ft_lens_default, &e) == -1);
  b = a;
  b.frame_count = 0;
  CHECK(ft_validate_job(&b, &ft_lens_default, &e) == -1 &&
        strstr(e.message, "no frames"));

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
    g.focus_mode = FT_FOCUS_SHIFTED;
    g.focus_offset_steps = -1200;
    g.has_film_base = 1;
    g.film_base_density[0] = 0.2512;
    g.film_base_density[1] = 0.6;
    g.film_base_density[2] = 0.91234;
    g.detail = FT_DETAIL_NEGATIVE;
    g.detail_format = FT_DETAIL_FORMAT_TIFF;
    g.ring_megabytes = 64;
    g.keep_raw = 1;
    g.share_pixels = 2450;
    g.half_frames_at_full_frame_ppi = 1;
    g.frame_count = 2;
    g.frames[0].index = 1;
    g.frames[0].type = FT_FRAME_FULL;
    g.frames[0].is_included = 1;
    g.frames[0].ppi = 1729.5;
    g.frames[0].bin = 2;
    g.frames[0].along_start_mm = 4.1;
    g.frames[0].across_start_mm = 1.6;
    g.frames[0].length_mm = 36.2;
    g.frames[0].width_mm = 24.4;
    g.frames[0].tilt_degrees = 0.12;
    g.frames[0].pass = 1;
    g.frames[0].group = 1;
    g.frames[1].index = 7;
    g.frames[1].type = FT_FRAME_HALF;
    g.frames[1].is_included = 0;
    g.frames[1].ppi = 6300;
    g.frames[1].along_start_mm = 42;
    g.frames[1].across_start_mm = 4.1;
    g.frames[1].length_mm = 18;
    g.frames[1].width_mm = 24;
    g.frames[1].has_sprockets = 1;
    g.frames[1].should_straighten = 1;
    g.frames[1].is_confidence_low = 1;
    g.frames[1].focus_offset_steps = -800;
    g.frames[1].is_done = 1;
    g.frames[1].group = 2;

    n = ft_format_job(&g, buf, sizeof buf);
    CHECK(n == sizeof golden - 1 && strcmp(buf, golden) == 0);
    /* the FILE writer is the same text */
    f = tmpfile();
    CHECK(ft_write_job(&g, f) == 0);
    rewind(f);
    n2 = fread(buf2, 1, sizeof buf2 - 1, f);
    buf2[n2] = 0;
    fclose(f);
    CHECK(n2 == n && strcmp(buf2, golden) == 0);

    /* too small: the length needed, cut text NUL-terminated */
    CHECK(ft_format_job(&g, small, sizeof small) == n);
    CHECK(strlen(small) == sizeof small - 1 &&
          memcmp(small, golden, sizeof small - 1) == 0);
    CHECK(ft_format_job(&g, NULL, 0) == n);

    /* a bad enum writes nothing */
    b = g;
    b.frames[1].type = (ft_frame_type)9;
    CHECK(ft_format_job(&b, buf2, sizeof buf2) == FT_JOB_BAD);
    f = tmpfile();
    CHECK(ft_write_job(&b, f) == -1);
    fclose(f);

    /* parse: exactly n bytes, no NUL needed; byte-stable round trip */
    memcpy(buf2, golden, n);
    buf2[n] = 'X'; /* past the end: never read */
    CHECK(ft_parse_job(&b, buf2, n, &e) == 0);
    CHECK(b.frame_count == 2 &&
          b.frames[1].index == 7 &&
          b.frames[1].group == 2 &&
          b.frames[0].pass == 1 &&
          b.share_pixels == 2450 &&
          b.half_frames_at_full_frame_ppi);
    CHECK(ft_format_job(&b, buf2, sizeof buf2) == n &&
          strcmp(buf2, golden) == 0);
    /* the FILE reader parses the same way */
    f = text(golden);
    CHECK(ft_read_job(&b, f, &e) == 0 && b.frames[1].is_done);
    fclose(f);

    /* parse errors name the line, as the FILE reader did */
    CHECK(ft_parse_job(&b, "[strip]\nholder = 24x217\nbogus = 1\n", 34, &e) ==
            -1 &&
          strcmp(e.message, "line 3: unknown key bogus") == 0);
    CHECK(ft_parse_job(&b, "", 0, &e) == -1 &&
          strcmp(e.message, "no [strip] section") == 0);
    /* a last line without '\n' is read */
    {
      const char* t = "[strip]\nholder = 24x217\nfilm = slide";
      CHECK(ft_parse_job(&b, t, strlen(t), &e) == 0 && b.film == FT_FILM_SLIDE);
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
      CHECK(ft_parse_job(&b, t, k + 255, &e) == 0);
      memmove(t + k + 1, t + k, 255);
      t[k] = ' ';
      CHECK(ft_parse_job(&b, t, k + 256, &e) == -1 &&
            strcmp(e.message, "line 3: too long") == 0);
    }
    /* a NUL inside the text is refused */
    {
      static const char t[] = "[strip]\nholder = 24x217\0junk\n";
      CHECK(ft_parse_job(&b, t, sizeof t - 1, &e) == -1 &&
            strcmp(e.message, "line 2: NUL byte") == 0);
    }
  }
  return TEST_DONE();
}

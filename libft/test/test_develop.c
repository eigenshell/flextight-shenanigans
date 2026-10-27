/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_develop.h"
#include "ft_dng.h"
#include "ft_geom.h"
#include "ft_out.h"
#include "test/ftest.h"

static unsigned rd16(const unsigned char* p) { return p[0] | p[1] << 8; }
static unsigned long rd32(const unsigned char* p) {
  return rd16(p) | (unsigned long)rd16(p + 2) << 16;
}

/* the IFD entry for tag, or NULL */
static const unsigned char* find_tag(const unsigned char* f, unsigned tag) {
  const unsigned char* ifd = f + rd32(f + 4);
  unsigned n = rd16(ifd), i;
  for(i = 0; i < n; i++)
    if(rd16(ifd + 2 + 12 * i) == tag) return ifd + 2 + 12 * i;
  return NULL;
}

int main(void) {
  ft_err e;

  /* defect map: pixel, optional channels, comments */
  {
    static const char txt[] =
      "# stuck column\n3021\n 400 g # green\n"
      "\n500 rb\n";
    ft_defects d;
    CHECK(ft_defects_parse(txt, sizeof txt - 1, &d, &e) == 0);
    CHECK(d.n == 3);
    CHECK(d.ccd[0] == 3021 && d.ch[0] == 7);
    CHECK(d.ccd[1] == 400 && d.ch[1] == 2);
    CHECK(d.ccd[2] == 500 && d.ch[2] == 5);
    CHECK(ft_defects_parse("12 x\n", 5, &d, &e) == -1);
    CHECK(ft_defects_parse("abc\n", 4, &d, &e) == -1);
  }

  /* crop + orient: image row = CCD pixel, column = scan line */
  {
    enum { L = 5, P = 4 };
    float t[L * P * 3];
    ft_develop_opts o;
    ft_image im;
    int l, c;
    for(l = 0; l < L; l++)
      for(c = 0; c < P; c++) {
        t[(l * P + c) * 3 + 0] = (float)(l * 10 + c);
        t[(l * P + c) * 3 + 1] = 0.5f;
        t[(l * P + c) * 3 + 2] = -1;
      }
    memset(&o, 0, sizeof o);
    ft_image_init(&im);
    CHECK(ft_crop_orient(t, L, P, &o, &im, &e) == 0);
    CHECK(im.w == L && im.h == P);
    CHECK(im.rgb[(2 * L + 3) * 3] == 32); /* row 2 = px 2, col 3 */
    ft_image_free(&im);
    o.crop_x = 1;
    o.crop_y = 2;
    o.crop_w = 2;
    o.crop_h = 3;
    CHECK(ft_crop_orient(t, L, P, &o, &im, &e) == 0);
    CHECK(im.w == 3 && im.h == 2);
    CHECK(im.rgb[0] == 21);               /* line 2, px 1 */
    CHECK(im.rgb[(1 * 3 + 2) * 3] == 42); /* line 4, px 2 */
    ft_image_free(&im);
    o.crop_w = 4; /* 1 + 4 > 4 px */
    CHECK(ft_crop_orient(t, L, P, &o, &im, &e) == -1);
  }

  /* develop end to end: flat-field, a user defect, alignment, crop */
  {
    enum { P = 6, L = 20, CL = 250, BIN = 6, K = 12 / BIN };
    static unsigned short raw[L * P * 3], cal[CL * P * 3];
    ft_develop_opts o;
    ft_defects d;
    ft_image im;
    int l, i, c;
    for(l = 0; l < CL; l++)
      for(i = 0; i < P * 3; i++)
        cal[l * P * 3 + i] = (unsigned short)(l < 200 ? 300 : 4300);
    for(l = 0; l < L; l++)
      for(c = 0; c < P; c++)
        for(i = 0; i < 3; i++) /* T = 0.25, px 3 stuck white */
          raw[(l * P + c) * 3 + i] = c == 3 ? 4300 : 1300;
    memset(&o, 0, sizeof o);
    o.x = 10;
    o.bin = BIN;
    /* output px 3 covers CCD (10 + 3) * 6 + 144 = 222 .. 227 */
    d.n = 1;
    d.ccd[0] = 224;
    d.ch[0] = 7;
    o.defects = &d;
    ft_image_init(&im);
    CHECK(ft_develop(raw, L, P, cal, CL, NULL, 0, &o, &im, &e) == 0);
    CHECK(im.w == L - 2 * K && im.h == P);
    CHECK_NEAR(im.rgb[(3 * im.w + 5) * 3 + 1], 0.25, 1e-6);
    CHECK_NEAR(im.rgb[(0 * im.w + 0) * 3 + 2], 0.25, 1e-6);

    /* PNG and DNG through the writer table */
    CHECK(ft_writer_find("dng") != NULL);
    CHECK(ft_writer_find("png") != NULL);
    CHECK(ft_writer_find("tiff") == NULL);
    CHECK(ft_writer_find("png")->write("test_develop.png", &im, &e) == 0);
    im.ppi = 2000;
    CHECK(ft_writer_find("dng")->write("test_develop.dng", &im, &e) == 0);
    {
      FILE* f = fopen("test_develop.dng", "rb");
      unsigned char* b = malloc(1 << 16);
      const unsigned char* t;
      size_t n;
      unsigned prev = 0, k, ntags;
      CHECK(f && b);
      n = fread(b, 1, 1 << 16, f);
      fclose(f);
      CHECK(n > 8 && memcmp(b, "II*\0", 4) == 0);
      ntags = rd16(b + rd32(b + 4));
      for(k = 0; k < ntags; k++) { /* ascending tags */
        unsigned tag = rd16(b + rd32(b + 4) + 2 + 12 * k);
        CHECK(tag > prev);
        prev = tag;
      }
      CHECK((t = find_tag(b, 262)) && rd16(t + 8) == 34892);
      CHECK((t = find_tag(b, 256)) && rd32(t + 8) == (unsigned long)im.w);
      CHECK((t = find_tag(b, 257)) && rd32(t + 8) == (unsigned long)im.h);
      CHECK((t = find_tag(b, 50706)) && t[8] == 1 && t[9] == 4);
      CHECK((t = find_tag(b, 50721)) && rd32(t + 4) == 9);
      /* ColorMatrix1[1] = -1.5371385, signed */
      CHECK((long)(int)rd32(b + rd32(t + 8) + 8) == -15371385L);
      CHECK((t = find_tag(b, 282)) && rd32(b + rd32(t + 8)) == 2000000UL);
      CHECK((t = find_tag(b, 273)) != NULL);
      {
        const unsigned char* px = b + rd32(t + 8); /* one strip */
        CHECK(rd16(px) == 16384);                  /* 0.25 * 65535 + 0.5 */
        CHECK(rd16(px + 6 * (3 * im.w + 5) + 2) == 16384);
      }
      free(b);
    }
    remove("test_develop.png");
    remove("test_develop.dng");
    ft_image_free(&im);
  }
  return TEST_DONE();
}

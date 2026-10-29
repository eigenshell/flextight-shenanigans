/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The streaming tiled writer read back by libtiff (tests only), an
 * independent reader: tags, tile layout, padding, DNG crop, ICC. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tiffio.h>

#include "ft_tiff.h"
#include "test/ftest.h"

/* the test pattern: column c, row r, sample s */
static unsigned short val(int c, int r, int s) {
  return (unsigned short)((c * 7 + r * 13 + s * 1000) & 0xffff);
}

static void write_file(const char* path, ft_tiff_kind kind, int w, int h,
                       const unsigned char* icc, unsigned long icc_n,
                       int crop) {
  ft_tiff_options o;
  ft_image meta;
  ft_tiff_writer* tw;
  ft_err e;
  unsigned short* col;
  int spp = kind == FT_TIFF_GRAY ? 1 : 3, c, r, s;

  ft_init_image(&meta);
  meta.ppi = 3150;
  strcpy(meta.description, "test frame");
  memset(&o, 0, sizeof o);
  o.kind = kind;
  o.width = w;
  o.height = h;
  o.metadata = &meta;
  o.icc_profile = icc;
  o.icc_profile_length = icc_n;
  tw = ft_open_tiff_writer(path, &o, &e);
  CHECK(tw != NULL);
  col = malloc((size_t)h * spp * sizeof *col);
  CHECK(col != NULL);
  for(c = 0; c < w; c++) {
    for(r = 0; r < h; r++)
      for(s = 0; s < spp; s++) col[r * spp + s] = val(c, r, s);
    CHECK(ft_write_tiff_column(tw, col, &e) == 0);
  }
  if(crop) ft_set_tiff_crop(tw, 10, 20, 200, 300);
  CHECK(ft_close_tiff_writer(tw, &e) == 0);
  free(col);
}

/* every pixel through libtiff's tile reader */
static void check_pixels(const char* path, int w, int h, int spp) {
  TIFF* t = TIFFOpen(path, "r");
  uint32_t tw = 0, th = 0, iw = 0, ih = 0;
  uint16_t bps = 0, sp = 0;
  unsigned short* buf;
  int x, y, r, c, s, bad = 0;

  CHECK(t != NULL);
  CHECK(TIFFIsTiled(t));
  CHECK(TIFFGetField(t, TIFFTAG_IMAGEWIDTH, &iw) && iw == (uint32_t)w);
  CHECK(TIFFGetField(t, TIFFTAG_IMAGELENGTH, &ih) && ih == (uint32_t)h);
  CHECK(TIFFGetField(t, TIFFTAG_TILEWIDTH, &tw) && tw == FT_TILE_PIXELS);
  CHECK(TIFFGetField(t, TIFFTAG_TILELENGTH, &th) && th == FT_TILE_PIXELS);
  CHECK(TIFFGetField(t, TIFFTAG_BITSPERSAMPLE, &bps) && bps == 16);
  CHECK(TIFFGetField(t, TIFFTAG_SAMPLESPERPIXEL, &sp) && sp == spp);
  buf = malloc(TIFFTileSize(t));
  CHECK(buf != NULL);
  for(y = 0; y < h; y += FT_TILE_PIXELS)
    for(x = 0; x < w; x += FT_TILE_PIXELS) {
      CHECK(TIFFReadTile(t, buf, (uint32_t)x, (uint32_t)y, 0, 0) > 0);
      for(r = 0; r < FT_TILE_PIXELS; r++)
        for(c = 0; c < FT_TILE_PIXELS; c++)
          for(s = 0; s < spp; s++) {
            unsigned short v = buf[(r * FT_TILE_PIXELS + c) * spp + s];
            int inside = y + r < h && x + c < w;
            if(v != (inside ? val(x + c, y + r, s) : 0)) bad++;
          }
    }
  CHECK(bad == 0);
  free(buf);
  TIFFClose(t);
}

int main(void) {
  static const unsigned char icc[7] = {'n', 'o', 't', ' ', 'i', 'c', 'c'};
  ft_err e;

  TIFFSetWarningHandler(NULL);

  /* sizes: 300 x 520 = 2 x 3 tiles, the last row and column partial */
  CHECK(ft_estimate_tiff_bytes(FT_TIFF_RGB, 300, 520) == 6.0 * 256 * 256 * 6);
  CHECK(ft_estimate_tiff_bytes(FT_TIFF_GRAY, 256, 256) == 256.0 * 256 * 2);

  /* DNG: LinearRaw, the crop patched at close */
  write_file("test_tiff.dng", FT_TIFF_DNG, 300, 520, NULL, 0, 1);
  check_pixels("test_tiff.dng", 300, 520, 3);
  {
    TIFF* t = TIFFOpen("test_tiff.dng", "r");
    uint16_t ph = 0;
    CHECK(t != NULL);
    CHECK(TIFFGetField(t, TIFFTAG_PHOTOMETRIC, &ph) && ph == 34892);
    TIFFClose(t);
  }
  { /* the crop tags straight from the file: LONG x 2 each */
    FILE* f = fopen("test_tiff.dng", "rb");
    unsigned char b[4096];
    size_t got;
    unsigned ifd, n, i, found = 0;
    CHECK(f != NULL);
    got = fread(b, 1, sizeof b, f);
    fclose(f);
    CHECK(got == sizeof b);
    ifd = b[4] | b[5] << 8;
    n = b[ifd] | b[ifd + 1] << 8;
    for(i = 0; i < n; i++) {
      const unsigned char* x = b + ifd + 2 + 12 * i;
      unsigned tag = x[0] | x[1] << 8;
      unsigned off = x[8] | x[9] << 8 | x[10] << 16;
      if(tag == 50719) {
        CHECK((x[2] | x[3] << 8) == 4 && x[4] == 2);
        CHECK(b[off] == 10 && b[off + 4] == 20);
        found++;
      }
      if(tag == 50720) {
        CHECK(b[off] == 200 && (b[off + 4] | b[off + 5] << 8) == 300);
        found++;
      }
      if(tag == 273 || tag == 278 || tag == 279)
        found += 100; /* never strips */
    }
    CHECK(found == 2);
  }

  /* RGB with an ICC profile (bytes carried as they are) */
  write_file("test_tiff_rgb.tif", FT_TIFF_RGB, 257, 256, icc, sizeof icc, 0);
  check_pixels("test_tiff_rgb.tif", 257, 256, 3);
  {
    TIFF* t = TIFFOpen("test_tiff_rgb.tif", "r");
    uint16_t ph = 0;
    uint32_t n = 0;
    void* p = NULL;
    CHECK(t != NULL);
    CHECK(TIFFGetField(t, TIFFTAG_PHOTOMETRIC, &ph) && ph == PHOTOMETRIC_RGB);
    CHECK(TIFFGetField(t, TIFFTAG_ICCPROFILE, &n, &p) &&
          n == sizeof icc &&
          memcmp(p, icc, sizeof icc) == 0);
    TIFFClose(t);
  }

  /* gray: one sample */
  write_file("test_tiff_gray.tif", FT_TIFF_GRAY, 40, 600, NULL, 0, 0);
  check_pixels("test_tiff_gray.tif", 40, 600, 1);
  {
    TIFF* t = TIFFOpen("test_tiff_gray.tif", "r");
    uint16_t ph = 9;
    CHECK(t != NULL);
    CHECK(TIFFGetField(t, TIFFTAG_PHOTOMETRIC, &ph) &&
          ph == PHOTOMETRIC_MINISBLACK);
    TIFFClose(t);
  }

  /* refusals: missing and extra columns, bad sizes, unwritable path */
  {
    ft_tiff_options o;
    ft_image meta;
    ft_tiff_writer* tw;
    unsigned short col[3 * 10] = {0};
    ft_init_image(&meta);
    memset(&o, 0, sizeof o);
    o.kind = FT_TIFF_RGB;
    o.width = 2;
    o.height = 10;
    o.metadata = &meta;
    CHECK((tw = ft_open_tiff_writer("test_tiff_short.tif", &o, &e)) != NULL);
    CHECK(ft_write_tiff_column(tw, col, &e) == 0);
    CHECK(ft_close_tiff_writer(tw, &e) == -1); /* 1 of 2 */
    CHECK(strstr(e.message, "1 of 2") != NULL);
    CHECK((tw = ft_open_tiff_writer("test_tiff_short.tif", &o, &e)) != NULL);
    CHECK(ft_write_tiff_column(tw, col, &e) == 0);
    CHECK(ft_write_tiff_column(tw, col, &e) == 0);
    CHECK(ft_write_tiff_column(tw, col, &e) == -1); /* a third */
    ft_abandon_tiff_writer(tw);
    o.width = 0;
    CHECK(ft_open_tiff_writer("test_tiff_short.tif", &o, &e) == NULL);
    o.width = 40000;
    o.height = 40000; /* 9.6 GB */
    CHECK(ft_open_tiff_writer("test_tiff_short.tif", &o, &e) == NULL);
    CHECK(strstr(e.message, "too large") != NULL);
    o.width = o.height = 4;
    CHECK(ft_open_tiff_writer("no/such/dir/x.tif", &o, &e) == NULL);
  }
  remove("test_tiff.dng");
  remove("test_tiff_rgb.tif");
  remove("test_tiff_gray.tif");
  remove("test_tiff_short.tif");
  return TEST_DONE();
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ft_jpeg_encode against libjpeg-turbo: with ftdev's settings (defaults,
 * jpeg_set_quality(q, TRUE), optimised Huffman tables, 4:4:4, the
 * integer DCT) both write the same bytes. libjpeg is the oracle here
 * only; libft never links it. */
#include <jpeglib.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_icc.h"
#include "ft_jpeg.h"
#include "test/ftest.h"

/* libjpeg-turbo with the settings the old ft_jpeg_write used */
static unsigned char* ref_encode(const unsigned char* rgb, int w, int h,
                                 int quality, double ppi,
                                 const unsigned char* icc, size_t icc_n,
                                 const char* comment, unsigned long* n) {
  struct jpeg_compress_struct c;
  struct jpeg_error_mgr em;
  unsigned char* out = NULL;
  c.err = jpeg_std_error(&em);
  jpeg_create_compress(&c);
  *n = 0;
  jpeg_mem_dest(&c, &out, n);
  c.image_width = (JDIMENSION)w;
  c.image_height = (JDIMENSION)h;
  c.input_components = 3;
  c.in_color_space = JCS_RGB;
  jpeg_set_defaults(&c);
  jpeg_set_quality(&c, quality, TRUE);
  c.optimize_coding = TRUE;
  c.comp_info[0].h_samp_factor = c.comp_info[0].v_samp_factor = 1;
  c.comp_info[1].h_samp_factor = c.comp_info[1].v_samp_factor = 1;
  c.comp_info[2].h_samp_factor = c.comp_info[2].v_samp_factor = 1;
  if (ppi > 0) {
    long d = (long)(ppi + 0.5);
    c.density_unit = 1;
    c.X_density = c.Y_density = (UINT16)(d < 1 ? 1 : d > 65535 ? 65535 : d);
  }
  jpeg_start_compress(&c, TRUE);
  if (icc && icc_n) jpeg_write_icc_profile(&c, icc, (unsigned int)icc_n);
  if (comment && *comment)
    jpeg_write_marker(&c, JPEG_COM, (const JOCTET*)comment,
                      (unsigned int)strlen(comment));
  while (c.next_scanline < c.image_height) {
    JSAMPROW row = (JSAMPROW)(rgb + (size_t)c.next_scanline * w * 3);
    jpeg_write_scanlines(&c, &row, 1);
  }
  jpeg_finish_compress(&c);
  jpeg_destroy_compress(&c);
  return out;
}

static unsigned long lcg = 12345;
static int rnd(int n) {
  lcg = lcg * 1103515245UL + 12345UL;
  return (int)((lcg >> 16) % (unsigned long)n);
}

/* kinds: 0 ramps, 1 noise, 2 extremes (0/255 blocks), 3 flat */
static void fill(unsigned char* rgb, int w, int h, int kind) {
  int x, y, c;
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      for (c = 0; c < 3; c++) {
        unsigned char* p = rgb + ((size_t)y * w + x) * 3 + c;
        if (kind == 0)
          *p = (unsigned char)((x * (c + 1) * 7 + y * 3) & 255);
        else if (kind == 1)
          *p = (unsigned char)rnd(256);
        else if (kind == 2)
          *p = (((x / 3 + y / 5 + c) & 1) ? 255 : 0);
        else
          *p = (unsigned char)(90 + 40 * c);
      }
}

static int same(const unsigned char* rgb, int w, int h, int q, double ppi,
                const unsigned char* icc, size_t icc_n, const char* com) {
  unsigned long rn;
  unsigned char* ref = ref_encode(rgb, w, h, q, ppi, icc, icc_n, com, &rn);
  unsigned char* mine = NULL;
  size_t mn = 0;
  ft_err e;
  int ok;
  if (ft_jpeg_encode(rgb, w, h, q, ppi, icc, icc_n, com, &mine, &mn, &e)) {
    fprintf(stderr, "ft_jpeg_encode: %s\n", e.msg);
    free(ref);
    return 0;
  }
  ok = mn == rn && memcmp(mine, ref, rn) == 0;
  if (!ok) {
    size_t i = 0;
    while (i < mn && i < rn && mine[i] == ref[i]) i++;
    fprintf(stderr, "%dx%d q%d: %zu vs %lu bytes, first diff at %zu\n", w, h, q,
            mn, rn, i);
  }
  free(ref);
  free(mine);
  return ok;
}

int main(void) {
  static const int SZ[][2] = {
    {1,   1  },
    {8,   8  },
    {7,   9  },
    {17,  33 },
    {64,  48 },
    {257, 131}
  };
  static const int Q[] = {1, 10, 50, 75, 90, 95, 100};
  unsigned char icc[FT_ICC_MAX], *big, *rgb;
  size_t icc_n = ft_icc_build(FT_SPACE_SRGB, icc, sizeof icc);
  size_t s, q;
  int kind;
  ft_err e;
  unsigned char* out = NULL;
  size_t on = 0;

  CHECK(icc_n > 0);
  rgb = malloc(320 * 200 * 3);
  CHECK(rgb != NULL);
  for (s = 0; s < sizeof SZ / sizeof SZ[0]; s++)
    for (kind = 0; kind < 4; kind++) {
      fill(rgb, SZ[s][0], SZ[s][1], kind);
      for (q = 0; q < sizeof Q / sizeof Q[0]; q++)
        CHECK(same(rgb, SZ[s][0], SZ[s][1], Q[q], 0, NULL, 0, NULL));
    }
  /* the share's settings: q90, density, ICC, comment */
  fill(rgb, 320, 200, 0);
  CHECK(same(rgb, 320, 200, FT_JPEG_QUALITY, 1411.2, icc, icc_n,
             "emulsion=portra400\ncmyd=0 2 0 -1\n"));
  CHECK(same(rgb, 320, 200, FT_JPEG_QUALITY, 70000, icc, icc_n, ""));
  CHECK(same(rgb, 320, 200, FT_JPEG_QUALITY, 0.3, NULL, 0, "x"));
  /* an ICC profile over one APP2 marker: chunked as libjpeg does */
  big = malloc(150000);
  CHECK(big != NULL);
  for (s = 0; s < 150000; s++) big[s] = (unsigned char)rnd(256);
  CHECK(same(rgb, 64, 48, 80, 300, big, 150000, NULL));
  free(big);

  /* refusals */
  CHECK(ft_jpeg_encode(rgb, 0, 5, 90, 0, NULL, 0, NULL, &out, &on, &e) == -1);
  CHECK(strcmp(e.msg, "jpeg: bad size 0x5") == 0);
  CHECK(ft_jpeg_encode(rgb, 5, 5, 101, 0, NULL, 0, NULL, &out, &on, &e) == -1);
  CHECK(strcmp(e.msg, "jpeg: quality 101 (1..100)") == 0);
  CHECK(out == NULL && on == 0);
  free(rgb);
  return TEST_DONE();
}

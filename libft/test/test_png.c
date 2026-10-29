/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_png.h"
#include "test/ftest.h"

int main(void) {
  static unsigned char rgb[300 * 300 * 3]; /* > 64 KiB: several blocks */
  unsigned char head[24];
  const char* path = "test/png_test_out.png";
  ft_err e;
  FILE* f;
  long size, raw = 300L * (300 * 3 + 1);
  long blocks = (raw + 65534) / 65535;

  memset(rgb, 128, sizeof rgb);
  CHECK(ft_write_png_rgb8(path, rgb, 300, 300, &e) == 0);
  f = fopen(path, "rb");
  CHECK(f != NULL);
  CHECK(fread(head, 1, 24, f) == 24);
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fclose(f);
  remove(path);
  CHECK(memcmp(head, "\x89PNG\r\n\x1a\n", 8) == 0);
  CHECK(memcmp(head + 12, "IHDR", 4) == 0);
  CHECK(head[16] == 0 && head[17] == 0 && head[18] == 1 && head[19] == 44);
  /* signature + IHDR + IDAT(zlib header, stored blocks, adler) + IEND */
  CHECK(size == 8 + 25 + 12 + 2 + raw + 5 * blocks + 4 + 12);
  CHECK(ft_write_png_rgb8(path, rgb, 0, 3, &e) == -1);

  /* 16 bits: depth 16 in IHDR, big-endian samples after the filter byte */
  {
    static const unsigned short px16[6] = {0x0102, 0xA0B0, 65535, 0, 1, 0x8000};
    unsigned char buf[128];
    long n;
    CHECK(ft_write_png_rgb16(path, px16, 2, 1, &e) == 0);
    f = fopen(path, "rb");
    CHECK(f != NULL);
    n = (long)fread(buf, 1, sizeof buf, f);
    fclose(f);
    remove(path);
    CHECK(buf[24] == 16 && buf[25] == 2); /* depth, RGB */
    /* IDAT data at 41: zlib header (2), stored block header (5), row */
    CHECK(memcmp(buf + 37, "IDAT", 4) == 0);
    CHECK(buf[41 + 7] == 0); /* filter: none */
    CHECK(buf[41 + 8] == 0x01 && buf[41 + 9] == 0x02);
    CHECK(buf[41 + 10] == 0xA0 && buf[41 + 11] == 0xB0);
    CHECK(buf[41 + 17] == 0x01 && buf[41 + 18] == 0x80);
    CHECK(n == 8 + 25 + 12 + 2 + 13 + 5 + 4 + 12);
    CHECK(ft_write_png_rgb16(path, px16, 0, 1, &e) == -1);
  }
  return TEST_DONE();
}

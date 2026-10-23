/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_png.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long crc_table[256];
static int crc_ready;

static unsigned long crc32_update(
    unsigned long c, const unsigned char *b, size_t n
) {
  if(!crc_ready) {
    unsigned long v;
    int i, k;
    for(i = 0; i < 256; i++) {
      v = (unsigned long)i;
      for(k = 0; k < 8; k++)
        v = v & 1 ? 0xedb88320UL ^ (v >> 1) : v >> 1;
      crc_table[i] = v;
    }
    crc_ready = 1;
  }
  c ^= 0xffffffffUL;
  while(n--)
    c = crc_table[(c ^ *b++) & 0xff] ^ (c >> 8);
  return c ^ 0xffffffffUL;
}

static void put32(unsigned char *p, unsigned long v) {
  p[0] = (unsigned char)(v >> 24);
  p[1] = (unsigned char)(v >> 16);
  p[2] = (unsigned char)(v >> 8);
  p[3] = (unsigned char)v;
}

static int chunk(
    FILE *f, const char *type, const unsigned char *data, size_t n
) {
  unsigned char hdr[8], tail[4];
  unsigned long c;
  put32(hdr, (unsigned long)n);
  memcpy(hdr + 4, type, 4);
  c = crc32_update(0, hdr + 4, 4);
  if(n)
    c = crc32_update(c, data, n);
  put32(tail, c);
  return fwrite(hdr, 1, 8, f) == 8 && (n == 0 || fwrite(data, 1, n, f) == n) &&
                 fwrite(tail, 1, 4, f) == 4
             ? 0
             : -1;
}

/* px: h rows of 3 * w samples, depth 8 (bytes) or 16 (big-endian pairs) */
static int write_png(
    const char *path, const unsigned char *px, int w, int h, int depth,
    ft_err *e
) {
  static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  size_t row, raw, nblk, pos = 0;
  unsigned char ihdr[13], *z, *p;
  unsigned long a = 1, b = 0;
  FILE *f;
  int ok;

  if(w <= 0 || h <= 0)
    return ft_fail(e, "png: empty image");
  row = 3 * (size_t)w * (depth / 8) + 1; /* filter byte + pixels */
  raw = row * (size_t)h;
  nblk = (raw + 65534) / 65535;
  z = malloc(2 + raw + 5 * nblk + 4);
  if(!z)
    return ft_fail(e, "png: out of memory");
  p = z;
  *p++ = 0x78; /* zlib: deflate, 32K window */
  *p++ = 0x01;
  while(pos < raw) {
    size_t len = raw - pos > 65535 ? 65535 : raw - pos, i;
    unsigned nlen = (unsigned)(0xffff ^ len);
    *p++ = pos + len == raw ? 1 : 0; /* BFINAL, BTYPE = stored */
    *p++ = (unsigned char)(len & 0xff);
    *p++ = (unsigned char)(len >> 8);
    *p++ = (unsigned char)(nlen & 0xff);
    *p++ = (unsigned char)(nlen >> 8);
    for(i = 0; i < len; i++, pos++) {
      size_t r = pos / row, c = pos % row;
      unsigned char v = c == 0 ? 0 : px[r * (row - 1) + c - 1];
      *p++ = v;
      a = (a + v) % 65521;
      b = (b + a) % 65521;
    }
  }
  put32(p, (b << 16) | a);
  p += 4;

  put32(ihdr, (unsigned long)w);
  put32(ihdr + 4, (unsigned long)h);
  ihdr[8] = (unsigned char)depth; /* bit depth */
  ihdr[9] = 2;                    /* colour type RGB */
  ihdr[10] = ihdr[11] = ihdr[12] = 0;

  f = fopen(path, "wb");
  if(!f) {
    free(z);
    return ft_fail(e, "png: cannot write %s", path);
  }
  ok = fwrite(sig, 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) == 0 &&
       chunk(f, "IDAT", z, (size_t)(p - z)) == 0 &&
       chunk(f, "IEND", NULL, 0) == 0;
  ok = fclose(f) == 0 && ok;
  free(z);
  return ok ? 0 : ft_fail(e, "png: write error on %s", path);
}

int ft_png_write_rgb8(
    const char *path, const unsigned char *rgb, int w, int h, ft_err *e
) {
  return write_png(path, rgb, w, h, 8, e);
}

int ft_png_write_rgb16(
    const char *path, const unsigned short *rgb, int w, int h, ft_err *e
) {
  size_t n, i;
  unsigned char *be;
  int rc;
  if(w <= 0 || h <= 0)
    return ft_fail(e, "png: empty image");
  n = 3 * (size_t)w * h;
  be = malloc(2 * n);
  if(!be)
    return ft_fail(e, "png: out of memory");
  for(i = 0; i < n; i++) { /* PNG samples are big-endian */
    be[2 * i] = (unsigned char)(rgb[i] >> 8);
    be[2 * i + 1] = (unsigned char)(rgb[i] & 0xff);
  }
  rc = write_png(path, be, w, h, 16, e);
  free(be);
  return rc;
}

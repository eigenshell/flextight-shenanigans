/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Tag set and layout per the DNG 1.4 specification (Adobe, 2012) and
 * TIFF 6.0 (ft_tiff_ifd.h): little-endian, one IFD, tags ascending,
 * values over 4 bytes after the IFD (word-aligned), then the strips. */
#include "ft_dng.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ft_tiff_ifd.h"

int ft_dng_noise_ok(const double* v) {
  int i;
  for(i = 0; i < 6; i++)
    if(!isfinite(v[i]) || v[i] < 0 || (i % 2 && v[i] == 0)) return 0;
  return 1;
}

unsigned ft_dng_q16(float v) {
  return v <= 0 ? 0 : v >= 1 ? 65535 : (unsigned)(v * 65535.0f + 0.5f);
}

void ft_dng_as_stored(ft_image* im) {
  size_t k, n = im->rgb ? (size_t)im->w * im->h * 3 : 0;
  for(k = 0; k < n; k++) im->rgb[k] = (float)ft_dng_q16(im->rgb[k]) / 65535.0f;
  /* XResolution: a rational over 1000 (ft_ifd_common), absent if 0 */
  im->ppi = im->ppi > 0
              ? (double)((unsigned long)(long)floor(im->ppi * 1000 + 0.5) &
                         0xFFFFFFFFUL) /
                  1000
              : 0;
  if(im->has_noise && !ft_dng_noise_ok(im->noise)) im->has_noise = 0;
}

/* ------------------------------------------------------------- reader */

static unsigned long rd16(const unsigned char* p) {
  return (unsigned long)p[0] | (unsigned long)p[1] << 8;
}

static size_t type_size(unsigned type) {
  return type == FT_T_SHORT  ? 2
         : type == FT_T_LONG ? 4
         : type == FT_T_RATIONAL ||
             type == FT_T_SRATIONAL ||
             type == FT_T_DOUBLE
           ? 8
           : 1;
}

static unsigned long rd32(const unsigned char* p) {
  return rd16(p) | rd16(p + 2) << 16;
}

typedef struct {
  unsigned long type, count, at; /* at: offset of the value bytes */
} tag_ref;

/* the tags the reader looks at */
static const unsigned WANT[] = {256, 257, 258,   259,   262,  270,
                                271, 272, 273,   277,   278,  279,
                                282, 284, 50706, 50721, 51041};
#define NWANT (sizeof WANT / sizeof WANT[0])

static int want(unsigned long tag) {
  unsigned i;
  for(i = 0; i < NWANT; i++)
    if(WANT[i] == tag) return (int)i;
  return -1;
}

/* NULL if the file has no such tag */
static const tag_ref* find(const tag_ref* t, unsigned tag) {
  int i = want(tag);
  return i >= 0 && t[i].count ? &t[i] : NULL;
}

static int tag_value(const unsigned char* b, const tag_ref* r, unsigned long i,
                     unsigned long* v) {
  if(!r || i >= r->count) return -1;
  if(r->type == FT_T_SHORT)
    *v = rd16(b + r->at + 2 * i);
  else if(r->type == FT_T_LONG)
    *v = rd32(b + r->at + 4 * i);
  else
    return -1;
  return 0;
}

int ft_dng_parse(const unsigned char* b, size_t n, ft_image* im, ft_err* e) {
  tag_ref tags[NWANT];
  unsigned long ifd, cnt, i, w, h, v, rps, nstrips, row_bytes, s;
  const tag_ref *off, *len;

  memset(tags, 0, sizeof tags);
  if(n < 8 || memcmp(b, "II*\0", 4) != 0)
    return ft_fail(e, "dng: not a little-endian TIFF");
  ifd = rd32(b + 4);
  if(ifd < 8 || ifd > n || n - ifd < 2)
    return ft_fail(e, "dng: bad IFD offset");
  cnt = rd16(b + ifd);
  if(cnt > 200 || n - ifd - 2 < 12 * cnt + 4)
    return ft_fail(e, "dng: truncated IFD");
  for(i = 0; i < cnt; i++) {
    const unsigned char* p = b + ifd + 2 + 12 * i;
    unsigned long tag = rd16(p), type = rd16(p + 2), c = rd32(p + 4);
    unsigned long sz = type_size((unsigned)type), bytes;
    int k = want(tag);
    tag_ref* r;
    if(k < 0 || !c) continue;
    if(c > 0x10000000UL / sz) return ft_fail(e, "dng: tag %lu too large", tag);
    bytes = sz * c;
    r = &tags[k];
    r->type = type;
    r->count = c;
    r->at = bytes <= 4 ? (unsigned long)(p + 8 - b) : rd32(p + 8);
    if(r->at > n || bytes > n - r->at)
      return ft_fail(e, "dng: tag %lu points past the end", tag);
  }
  if(!find(tags, 50706)) return ft_fail(e, "dng: no DNGVersion: not a DNG");
  if(tag_value(b, find(tags, 262), 0, &v) || v != 34892)
    return ft_fail(e, "dng: not LinearRaw (only this writer's DNGs)");
  if(tag_value(b, find(tags, 259), 0, &v) || v != 1)
    return ft_fail(e, "dng: compressed (only uncompressed is read)");
  if(tag_value(b, find(tags, 277), 0, &v) || v != 3)
    return ft_fail(e, "dng: not 3 samples per pixel");
  if(find(tags, 284) && (tag_value(b, find(tags, 284), 0, &v) || v != 1))
    return ft_fail(e, "dng: not chunky (interleaved) RGB");
  for(i = 0; i < 3; i++)
    if(tag_value(b, find(tags, 258), i, &v) || v != 16)
      return ft_fail(e, "dng: not 16 bits per sample");
  if(tag_value(b, find(tags, 256), 0, &w) ||
     tag_value(b, find(tags, 257), 0, &h) ||
     w < 1 ||
     h < 1 ||
     w > 100000 ||
     h > 100000)
    return ft_fail(e, "dng: bad size");
  row_bytes = w * 6;
  if(tag_value(b, find(tags, 278), 0, &rps) || rps < 1) rps = h;
  nstrips = (h + rps - 1) / rps;
  off = find(tags, 273);
  len = find(tags, 279);
  if(!off || !len || off->count != nstrips || len->count != nstrips)
    return ft_fail(e, "dng: strip tags do not match the size");
  /* every strip in the file before anything is allocated */
  for(s = 0; s < nstrips; s++) {
    unsigned long o, l, rows = s + 1 < nstrips ? rps : h - s * rps;
    /* in double: rows x row_bytes may wrap a 32-bit long */
    if(tag_value(b, off, s, &o) ||
       tag_value(b, len, s, &l) ||
       (double)rows * row_bytes > (double)n ||
       l != rows * row_bytes ||
       o > n ||
       l > n - o)
      return ft_fail(e, "dng: strip %lu outside the file", s);
  }
  /* strips may overlap, so they alone do not bound w x h: the pixels
     must fit in the file (double: no wrap on 32 bits) */
  if((double)w * h * 6 > (double)n)
    return ft_fail(e, "dng: %lux%lu pixels do not fit in the file", w, h);
  if(ft_image_alloc(im, (int)w, (int)h, e)) return -1;
  for(s = 0; s < nstrips; s++) {
    unsigned long o, rows = s + 1 < nstrips ? rps : h - s * rps, k;
    tag_value(b, off, s, &o);
    for(k = 0; k < rows * w * 3; k++)
      im->rgb[s * rps * w * 3 + k] = (float)rd16(b + o + 2 * k) / 65535.0f;
  }
  {
    const tag_ref* x = find(tags, 282);
    if(x && x->type == FT_T_RATIONAL && rd32(b + x->at + 4))
      im->ppi = (double)rd32(b + x->at) / rd32(b + x->at + 4);
  }
  {
    /* a bad NoiseProfile reads as none (export then estimates) */
    const tag_ref* np = find(tags, 51041);
    if(np && np->type == FT_T_DOUBLE && np->count == 6) {
      double v[6];
      int k;
      for(i = 0; i < 6; i++) {
        uint64_t u = 0;
        for(k = 7; k >= 0; k--) u = u << 8 | b[np->at + 8 * i + k];
        memcpy(&v[i], &u, 8);
      }
      if(ft_dng_noise_ok(v)) {
        memcpy(im->noise, v, sizeof v);
        im->has_noise = 1;
      }
    }
  }
  {
    const tag_ref* cm = find(tags, 50721);
    if(cm && cm->type == FT_T_SRATIONAL && cm->count == 9)
      for(i = 0; i < 9; i++) {
        long num = (long)(int)rd32(b + cm->at + 8 * i);
        long den = (long)(int)rd32(b + cm->at + 8 * i + 4);
        if(den) im->xyz_to_rgb[i] = (double)num / den;
      }
  }
  {
    static const struct {
      unsigned tag;
      size_t cap;
      size_t off;
    } S[] = {
      {270, sizeof im->desc, offsetof(ft_image, desc)},
      {271, sizeof im->make, offsetof(ft_image, make)},
      {272, sizeof im->model, offsetof(ft_image, model)},
    };
    for(i = 0; i < 3; i++) {
      const tag_ref* t = find(tags, S[i].tag);
      char* dst = (char*)im + S[i].off;
      size_t m;
      if(!t || t->type != FT_T_ASCII) continue;
      m = t->count < S[i].cap ? t->count : S[i].cap - 1;
      memcpy(dst, b + t->at, m);
      dst[m] = 0;
    }
  }
  return 0;
}

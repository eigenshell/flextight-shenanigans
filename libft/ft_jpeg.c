/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Baseline JPEG encoder (ITU T.81), 8-bit YCbCr 4:4:4, one interleaved
 * scan, optimised Huffman tables. Each step does what libjpeg-turbo does
 * with jpeg_set_defaults + jpeg_set_quality(q, TRUE) + optimize_coding
 * and the default integer DCT, so the output bytes match it:
 *   - edges: the last column and row repeat out to a multiple of 8;
 *   - colour: the JFIF matrix in 16-bit fixed point, Cb/Cr rounded with
 *     0.5 - epsilon;
 *   - DCT: the Loeffler-Ligtenberg-Moschytz integer transform with 13
 *     constant bits and 2 pass-1 bits (output scaled by 8);
 *   - quantisation: rounded division by 8 * the table entry;
 *   - Huffman: frequencies from a first pass, code lengths by T.81 K.2
 *     (ties go to the higher symbol, a reserved all-ones code), lengths
 *     over 16 folded as in K.3;
 *   - markers: SOI, JFIF APP0, ICC APP2s, COM, DQT 0, DQT 1, SOF0,
 *     DHT DC0 AC0 DC1 AC1, SOS, data, EOI.
 * The DCT is redone in the second pass instead of keeping the
 * coefficients: memory stays one block row. */
#include "ft_jpeg.h"

#include <stdlib.h>
#include <string.h>

/* zigzag position -> natural (row-major) index */
static const unsigned char ZZ[64] = {
  0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
  12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
  35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
  58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/* T.81 Annex K tables, natural order */
static const unsigned char QBASE[2][64] = {
  {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
   14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
   18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
   49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99},
  {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
   24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
   99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
   99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99}};

typedef struct {
  unsigned char* b;
  size_t n, cap;
  int oom;
  unsigned long acc; /* pending bits, low nacc bits */
  int nacc;
} wbuf;

/* one Huffman table: lengths and values as written, codes derived */
typedef struct {
  unsigned char bits[17]; /* bits[1..16]: codes per length */
  unsigned char val[256];
  unsigned short code[256];
  unsigned char size[256]; /* 0: no code */
} htab;

typedef struct {
  int q[2][64];         /* natural order, table values */
  long freq[2][2][257]; /* [dc/ac][table][symbol] */
  htab h[2][2];
} enc;

static void put(wbuf* w, const void* p, size_t n) {
  if(w->oom) return;
  if(w->n + n > w->cap) {
    size_t c = w->cap ? w->cap : 4096;
    unsigned char* nb;
    while(c < w->n + n) c *= 2;
    nb = realloc(w->b, c);
    if(!nb) {
      w->oom = 1;
      return;
    }
    w->b = nb;
    w->cap = c;
  }
  memcpy(w->b + w->n, p, n);
  w->n += n;
}

static void byte(wbuf* w, int v) {
  unsigned char c = (unsigned char)v;
  put(w, &c, 1);
}

static void word(wbuf* w, int v) {
  byte(w, v >> 8);
  byte(w, v & 255);
}

/* entropy-coded bits, MSB first, a 0 after every 0xFF byte */
static void bits(wbuf* w, unsigned v, int n) {
  w->acc = (w->acc << n) | (v & ((1UL << n) - 1));
  w->nacc += n;
  while(w->nacc >= 8) {
    int c = (int)((w->acc >> (w->nacc - 8)) & 255);
    w->nacc -= 8;
    byte(w, c);
    if(c == 255) byte(w, 0);
  }
  w->acc &= (1UL << w->nacc) - 1;
}

/* the 8-bit quantisation tables for quality q */
static void quant_tables(enc* s, int q) {
  int scale = q < 50 ? 5000 / q : 200 - q * 2, t, i;
  for(t = 0; t < 2; t++)
    for(i = 0; i < 64; i++) {
      long v = ((long)QBASE[t][i] * scale + 50) / 100;
      s->q[t][i] = (int)(v < 1 ? 1 : v > 255 ? 255 : v);
    }
}

/* the forward DCT, in place, input level-shifted samples */
#define CB 13 /* constant bits */
#define P1 2  /* pass-1 bits */
#define DESC(x, n) (((x) + (1L << ((n) - 1))) >> (n))
static void fdct(long* d) {
  long t0, t1, t2, t3, t4, t5, t6, t7, t10, t11, t12, t13;
  long z1, z2, z3, z4, z5;
  int i, pass;
  for(pass = 0; pass < 2; pass++)
    for(i = 0; i < 8; i++) {
      long* p = pass ? d + i : d + i * 8;
      int st = pass ? 8 : 1;
      int sh = pass ? CB + P1 : CB - P1;
      t0 = p[0] + p[7 * st];
      t7 = p[0] - p[7 * st];
      t1 = p[st] + p[6 * st];
      t6 = p[st] - p[6 * st];
      t2 = p[2 * st] + p[5 * st];
      t5 = p[2 * st] - p[5 * st];
      t3 = p[3 * st] + p[4 * st];
      t4 = p[3 * st] - p[4 * st];
      t10 = t0 + t3;
      t13 = t0 - t3;
      t11 = t1 + t2;
      t12 = t1 - t2;
      if(pass) {
        p[0] = DESC(t10 + t11, P1);
        p[4 * st] = DESC(t10 - t11, P1);
      } else {
        p[0] = (t10 + t11) * (1 << P1);
        p[4 * st] = (t10 - t11) * (1 << P1);
      }
      z1 = (t12 + t13) * 4433;                /* 0.541196100 */
      p[2 * st] = DESC(z1 + t13 * 6270, sh);  /* 0.765366865 */
      p[6 * st] = DESC(z1 - t12 * 15137, sh); /* 1.847759065 */
      z1 = t4 + t7;
      z2 = t5 + t6;
      z3 = t4 + t6;
      z4 = t5 + t7;
      z5 = (z3 + z4) * 9633; /* 1.175875602 */
      t4 *= 2446;            /* 0.298631336 */
      t5 *= 16819;           /* 2.053119869 */
      t6 *= 25172;           /* 3.072711026 */
      t7 *= 12299;           /* 1.501321110 */
      z1 *= -7373;           /* 0.899976223 */
      z2 *= -20995;          /* 2.562915447 */
      z3 *= -16069;          /* 1.961570560 */
      z4 *= -3196;           /* 0.390180644 */
      z3 += z5;
      z4 += z5;
      p[7 * st] = DESC(t4 + z1 + z3, sh);
      p[5 * st] = DESC(t5 + z2 + z4, sh);
      p[3 * st] = DESC(t6 + z2 + z3, sh);
      p[st] = DESC(t7 + z1 + z4, sh);
    }
}

/* one block of component c (0 Y, 1 Cb, 2 Cr) at block column bx of the
 * block row whose 8 rows (edge-repeated) are rows[] -> quantised
 * coefficients, natural order */
static void block(const enc* s, const unsigned char* const rows[8], int w,
                  int bx, int c, int out[64]) {
  long d[64];
  const int* q = s->q[c ? 1 : 0];
  int x, y;
  for(y = 0; y < 8; y++)
    for(x = 0; x < 8; x++) {
      int xx = bx * 8 + x;
      const unsigned char* p = rows[y] + (xx < w ? xx : w - 1) * 3;
      long r = p[0], g = p[1], b = p[2], v;
      /* the JFIF matrix, 16 fraction bits: FIX(0.29900) = 19595 ..;
       * Cb/Cr: + 128 and 0.5 - epsilon so 255 stays 255 */
      if(c == 0)
        v = (19595 * r + 38470 * g + 7471 * b + 32768) >> 16;
      else if(c == 1)
        v = (-11059 * r - 21709 * g + 32768 * b + (128L << 16) + 32767) >> 16;
      else
        v = (32768 * r - 27439 * g - 5329 * b + (128L << 16) + 32767) >> 16;
      d[y * 8 + x] = v - 128;
    }
  fdct(d);
  for(x = 0; x < 64; x++) {
    long dv = (long)q[x] * 8;
    out[x] = (int)(d[x] < 0 ? -((-d[x] + dv / 2) / dv) : (d[x] + dv / 2) / dv);
  }
}

static int nbits(int v) {
  int n = 0;
  if(v < 0) v = -v;
  while(v) {
    n++;
    v >>= 1;
  }
  return n;
}

/* count (w NULL) or write one block's symbols */
static void code_block(enc* s, wbuf* w, const int blk[64], int t, int* dc) {
  int diff = blk[0] - *dc, r = 0, k, n;
  htab *hd = &s->h[0][t], *ha = &s->h[1][t];
  *dc = blk[0];
  n = nbits(diff);
  if(w) {
    bits(w, hd->code[n], hd->size[n]);
    if(n) bits(w, (unsigned)(diff < 0 ? diff - 1 : diff), n);
  } else {
    s->freq[0][t][n]++;
  }
  for(k = 1; k < 64; k++) {
    int v = blk[ZZ[k]], sym;
    if(v == 0) {
      r++;
      continue;
    }
    while(r > 15) {
      if(w)
        bits(w, ha->code[0xF0], ha->size[0xF0]);
      else
        s->freq[1][t][0xF0]++;
      r -= 16;
    }
    n = nbits(v);
    sym = (r << 4) + n;
    if(w) {
      bits(w, ha->code[sym], ha->size[sym]);
      bits(w, (unsigned)(v < 0 ? v - 1 : v), n);
    } else {
      s->freq[1][t][sym]++;
    }
    r = 0;
  }
  if(r > 0) {
    if(w)
      bits(w, ha->code[0], ha->size[0]);
    else
      s->freq[1][t][0]++;
  }
}

/* T.81 K.2 code lengths from the frequencies, K.3 limit to 16 bits, then
 * the canonical codes (C.2) */
static void optimal(const long* freq_in, htab* h) {
  long freq[257];
  int csize[257], others[257], cnt[33], i, j, c1, c2, p;
  unsigned code;
  memcpy(freq, freq_in, sizeof freq);
  memset(csize, 0, sizeof csize);
  memset(cnt, 0, sizeof cnt);
  for(i = 0; i < 257; i++) others[i] = -1;
  freq[256] = 1; /* no code is all ones */
  for(;;) {
    long v = 1000000000L;
    c1 = c2 = -1;
    for(i = 0; i <= 256; i++)
      if(freq[i] && freq[i] <= v) {
        v = freq[i];
        c1 = i;
      }
    v = 1000000000L;
    for(i = 0; i <= 256; i++)
      if(freq[i] && freq[i] <= v && i != c1) {
        v = freq[i];
        c2 = i;
      }
    if(c2 < 0) break;
    freq[c1] += freq[c2];
    freq[c2] = 0;
    csize[c1]++;
    while(others[c1] >= 0) {
      c1 = others[c1];
      csize[c1]++;
    }
    others[c1] = c2;
    csize[c2]++;
    while(others[c2] >= 0) {
      c2 = others[c2];
      csize[c2]++;
    }
  }
  for(i = 0; i <= 256; i++)
    if(csize[i]) cnt[csize[i] > 32 ? 32 : csize[i]]++;
  for(i = 32; i > 16; i--)
    while(cnt[i] > 0) {
      j = i - 2;
      while(cnt[j] == 0) j--;
      cnt[i] -= 2;
      cnt[i - 1]++;
      cnt[j + 1] += 2;
      cnt[j]--;
    }
  while(cnt[i] == 0) i--;
  cnt[i]--; /* drop the reserved code */
  memset(h, 0, sizeof *h);
  for(i = 1; i <= 16; i++) h->bits[i] = (unsigned char)cnt[i];
  p = 0;
  for(i = 1; i <= 32; i++)
    for(j = 0; j <= 255; j++)
      if(csize[j] == i) h->val[p++] = (unsigned char)j;
  /* canonical codes in the order of val[] */
  code = 0;
  p = 0;
  for(i = 1; i <= 16; i++) {
    for(j = 0; j < h->bits[i]; j++) {
      h->code[h->val[p]] = (unsigned short)code++;
      h->size[h->val[p]] = (unsigned char)i;
      p++;
    }
    code <<= 1;
  }
}

static void marker(wbuf* w, int m, int len) {
  byte(w, 0xFF);
  byte(w, m);
  word(w, len);
}

static void headers(const enc* s, wbuf* w, int wd, int ht, double ppi,
                    const unsigned char* icc, size_t icc_n,
                    const char* comment) {
  static const unsigned char JFIF[5] = {'J', 'F', 'I', 'F', 0};
  static const unsigned char ICC[12] = "ICC_PROFILE";
  int t, i, k;
  byte(w, 0xFF);
  byte(w, 0xD8);
  marker(w, 0xE0, 16);
  put(w, JFIF, 5);
  byte(w, 1); /* JFIF 1.01 */
  byte(w, 1);
  if(ppi > 0) {
    long d = (long)(ppi + 0.5);
    d = d < 1 ? 1 : d > 65535 ? 65535 : d;
    byte(w, 1); /* dots per inch */
    word(w, (int)d);
    word(w, (int)d);
  } else {
    byte(w, 0);
    word(w, 1);
    word(w, 1);
  }
  byte(w, 0); /* no thumbnail */
  byte(w, 0);
  if(icc && icc_n) {
    size_t nm = (icc_n + 65518) / 65519, off = 0;
    for(k = 1; off < icc_n; k++) {
      size_t len = icc_n - off > 65519 ? 65519 : icc_n - off;
      marker(w, 0xE2, (int)(len + 16));
      put(w, ICC, 12);
      byte(w, k);
      byte(w, (int)nm);
      put(w, icc + off, len);
      off += len;
    }
  }
  if(comment && *comment) {
    size_t len = strlen(comment);
    marker(w, 0xFE, (int)(len + 2));
    put(w, comment, len);
  }
  for(t = 0; t < 2; t++) {
    marker(w, 0xDB, 67);
    byte(w, t);
    for(i = 0; i < 64; i++) byte(w, s->q[t][ZZ[i]]);
  }
  marker(w, 0xC0, 17);
  byte(w, 8);
  word(w, ht);
  word(w, wd);
  byte(w, 3);
  for(i = 0; i < 3; i++) {
    byte(w, i + 1);
    byte(w, 0x11);
    byte(w, i ? 1 : 0);
  }
  for(t = 0; t < 2; t++)
    for(k = 0; k < 2; k++) { /* DC t, AC t */
      const htab* h = &s->h[k][t];
      int nv = 0;
      for(i = 1; i <= 16; i++) nv += h->bits[i];
      marker(w, 0xC4, 19 + nv);
      byte(w, (k << 4) | t);
      put(w, h->bits + 1, 16);
      put(w, h->val, (size_t)nv);
    }
  marker(w, 0xDA, 12);
  byte(w, 3);
  for(i = 0; i < 3; i++) {
    byte(w, i + 1);
    byte(w, i ? 0x11 : 0x00);
  }
  byte(w, 0);
  byte(w, 63);
  byte(w, 0);
}

/* every MCU (Y, Cb, Cr blocks) in raster order: count or write */
static void scan(enc* s, wbuf* w, const unsigned char* rgb, int wd, int ht) {
  int bw = (wd + 7) / 8, bh = (ht + 7) / 8, by, bx, c, y;
  int dc[3] = {0, 0, 0}, blk[64];
  const unsigned char* rows[8];
  for(by = 0; by < bh; by++) {
    for(y = 0; y < 8; y++) {
      int yy = by * 8 + y;
      rows[y] = rgb + (size_t)(yy < ht ? yy : ht - 1) * wd * 3;
    }
    for(bx = 0; bx < bw; bx++)
      for(c = 0; c < 3; c++) {
        block(s, rows, wd, bx, c, blk);
        code_block(s, w, blk, c ? 1 : 0, &dc[c]);
      }
  }
}

int ft_jpeg_encode(const unsigned char* rgb, int w, int h, int quality,
                   double ppi, const unsigned char* icc, size_t icc_n,
                   const char* comment, unsigned char** out, size_t* n,
                   ft_err* e) {
  enc* s;
  wbuf b;
  int t, k;

  *out = NULL;
  *n = 0;
  if(!rgb || w < 1 || h < 1 || w > 65500 || h > 65500)
    return ft_fail(e, "jpeg: bad size %dx%d", w, h);
  if(quality < 1 || quality > 100)
    return ft_fail(e, "jpeg: quality %d (1..100)", quality);
  if(icc && icc_n > 65519L * 255)
    return ft_fail(e, "jpeg: ICC profile too large");
  if(comment && strlen(comment) > FT_JPEG_COMMENT_MAX)
    return ft_fail(e, "jpeg: comment too long");
  s = calloc(1, sizeof *s);
  if(!s) return ft_fail(e, "jpeg: out of memory");
  quant_tables(s, quality);
  scan(s, NULL, rgb, w, h);
  for(k = 0; k < 2; k++)
    for(t = 0; t < 2; t++) optimal(s->freq[k][t], &s->h[k][t]);
  memset(&b, 0, sizeof b);
  headers(s, &b, w, h, ppi, icc, icc_n, comment);
  scan(s, &b, rgb, w, h);
  if(b.nacc) /* pad with ones */
    bits(&b, 0x7F, 8 - b.nacc);
  byte(&b, 0xFF);
  byte(&b, 0xD9);
  free(s);
  if(b.oom) {
    free(b.b);
    return ft_fail(e, "jpeg: out of memory");
  }
  *out = b.b;
  *n = b.n;
  return 0;
}

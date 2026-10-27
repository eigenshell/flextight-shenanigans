/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Layout per ICC.1:2001-04 (version 2.4 of the format; the profiles say
 * 2.1, the lowest version with every type used here). */
#include "ft_icc.h"

#include <math.h>
#include <string.h>

#define TRC_N 1024

/* ICC's D50 as stored in s15Fixed16 (the header's illuminant) */
static const double D50[3] = {0.9642, 1.0, 0.8249};

static void be16(unsigned char* p, unsigned long v) {
  p[0] = (unsigned char)(v >> 8);
  p[1] = (unsigned char)v;
}

static void be32(unsigned char* p, unsigned long v) {
  be16(p, (v >> 16) & 0xffff);
  be16(p + 2, v & 0xffff);
}

static void s15(unsigned char* p, double v) {
  long f = (long)floor(v * 65536.0 + 0.5);
  be32(p, (unsigned long)f & 0xffffffffUL);
}

void ft_icc_bradford(double m[9]) {
  static const double B[9] = {0.8951, 0.2664, -0.1614, -0.7502, 1.7135,
                              0.0367, 0.0389, -0.0685, 1.0296};
  double bi[9], w[3], src[3], dst[3], d[9], t[9];
  int i, j, k;
  double xy[8];
  ft_space_primaries(FT_SPACE_SRGB, xy);
  w[0] = xy[6] / xy[7];
  w[1] = 1;
  w[2] = (1 - xy[6] - xy[7]) / xy[7];
  for(i = 0; i < 3; i++) {
    src[i] = B[3 * i] * w[0] + B[3 * i + 1] * w[1] + B[3 * i + 2] * w[2];
    dst[i] = B[3 * i] * D50[0] + B[3 * i + 1] * D50[1] + B[3 * i + 2] * D50[2];
  }
  ft_mat3_inv(B, bi);
  memset(d, 0, sizeof d);
  for(i = 0; i < 3; i++) d[4 * i] = dst[i] / src[i];
  for(i = 0; i < 3; i++) /* t = d B */
    for(j = 0; j < 3; j++) t[3 * i + j] = d[4 * i] * B[3 * i + j];
  for(i = 0; i < 3; i++) /* m = B^-1 t */
    for(j = 0; j < 3; j++) {
      m[3 * i + j] = 0;
      for(k = 0; k < 3; k++) m[3 * i + j] += bi[3 * i + k] * t[3 * k + j];
    }
}

/* a tag's data at buf + at; returns its size (unpadded) */
static size_t text_desc(unsigned char* p, const char* s) {
  size_t n = strlen(s) + 1;
  memcpy(p, "desc", 4);
  memset(p + 4, 0, 4);
  be32(p + 8, (unsigned long)n);
  memcpy(p + 12, s, n);
  memset(p + 12 + n, 0, 4 + 4 + 2 + 1 + 67); /* no Unicode, no Mac */
  return 12 + n + 78;
}

static size_t text(unsigned char* p, const char* s) {
  size_t n = strlen(s) + 1;
  memcpy(p, "text", 4);
  memset(p + 4, 0, 4);
  memcpy(p + 8, s, n);
  return 8 + n;
}

static size_t xyz(unsigned char* p, const double v[3]) {
  memcpy(p, "XYZ ", 4);
  memset(p + 4, 0, 4);
  s15(p + 8, v[0]);
  s15(p + 12, v[1]);
  s15(p + 16, v[2]);
  return 20;
}

/* s < 0: linear (no entries: the identity) */
static size_t curv(unsigned char* p, int s) {
  int i;
  memcpy(p, "curv", 4);
  memset(p + 4, 0, 4);
  if(s < 0) {
    be32(p + 8, 0);
    return 12;
  }
  if(s == FT_SPACE_ADOBE_RGB) {
    be32(p + 8, 1);
    be16(p + 12, 0x0233); /* u8Fixed8 2.19921875 = 563/256 */
    return 14;
  }
  be32(p + 8, TRC_N);
  for(i = 0; i < TRC_N; i++) {
    double v = ft_space_decode((ft_space)s, (double)i / (TRC_N - 1));
    be16(p + 12 + 2 * i, (unsigned long)floor(v * 65535 + 0.5));
  }
  return 12 + 2 * TRC_N;
}

/* the profile for RGB -> XYZ D65 matrix m with TRC curve (an ft_space,
 * or -1 for linear), described as desc */
static size_t build(const double m[9], int curve, const char* desc,
                    unsigned char* buf, size_t cap) {
  static const char* SIG[] = {"desc", "cprt", "wtpt", "rXYZ", "gXYZ",
                              "bXYZ", "rTRC", "gTRC", "bTRC"};
  unsigned char tmp[FT_ICC_MAX];
  double a[9], col[3][3];
  size_t at, size[9], off[9];
  int i, j, k;

  if(cap < 1) return 0;
  ft_icc_bradford(a);
  for(j = 0; j < 3; j++) /* colorant j, D50 */
    for(i = 0; i < 3; i++) {
      col[j][i] = 0;
      for(k = 0; k < 3; k++) col[j][i] += a[3 * i + k] * m[3 * k + j];
    }
  memset(tmp, 0, sizeof tmp);
  /* tag data after the header (128) and a 9-entry tag table (4 + 108) */
  at = 128 + 4 + 12 * 9;
  for(i = 0; i < 9; i++) {
    off[i] = at;
    if(i == 0)
      size[i] = text_desc(tmp + at, desc);
    else if(i == 1)
      size[i] = text(tmp + at, "No copyright, use freely");
    else if(i == 2)
      size[i] = xyz(tmp + at, D50);
    else if(i <= 5)
      size[i] = xyz(tmp + at, col[i - 3]);
    else if(i == 6)
      size[i] = curv(tmp + at, curve);
    else { /* g and b share r's curve */
      off[i] = off[6];
      size[i] = size[6];
      continue;
    }
    at += (size[i] + 3) & ~(size_t)3;
  }
  /* header */
  be32(tmp, (unsigned long)at);
  be32(tmp + 8, 0x02100000UL); /* version 2.1 */
  memcpy(tmp + 12, "mntr", 4);
  memcpy(tmp + 16, "RGB ", 4);
  memcpy(tmp + 20, "XYZ ", 4);
  be16(tmp + 24, 2026); /* fixed date: same bytes */
  be16(tmp + 26, 1);    /* every run */
  be16(tmp + 28, 1);
  memcpy(tmp + 36, "acsp", 4);
  be32(tmp + 64, 0); /* perceptual */
  s15(tmp + 68, D50[0]);
  s15(tmp + 72, D50[1]);
  s15(tmp + 76, D50[2]);
  be32(tmp + 128, 9);
  for(i = 0; i < 9; i++) {
    memcpy(tmp + 132 + 12 * i, SIG[i], 4);
    be32(tmp + 136 + 12 * i, (unsigned long)off[i]);
    be32(tmp + 140 + 12 * i, (unsigned long)size[i]);
  }
  if(at > cap) return 0;
  memcpy(buf, tmp, at);
  return at;
}

size_t ft_icc_build(ft_space s, unsigned char* buf, size_t cap) {
  double xy[8], m[9];
  char desc[64];
  const char *w, *t = " (flextight)";
  size_t n = 0;

  if((unsigned)s > FT_SPACE_REC2020) return 0;
  ft_space_primaries(s, xy);
  ft_rgb_to_xyz(xy, m);
  for(w = ft_space_what(s); *w && n < 40;) /* "<space> (flextight)" */
    desc[n++] = *w++;
  while(*t) desc[n++] = *t++;
  desc[n] = 0;
  return build(m, (int)s, desc, buf, cap);
}

size_t ft_icc_build_linear(const double xyz_to_rgb[9], const char* desc,
                           unsigned char* buf, size_t cap) {
  double m[9];
  char d[64];
  size_t n = 0;
  if(ft_mat3_inv(xyz_to_rgb, m)) return 0;
  while(desc[n] && n < sizeof d - 1) {
    d[n] = desc[n];
    n++;
  }
  d[n] = 0;
  return build(m, -1, d, buf, cap);
}

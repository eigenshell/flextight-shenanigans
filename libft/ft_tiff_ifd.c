/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_tiff_ifd.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static size_t type_size(unsigned type) {
  return type == FT_T_SHORT  ? 2
         : type == FT_T_LONG ? 4
         : type == FT_T_RATIONAL || type == FT_T_SRATIONAL ||
                 type == FT_T_DOUBLE
             ? 8
             : 1;
}

void ft_le16(unsigned char *p, unsigned long v) {
  p[0] = (unsigned char)v;
  p[1] = (unsigned char)(v >> 8);
}

void ft_le32(unsigned char *p, unsigned long v) {
  ft_le16(p, v & 0xffff);
  ft_le16(p + 2, v >> 16);
}

unsigned char *ft_ifd_add(
    ft_ifd *d, unsigned tag, unsigned type, unsigned long count
) {
  ft_ifd_entry *x;
  if(d->n == FT_IFD_MAX_TAGS || d->oom) {
    d->oom = 1;
    return NULL;
  }
  x = &d->e[d->n];
  x->len = type_size(type) * count;
  x->data = calloc(x->len ? x->len : 1, 1);
  if(!x->data) {
    d->oom = 1;
    return NULL;
  }
  x->tag = tag;
  x->type = type;
  x->count = count;
  x->off = 0;
  d->n++;
  return x->data;
}

void ft_ifd_short(ft_ifd *d, unsigned tag, unsigned long v) {
  unsigned char *p = ft_ifd_add(d, tag, FT_T_SHORT, 1);
  if(p)
    ft_le16(p, v);
}

void ft_ifd_long(ft_ifd *d, unsigned tag, unsigned long v) {
  unsigned char *p = ft_ifd_add(d, tag, FT_T_LONG, 1);
  if(p)
    ft_le32(p, v);
}

void ft_ifd_ascii(ft_ifd *d, unsigned tag, const char *s) {
  unsigned char *p =
      ft_ifd_add(d, tag, FT_T_ASCII, (unsigned long)strlen(s) + 1);
  if(p)
    memcpy(p, s, strlen(s));
}

void ft_ifd_bytes(
    ft_ifd *d, unsigned tag, unsigned type, const unsigned char *b,
    unsigned long n
) {
  unsigned char *p = ft_ifd_add(d, tag, type, n);
  if(p)
    memcpy(p, b, (size_t)n);
}

void ft_ifd_rationals(
    ft_ifd *d, unsigned tag, unsigned type, const double *v, int n, long den
) {
  unsigned char *p = ft_ifd_add(d, tag, type, (unsigned long)n);
  int i;
  for(i = 0; p && i < n; i++) {
    long num = (long)floor(v[i] * den + 0.5);
    ft_le32(p + 8 * i, (unsigned long)num);
    ft_le32(p + 8 * i + 4, (unsigned long)den);
  }
}

void ft_ifd_doubles(ft_ifd *d, unsigned tag, const double *v, int n) {
  unsigned char *p = ft_ifd_add(d, tag, FT_T_DOUBLE, (unsigned long)n);
  int i, k;
  for(i = 0; p && i < n; i++) {
    uint64_t u;
    memcpy(&u, &v[i], 8);
    for(k = 0; k < 8; k++)
      p[8 * i + k] = (unsigned char)(u >> 8 * k);
  }
}

ft_ifd_entry *ft_ifd_find(ft_ifd *d, unsigned tag) {
  int i;
  for(i = 0; i < d->n; i++)
    if(d->e[i].tag == tag)
      return &d->e[i];
  return NULL;
}

void ft_ifd_common(
    ft_ifd *d, const ft_image *im, int w, int h, int spp, int dng
) {
  static const unsigned char version[4] = {1, 4, 0, 0};
  static const unsigned char backward[4] = {1, 1, 0, 0};
  static const double neutral[3] = {1, 1, 1};
  char stamp[20] = "", ucm[80];
  time_t now = time(NULL);
  struct tm *tm = localtime(&now);
  unsigned char *p;
  int i;

  if(tm)
    strftime(stamp, sizeof stamp, "%Y:%m:%d %H:%M:%S", tm);
  ft_ifd_long(d, 254, 0); /* NewSubFileType: main */
  ft_ifd_long(d, 256, (unsigned long)w);
  ft_ifd_long(d, 257, (unsigned long)h);
  if((p = ft_ifd_add(d, 258, FT_T_SHORT, (unsigned long)spp)) != NULL)
    for(i = 0; i < spp; i++) /* BitsPerSample */
      ft_le16(p + 2 * i, 16);
  ft_ifd_short(d, 259, 1); /* no compression */
  if(im->desc[0])
    ft_ifd_ascii(d, 270, im->desc);
  ft_ifd_ascii(d, 271, im->make);
  ft_ifd_ascii(d, 272, im->model);
  ft_ifd_short(d, 274, 1); /* Orientation: as stored */
  ft_ifd_short(d, 277, (unsigned long)spp);
  if(im->ppi > 0) {
    ft_ifd_rationals(d, 282, FT_T_RATIONAL, &im->ppi, 1, 1000);
    ft_ifd_rationals(d, 283, FT_T_RATIONAL, &im->ppi, 1, 1000);
  }
  ft_ifd_short(d, 284, 1); /* chunky */
  if(im->ppi > 0)
    ft_ifd_short(d, 296, 2); /* inches */
  ft_ifd_ascii(d, 305, "flextight-shenanigans libft");
  if(stamp[0])
    ft_ifd_ascii(d, 306, stamp);
  if(!dng)
    return;
  ft_ifd_short(d, 262, 34892); /* LinearRaw */
  ft_ifd_bytes(d, 50706, FT_T_BYTE, version, 4);
  ft_ifd_bytes(d, 50707, FT_T_BYTE, backward, 4);
  snprintf(ucm, sizeof ucm, "%s %s", im->make, im->model);
  ft_ifd_ascii(d, 50708, ucm);
  if((p = ft_ifd_add(d, 50717, FT_T_LONG, 3)) != NULL) { /* WhiteLevel */
    ft_le32(p, 65535);
    ft_le32(p + 4, 65535);
    ft_le32(p + 8, 65535);
  }
  ft_ifd_rationals(d, 50721, FT_T_SRATIONAL, im->xyz_to_rgb, 9, 10000000L);
  ft_ifd_rationals(d, 50728, FT_T_RATIONAL, neutral, 3, 1);
  ft_ifd_short(d, 50778, 21); /* D65 */
}

static int by_tag(const void *a, const void *b) {
  return (int)((const ft_ifd_entry *)a)->tag -
         (int)((const ft_ifd_entry *)b)->tag;
}

unsigned long ft_ifd_layout(ft_ifd *d) {
  unsigned long pos;
  int i;
  qsort(d->e, (size_t)d->n, sizeof d->e[0], by_tag);
  pos = 8 + 2 + 12UL * d->n + 4;
  for(i = 0; i < d->n; i++)
    if(d->e[i].len > 4) {
      d->e[i].off = pos;
      pos += (d->e[i].len + 1) & ~1UL;
    } else {
      d->e[i].off = 8 + 2 + 12UL * i + 8;
    }
  return pos;
}

int ft_ifd_write(const ft_ifd *d, FILE *f) {
  unsigned char hdr[8], ent[12];
  int i, bad = 0;
  memcpy(hdr, "II*\0", 4);
  ft_le32(hdr + 4, 8);
  bad |= fwrite(hdr, 1, 8, f) != 8;
  ft_le16(ent, (unsigned long)d->n);
  bad |= fwrite(ent, 1, 2, f) != 2;
  for(i = 0; i < d->n; i++) {
    memset(ent, 0, sizeof ent);
    ft_le16(ent, d->e[i].tag);
    ft_le16(ent + 2, d->e[i].type);
    ft_le32(ent + 4, d->e[i].count);
    if(d->e[i].len <= 4)
      memcpy(ent + 8, d->e[i].data, d->e[i].len);
    else
      ft_le32(ent + 8, d->e[i].off);
    bad |= fwrite(ent, 1, 12, f) != 12;
  }
  memset(ent, 0, 4);
  bad |= fwrite(ent, 1, 4, f) != 4; /* no next IFD */
  for(i = 0; i < d->n; i++)
    if(d->e[i].len > 4) {
      bad |= fwrite(d->e[i].data, 1, d->e[i].len, f) != d->e[i].len;
      if(d->e[i].len & 1)
        bad |= fputc(0, f) == EOF;
    }
  return bad ? -1 : 0;
}

void ft_ifd_free(ft_ifd *d) {
  int i;
  for(i = 0; i < d->n; i++)
    free(d->e[i].data);
  d->n = 0;
}

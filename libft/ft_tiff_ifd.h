/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libft-internal: one little-endian TIFF IFD, built tag by tag, then laid
 * out (tags ascending, values over 4 bytes after the IFD, word-aligned)
 * and written. Shared by the DNG strip writer (ft_dng.c) and the
 * streaming tiled writer (ft_tiff.c). TIFF 6.0 and DNG 1.4. */
#ifndef FT_TIFF_IFD_H
#define FT_TIFF_IFD_H
#include "ft_image.h"
#include <stddef.h>
#include <stdio.h>

enum {
  FT_T_BYTE = 1,
  FT_T_ASCII = 2,
  FT_T_SHORT = 3,
  FT_T_LONG = 4,
  FT_T_RATIONAL = 5,
  FT_T_UNDEFINED = 7,
  FT_T_SRATIONAL = 10,
  FT_T_DOUBLE = 12
};

#define FT_IFD_MAX_TAGS 40

typedef struct {
  unsigned tag, type;
  unsigned long count;
  unsigned char *data; /* count values, little-endian */
  size_t len;
  unsigned long off; /* after ft_ifd_layout: file offset of the
                        value bytes (inside the entry when
                        len <= 4) */
} ft_ifd_entry;

typedef struct {
  ft_ifd_entry e[FT_IFD_MAX_TAGS];
  int n, oom;
} ft_ifd;

void ft_le16(unsigned char *p, unsigned long v);
void ft_le32(unsigned char *p, unsigned long v);

/* a zeroed entry with room for count values; NULL (and oom set) when full
 * or out of memory. The ifd starts zeroed (memset). */
unsigned char *ft_ifd_add(
    ft_ifd *d, unsigned tag, unsigned type, unsigned long count
);
void ft_ifd_short(ft_ifd *d, unsigned tag, unsigned long v);
void ft_ifd_long(ft_ifd *d, unsigned tag, unsigned long v);
void ft_ifd_ascii(ft_ifd *d, unsigned tag, const char *s);
void ft_ifd_bytes(
    ft_ifd *d, unsigned tag, unsigned type, const unsigned char *b,
    unsigned long n
);
/* signed or unsigned rationals over a fixed denominator */
void ft_ifd_rationals(
    ft_ifd *d, unsigned tag, unsigned type, const double *v, int n, long den
);
/* IEEE 754 doubles, little-endian whatever the host */
void ft_ifd_doubles(ft_ifd *d, unsigned tag, const double *v, int n);
ft_ifd_entry *ft_ifd_find(ft_ifd *d, unsigned tag);

/* The tags every file of ours carries: size, 16-bit samples, no
 * compression, description, make/model, orientation, resolution,
 * software, date; for a DNG also LinearRaw and the DNG set (version,
 * camera, white level, ColorMatrix1 from im, AsShotNeutral 1,1,1, D65).
 * The caller adds photometric (non-DNG) and the pixel layout tags. */
void ft_ifd_common(
    ft_ifd *d, const ft_image *im, int w, int h, int spp, int dng
);

/* sorts the tags, sets every entry's off; returns the first byte after
 * the header, the IFD and the external values (where pixel data starts) */
unsigned long ft_ifd_layout(ft_ifd *d);
/* header + IFD + external values, as laid out; 0 ok, -1 write error */
int ft_ifd_write(const ft_ifd *d, FILE *f);
void ft_ifd_free(ft_ifd *d);

#endif

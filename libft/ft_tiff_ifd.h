/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libft-internal: one little-endian TIFF IFD (image file directory),
 * built tag by tag, then laid out (tags ascending, values over 4 bytes
 * after the IFD, word-aligned) and written. Shared by the DNG strip writer
 * (ft_dng.c) and the streaming tiled writer (ft_tiff.c). TIFF 6.0 and DNG
 * 1.4. */
#ifndef FT_TIFF_IFD_H
#define FT_TIFF_IFD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ft_image.h"

/* The TIFF field types we write. */
enum {
  FT_TIFF_BYTE = 1,
  FT_TIFF_ASCII = 2,
  FT_TIFF_SHORT = 3,
  FT_TIFF_LONG = 4,
  FT_TIFF_RATIONAL = 5,
  FT_TIFF_UNDEFINED = 7,
  FT_TIFF_SRATIONAL = 10,
  FT_TIFF_DOUBLE = 12
};

/* the most tags one IFD holds */
#define FT_IFD_MAX_TAGS 40

/* One tag of the IFD. */
typedef struct ft_ifd_entry {
  /* the tag number */
  uint32_t tag;
  /* its field type, FT_TIFF_* */
  uint32_t type;
  /* values of that type */
  uint32_t count;
  /* the values, little endian */
  uint8_t* data;
  /* bytes in data */
  size_t length;
  /* after ft_lay_out_ifd: the file offset of the value bytes (inside the
   * entry itself when length <= 4) */
  uint32_t offset;
} ft_ifd_entry;

/* An IFD being built. Start it zeroed (memset). */
typedef struct ft_ifd {
  ft_ifd_entry entries[FT_IFD_MAX_TAGS];
  /* entries used */
  uint32_t entry_count;
  /* an add failed: the IFD is full or memory ran out */
  bool is_out_of_memory;
} ft_ifd;

/* Add a tag with room for count values, all zero.
 * Returns the value bytes, or NULL (and is_out_of_memory set) when the
 * IFD is full or memory runs out. */
uint8_t* ft_add_ifd_entry(ft_ifd* ifd, uint32_t tag, uint32_t type,
                          uint32_t count);

/* Add a tag with one SHORT or one LONG value. */
void ft_add_ifd_short(ft_ifd* ifd, uint32_t tag, uint32_t value);
void ft_add_ifd_long(ft_ifd* ifd, uint32_t tag, uint32_t value);

/* Add a text tag (ASCII, NUL included). */
void ft_add_ifd_text(ft_ifd* ifd, uint32_t tag, const char* text);

/* Add a tag of count bytes of the given type. */
void ft_add_ifd_bytes(ft_ifd* ifd, uint32_t tag, uint32_t type,
                      const uint8_t* bytes, uint32_t count);

/* Add count signed (FT_TIFF_SRATIONAL) or unsigned (FT_TIFF_RATIONAL)
 * rationals, each value over the fixed denominator. */
void ft_add_ifd_rationals(ft_ifd* ifd, uint32_t tag, uint32_t type,
                          const double* values, uint32_t count,
                          int32_t denominator);

/* Add count IEEE 754 doubles, little endian whatever the host. */
void ft_add_ifd_doubles(ft_ifd* ifd, uint32_t tag, const double* values,
                        uint32_t count);

/* Find a tag. Returns NULL when the IFD does not have it. */
ft_ifd_entry* ft_find_ifd_entry(ft_ifd* ifd, uint32_t tag);

/* Add the tags every file of ours carries: size, 16-bit samples, no
 * compression, description, make/model, orientation, resolution,
 * software, date; for a DNG also LinearRaw and the DNG set (version,
 * camera, white level, ColorMatrix1 from image, AsShotNeutral 1,1,1,
 * D65). The caller adds photometric (not for a DNG) and the pixel layout
 * tags.
 *
 * samples_per_pixel  1 or 3
 * is_dng             add the DNG tags */
void ft_add_ifd_common_tags(ft_ifd* ifd, const ft_image* image, uint32_t width,
                            uint32_t height, uint32_t samples_per_pixel,
                            bool is_dng);

/* Sort the tags and set every entry's offset.
 * Returns the first byte after the header, the IFD and the values that
 * do not fit in their entries: where the pixel data starts. */
uint32_t ft_lay_out_ifd(ft_ifd* ifd);

/* Write the header, the IFD and the outside values, as laid out.
 * Returns 0, or -1 on a write error. */
int32_t ft_write_ifd(const ft_ifd* ifd, FILE* file);

/* Free every entry's values. */
void ft_free_ifd(ft_ifd* ifd);

#endif

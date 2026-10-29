/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Output formats. Every writer takes the same developed ft_image, so a new
 * format (FlexColor 3F/.fff, TIFF) is one function and one table entry. */
#ifndef FT_OUT_H
#define FT_OUT_H

#include <stdint.h>

#include "ft.h"
#include "ft_image.h"

/* One output format. */
typedef struct ft_writer {
  /* the name picked on the command line */
  const char* name;
  /* the file extension, with the dot */
  const char* extension;
  /* one line saying what it writes */
  const char* description;
  /* write image to path; returns 0, or -1 with error filled */
  int32_t (*write)(const char* path, const ft_image* image, ft_err* error);
} ft_writer;

/* Find a writer by name. Returns NULL when no writer has that name. */
const ft_writer* ft_find_writer(const char* name);

/* All writers; the last entry has name NULL. */
const ft_writer* ft_get_writers(void);

/* Write an 8-bit PNG for a quick look: gamma 2.2 transmittance, not
 * inverted. Returns 0, or -1 with error filled. */
int32_t ft_write_preview_png(const char* path, const ft_image* image,
                             ft_err* error);

/* Write a 16-bit sRGB PNG of a rendered image (linear Rec.2020,
 * ft_render.h) through the output stage (ft_output.h: soft gamut, sRGB
 * curve), without ICC profile: a viewing aid; JPEG exports carry
 * profiles. Returns 0, or -1 with error filled. */
int32_t ft_write_positive_png(const char* path, const ft_image* image,
                              ft_err* error);

#endif

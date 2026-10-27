/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Output formats. Every writer takes the same developed ft_image, so a new
 * format (FlexColor 3F/.fff, TIFF) is one function and one table entry. */
#ifndef FT_OUT_H
#define FT_OUT_H
#include "ft.h"
#include "ft_image.h"

typedef struct {
  const char* name; /* as selected on the command line */
  const char* ext;  /* file extension, with the dot */
  const char* what;
  int (*write)(const char* path, const ft_image* im, ft_err* e);
} ft_writer;

/* NULL if no writer has that name */
const ft_writer* ft_writer_find(const char* name);
/* the table, terminated by an entry with name NULL */
const ft_writer* ft_writers(void);

/* 8-bit PNG for a quick look: gamma 2.2 transmittance, not inverted */
int ft_png_write_preview(const char* path, const ft_image* im, ft_err* e);
/* 16-bit sRGB PNG of a rendered image (linear Rec.2020, ft_render.h)
 * through the output stage (ft_output.h: soft gamut, sRGB curve), no
 * ICC (a viewing aid; JPEG exports carry profiles) */
int ft_png_write_positive(const char* path, const ft_image* im, ft_err* e);

#endif

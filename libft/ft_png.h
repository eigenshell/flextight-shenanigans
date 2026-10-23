/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FT_PNG_H
#define FT_PNG_H
#include "ft.h"

/* 8-bit RGB PNG, uncompressed (stored deflate blocks): no zlib needed.
 * rgb is h rows of w pixels, 3 bytes each. */
int ft_png_write_rgb8(
    const char *path, const unsigned char *rgb, int w, int h, ft_err *e
);
/* the same, 16 bits per sample (rgb: h rows of w pixels, 3 values each) */
int ft_png_write_rgb16(
    const char *path, const unsigned short *rgb, int w, int h, ft_err *e
);

#endif

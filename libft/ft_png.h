/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Write RGB images as PNG files.
 *
 * The image data is stored uncompressed (stored deflate blocks), so no
 * zlib is needed. Pixels go row by row, top to bottom; each pixel is red,
 * green, blue. */
#ifndef FT_PNG_H
#define FT_PNG_H

#include <stdint.h>

#include "ft.h"

/* Write an 8-bit RGB PNG.
 *
 * path    the file to create (overwritten if it exists)
 * pixels  height rows of width pixels, 3 bytes each
 * width   pixels per row, at least 1
 * height  rows, at least 1
 * error   receives the message on failure
 *
 * Returns 0, or -1 when the image is empty, memory runs out or the file
 * cannot be written. */
int32_t ft_write_png_rgb8(const char* path, const uint8_t* pixels,
                          uint32_t width, uint32_t height, ft_err* error);

/* Write a 16-bit RGB PNG; the same as ft_write_png_rgb8, with pixels of
 * three 16-bit values. */
int32_t ft_write_png_rgb16(const char* path, const uint16_t* pixels,
                           uint32_t width, uint32_t height, ft_err* error);

#endif

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Baseline JPEG encoder. Pure (browser tier: heap, no files):
 * ft_jpeg_encode writes into memory; ft_jpeg_write (ft_jpeg_io.c) puts
 * that in a file. The bytes are what libjpeg-turbo writes with the same
 * settings (test_jpeg_ref pins it), so the page and ftdev make identical
 * JPEGs without libft linking libjpeg. */
#ifndef FT_JPEG_H
#define FT_JPEG_H
#include <stddef.h>
#include "ft.h"

#define FT_JPEG_QUALITY 90

/* rgb: h rows of w pixels, 3 bytes each (encoded, ft_output_quantise8).
 * quality 1..100 (the IJG tables scaled as jpeg_set_quality, baseline);
 * 4:4:4 (no chroma subsampling: film grain and fine colour survive),
 * optimised Huffman tables; the ICC profile (icc, icc_n bytes; NULL for
 * none) in APP2; JFIF density ppi dots per inch (0: none); comment (NULL
 * or "" for none, at most FT_JPEG_COMMENT_MAX bytes) in a COM marker:
 * ftdev export puts the resolved recipe there. */
#define FT_JPEG_COMMENT_MAX 65533
/* the JPEG in a malloc'd buffer: *out (the caller frees it), *n bytes.
 * -1 with e filled (*out NULL, *n 0) on bad arguments or no memory. */
int ft_jpeg_encode(const unsigned char *rgb, int w, int h, int quality,
                   double ppi, const unsigned char *icc, size_t icc_n,
                   const char *comment, unsigned char **out, size_t *n,
                   ft_err *e);
/* the same into the file path (removed again on failure) */
int ft_jpeg_write(const char *path, const unsigned char *rgb, int w, int h,
                  int quality, double ppi, const unsigned char *icc,
                  size_t icc_n, const char *comment, ft_err *e);

#endif

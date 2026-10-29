/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Baseline JPEG encoder. Pure (heap, no files), so it runs in the
 * browser: ft_encode_jpeg writes into memory; ft_write_jpeg
 * (ft_jpeg_io.c) puts that in a file. The bytes are what libjpeg-turbo
 * writes with the same settings (test_jpeg_ref pins it), so Quixote and
 * ftdev make identical JPEGs without libft linking libjpeg. */
#ifndef FT_JPEG_H
#define FT_JPEG_H

#include <stddef.h>
#include <stdint.h>

#include "ft.h"

/* the quality our exports use */
#define FT_JPEG_QUALITY 90
/* the longest comment one COM marker holds */
#define FT_JPEG_COMMENT_MAX_BYTES 65533

/* Encode an image as a baseline JPEG: 4:4:4 (no chroma subsampling, so
 * film grain and fine colour survive), optimised Huffman tables.
 *
 * pixels              height rows of width pixels, 3 bytes each (encoded,
 *                     ft_quantise_output8)
 * quality             1..100: the IJG tables scaled as jpeg_set_quality
 *                     does, baseline
 * ppi                 the JFIF density in dots per inch; 0 = none
 * icc_profile         an ICC profile for APP2, icc_profile_length bytes;
 *                     NULL = none
 * comment             text for a COM marker (ftdev export puts the
 *                     resolved recipe there); NULL or "" = none, at most
 *                     FT_JPEG_COMMENT_MAX_BYTES
 * jpeg, jpeg_length   receive the JPEG in a malloc'd buffer; the caller
 *                     frees it
 *
 * Returns 0, or -1 with error filled (*jpeg NULL, *jpeg_length 0) on bad
 * arguments or no memory. */
int32_t ft_encode_jpeg(const uint8_t* pixels, uint32_t width, uint32_t height,
                       int32_t quality, double ppi, const uint8_t* icc_profile,
                       size_t icc_profile_length, const char* comment,
                       uint8_t** jpeg, size_t* jpeg_length, ft_err* error);

/* The same as ft_encode_jpeg, into the file path (removed again on
 * failure). Returns 0, or -1 with error filled. */
int32_t ft_write_jpeg(const char* path, const uint8_t* pixels, uint32_t width,
                      uint32_t height, int32_t quality, double ppi,
                      const uint8_t* icc_profile, size_t icc_profile_length,
                      const char* comment, ft_err* error);

#endif

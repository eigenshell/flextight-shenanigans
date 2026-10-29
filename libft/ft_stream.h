/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Develop one line at a time (sub-project 2b): the stages of ft_develop
 * before the crop (flat-field, defects, RGB alignment) with only a ring of
 * 2k + 1 lines in memory (k = 12 / bin, the RGB row offset), so a scan is
 * developed while it arrives. Output line n is exactly line n of
 * ft_develop's image before its crop (the same functions, line by line),
 * for scans longer than 2k lines. */
#ifndef FT_STREAM_H
#define FT_STREAM_H

#include <stddef.h>
#include <stdint.h>

#include "ft.h"
#include "ft_develop.h"

/* A developer for one scan. Its fields are private to ft_stream.c. */
typedef struct ft_stream ft_stream;

/* Start developing a scan.
 *
 * pixels              pixels per line
 * calibration         calibration_lines x pixels x 3 (the pass's
 *                     calibration lines)
 * faulty_dump         the Faul Pixs dump, faulty_dump_length bytes; or NULL
 * options             first pixel, bin and defects (the crop is ignored)
 *
 * Returns the developer, or NULL with error filled on bad input, a
 * calibration that does not split, too many defects or no memory. */
ft_stream* ft_open_stream(uint32_t pixels, const uint16_t* calibration,
                          uint32_t calibration_lines, const char* faulty_dump,
                          size_t faulty_dump_length,
                          const ft_develop_options* options, ft_err* error);

/* The RGB row offset k: the first output line comes with raw line 2k. */
uint32_t ft_get_stream_row_offset(const ft_stream* stream);

/* Develop one raw line (pixels x 3 values).
 *
 * output  receives pixels x 3 transmittance values when a line is ready
 *
 * Returns 1 with output filled, 0 while the first 2k lines fill the
 * ring, or -1 with error filled. */
int32_t ft_push_stream_line(ft_stream* stream, const uint16_t* raw,
                            float* output, ft_err* error);

/* Free the developer. Safe to call with NULL. */
void ft_close_stream(ft_stream* stream);

#endif

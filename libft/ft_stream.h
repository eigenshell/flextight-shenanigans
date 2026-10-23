/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Develop one line at a time (sub-project 2b): the stages of ft_develop
 * before the crop (flat-field, defects, RGB alignment) with only a ring of
 * 2k + 1 lines in memory (k = 12 / bin, the tri-linear row offset), so a
 * scan is developed while it arrives. Output line n is exactly line n of
 * ft_develop's image before its crop (the same functions, line by line),
 * for scans longer than 2k lines. */
#ifndef FT_STREAM_H
#define FT_STREAM_H
#include "ft.h"
#include "ft_develop.h"
#include <stddef.h>

typedef struct ft_stream ft_stream;

/* px: pixels per line; cal: cal_lines x px x 3 (the pass's calibration
 * lines); faul: the Faul Pixs dump or NULL; o: x, bin, defects (its crop
 * is ignored). NULL with e filled on bad input, a calibration that does
 * not split, too many defects or out of memory. */
ft_stream *ft_stream_open(
    int px, const unsigned short *cal, int cal_lines, const char *faul,
    size_t faul_n, const ft_develop_opts *o, ft_err *e
);
/* tri-linear offset k: the first output line comes with raw line 2k */
int ft_stream_k(const ft_stream *s);
/* one raw line (px x 3) in. 1 with out (px x 3 transmittance) filled,
 * 0 while the first 2k lines fill the ring, -1 with e filled (out of
 * memory). */
int ft_stream_push(
    ft_stream *s, const unsigned short *raw, float *out, ft_err *e
);
void ft_stream_close(ft_stream *s);

#endif

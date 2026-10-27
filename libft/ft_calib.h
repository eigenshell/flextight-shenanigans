/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host-side CCD correction, ported from tools/poc/ftraw.py. Images are
 * lines x px x 3 (RGB interleaved, line-major). */
#ifndef FT_CALIB_H
#define FT_CALIB_H
#include <stddef.h>

#include "ft.h"

#define FT_FAUL_MAX 64

/* dark/white lines: line mean within this share of the range from either
 * extreme. Tight, because the lamp's on/off transition lines can be half
 * lit (seen at bin 1, ~8 % of the range): those are neither. */
#define FT_CAL_BAND 0.02
/* dark/white reference per pixel and channel (px*3 floats each) from the
 * 250 .cal lines, classified by level (within FT_CAL_BAND of either
 * extreme) */
int ft_split_cal(const unsigned short* cal, int lines, int px, float* dark,
                 float* white, ft_err* e);
/* per-channel noise of the white reference in transmittance units: each
 * pixel's variance over the white lines / (white - dark)^2, averaged over
 * the line (the weights of a mono b/w file, ft_density.h) */
int ft_white_var(const unsigned short* cal, int lines, int px, double var[3],
                 ft_err* e);
/* transmittance 0..1 into t (lines*px*3 floats) */
void ft_flatfield(const unsigned short* raw, int lines, int px,
                  const float* dark, const float* white, float* t);
/* +999 entries of a Faul Pixs dump per channel; returns their number */
int ft_faul_parse(const char* text, size_t n, int pix[3][FT_FAUL_MAX],
                  int count[3]);
/* interpolate output pixels whose CCD span starts at a flagged pixel;
 * returns -1 on bin < 1 or out of memory, else 0. pix is read-only but not
 * const: C99 forbids int(*)[N] -> const int(*)[N]. */
int ft_faul_fix(float* t, int lines, int px, int pix[3][FT_FAUL_MAX],
                const int count[3], int x, int bin);
/* realign the tri-linear rows in place (k = 12 / bin); returns new lines */
int ft_align_rgb(float* t, int lines, int px, int k);

#endif

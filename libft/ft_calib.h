/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host-side CCD correction, ported from tools/poc/ftraw.py.
 *
 * Images are lines x pixels x 3: RGB interleaved, one line after the
 * other. */
#ifndef FT_CALIB_H
#define FT_CALIB_H

#include <stddef.h>
#include <stdint.h>

#include "ft.h"

/* the most faulty pixels kept per channel */
#define FT_FAULTY_PIXEL_MAX 64

/* A calibration line counts as dark or white when its mean is within
 * this share of the range from the darkest or brightest line. Tight,
 * because the lamp's on/off transition lines can be half lit (seen at
 * bin 1, ~8 % of the range): those are neither. */
#define FT_CALIBRATION_BAND 0.02

/* Average the dark and white reference from the .cal lines (250 of
 * them), each line classified by its level.
 *
 * calibration  lines x pixels x 3 raw values
 * dark, white  receive pixels x 3 averages each
 *
 * Returns 0, or -1 when there are fewer than 10 dark or 10 white lines,
 * the size is 0, or memory runs out. */
int32_t ft_split_calibration(const uint16_t* calibration, uint32_t lines,
                             uint32_t pixels, float* dark, float* white,
                             ft_err* error);

/* The noise of the white reference per channel, in transmittance units:
 * each pixel's variance over the white lines / (white - dark)^2,
 * averaged over the line (the weights of a mono b/w file, ft_density.h).
 *
 * variance  receives the red, green and blue noise
 *
 * Returns 0, or -1 as ft_split_calibration. */
int32_t ft_measure_white_variance(const uint16_t* calibration, uint32_t lines,
                                  uint32_t pixels, double variance[3],
                                  ft_err* error);

/* Turn raw values into transmittance 0..1 with the dark and white
 * reference.
 *
 * raw            lines x pixels x 3 raw values
 * transmittance  receives lines x pixels x 3 values */
void ft_apply_flat_field(const uint16_t* raw, uint32_t lines, uint32_t pixels,
                         const float* dark, const float* white,
                         float* transmittance);

/* Read the +999 entries of a "Faul Pixs" dump, per channel.
 *
 * text         the dump
 * text_length  its length in bytes
 * faulty       receives the CCD pixel of each entry
 * count        receives the entries per channel
 *
 * Returns the entries in all channels together. */
uint32_t ft_parse_faulty_pixels(const char* text, size_t text_length,
                                int32_t faulty[3][FT_FAULTY_PIXEL_MAX],
                                uint32_t count[3]);

/* Interpolate the output pixels whose CCD span starts at a faulty pixel,
 * from the good pixels left and right of them.
 *
 * transmittance  lines x pixels x 3 values, repaired in place
 * faulty, count  from ft_parse_faulty_pixels. faulty is only read, but
 *                not const: C99 does not convert int32_t(*)[N] to
 *                const int32_t(*)[N].
 * window_start   the output pixel the scan line starts at
 * bin            CCD pixels per output pixel
 *
 * Returns 0, or -1 when bin or pixels is 0 or memory runs out. */
int32_t ft_repair_faulty_pixels(float* transmittance, uint32_t lines,
                                uint32_t pixels,
                                int32_t faulty[3][FT_FAULTY_PIXEL_MAX],
                                const uint32_t count[3], int32_t window_start,
                                uint32_t bin);

/* Realign the CCD's three colour rows in place. The rows sit apart on the
 * CCD, so red arrives row_offset lines after green and 2 x row_offset
 * after blue (row_offset = 12 / bin).
 *
 * Returns the lines left: lines - 2 x row_offset, or lines unchanged
 * when row_offset is 0 or there are too few lines. */
uint32_t ft_align_rgb_rows(float* transmittance, uint32_t lines,
                           uint32_t pixels, uint32_t row_offset);

#endif

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The a-trous wavelet engine (B3 spline, Starck & Murtagh), one float plane
 * at a time, shared by the sensor and the grain NR (ft_nr.h).
 *
 * c_0 = in, c_{j+1} = c_j smoothed by [1 4 6 4 1]/16 with holes 2^j
 * (separable, mirrored edges), detail w_j = c_j - c_{j+1}, 5 scales. The
 * shrinkage run returns c_5 + sum_j soft(w_j, k * sigma * F_j * weights_j):
 * sigma is the noise sigma of the plane, F_j the std of w_j for unit-sigma
 * white noise (exact values from the kernel), weights_j the scale weights
 * (NULL = FT_ATROUS_DEFAULT_WEIGHTS, which leave larger structures and
 * gradients alone).
 *
 * A band of rows reads its input with a +-FT_ATROUS_HALO_ROWS halo and is
 * bit-identical to the same rows of a whole-image run. Planes smaller than
 * FT_ATROUS_MIN_SIDE in either direction pass through (copied, sanitised).
 * Pure C99: no I/O, no allocation (the browser runs the same code). */
#ifndef FT_ATROUS_H
#define FT_ATROUS_H

#include <stddef.h>
#include <stdint.h>

/* the detail scales */
#define FT_ATROUS_SCALES 5
/* the rows a band reads past each end: the support of c_5 is +-62 */
#define FT_ATROUS_HALO_ROWS 64
/* the smallest plane the shrink works on: one mirror covers the widest
 * hole */
#define FT_ATROUS_MIN_SIDE 33

/* the std of w_j for unit-sigma white noise */
extern const double FT_ATROUS_NOISE_STD[FT_ATROUS_SCALES];
/* the default scale weights (tune on hardware scans) */
extern const double FT_ATROUS_DEFAULT_WEIGHTS[FT_ATROUS_SCALES];

/* The scratch for a run over band_height rows of a width-wide plane (a
 * whole-image run: band_height = height), in bytes. */
size_t ft_get_atrous_scratch_bytes(uint32_t width, uint32_t band_height);

/* One smoothing step at scale (holes 2^scale): output = c_{j+1} from
 * input = c_j, width x height, mirrored edges. output must not alias
 * input; row holds width floats. */
void ft_smooth_atrous(const float* input, uint32_t width, uint32_t height,
                      uint32_t scale, float* output, float* row);

/* The shrinkage run on rows [first_row, end_row) of a width x height
 * plane: output receives those rows (at their own offsets in a width x
 * height plane); input is read with the halo. NaN and inf in the input
 * count as 0. output must not alias input.
 *
 * k        the threshold in noise sigmas
 * sigma    the plane's noise sigma
 * weights  the scale weights; NULL = FT_ATROUS_DEFAULT_WEIGHTS */
void ft_shrink_atrous_band(const float* input, uint32_t width, uint32_t height,
                           int32_t first_row, int32_t end_row, float* output,
                           double k, double sigma, const double* weights,
                           void* scratch);

/* The same over the whole plane; output may equal input. */
void ft_shrink_atrous(const float* input, uint32_t width, uint32_t height,
                      float* output, double k, double sigma,
                      const double* weights, void* scratch);

#endif

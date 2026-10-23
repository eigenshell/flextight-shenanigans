/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The a-trous wavelet engine (B3 spline, Starck & Murtagh), one float plane
 * at a time, shared by the sensor and the grain NR (ft_nr.h).
 *
 * c_0 = in, c_{j+1} = c_j smoothed by [1 4 6 4 1]/16 with holes 2^j
 * (separable, mirrored edges), detail w_j = c_j - c_{j+1}, 5 scales. The
 * shrinkage run returns c_5 + sum_j soft(w_j, k * sigma * F_j * wts_j):
 * sigma is the noise sigma of the plane, F_j the std of w_j for unit-sigma
 * white noise (exact values from the kernel), wts_j the scale weights
 * (NULL = FT_ATROUS_W, which leave larger structures and gradients alone).
 *
 * A band of rows reads its input with a +-FT_ATROUS_HALO-row halo and is
 * bit-identical to the same rows of a whole-image run. Planes smaller than
 * FT_ATROUS_MIN in either direction pass through (copied, sanitised).
 * Pure C99: no I/O, no allocation (the browser runs the same code). */
#ifndef FT_ATROUS_H
#define FT_ATROUS_H
#include <stddef.h>

#define FT_ATROUS_SCALES 5
#define FT_ATROUS_HALO 64 /* the support of c_5: +-62 rows */
#define FT_ATROUS_MIN 33  /* one mirror covers the widest hole */

/* std of w_j for unit-sigma white noise */
extern const double FT_ATROUS_F[FT_ATROUS_SCALES];
/* the default scale weights (tune on hardware scans) */
extern const double FT_ATROUS_W[FT_ATROUS_SCALES];

/* scratch for a run over band_h rows of a w-wide plane (a whole-image run:
 * band_h = h) */
size_t ft_atrous_scratch_bytes(int w, int band_h);

/* one smoothing step at scale j (holes 2^j): out = c_{j+1} from in = c_j,
 * w x h, mirrored edges. out must not alias in; row: w floats. */
void ft_atrous_smooth(
    const float *in, int w, int h, int j, float *out, float *row
);

/* the shrinkage run on rows [y0, y1) of a w x h plane: out receives those
 * rows (at their own offsets in a w x h plane), in is read with the halo.
 * NaN/inf in the input count as 0. out must not alias in. */
void ft_atrous_band(
    const float *in, int w, int h, int y0, int y1, float *out, double k,
    double sigma, const double *wts, void *scratch
);
/* the whole plane; out may equal in */
void ft_atrous(
    const float *in, int w, int h, float *out, double k, double sigma,
    const double *wts, void *scratch
);

#endif

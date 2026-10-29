/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Noise reduction over linear transmittance, two stages, both T -> T (the
 * render core's per-pixel LUTs cannot express neighbourhoods, so NR runs
 * before it).
 *
 *   ft_reduce_sensor_noise  calibration-driven: variance_c(T) = S_c T + O_c
 * (the frame's NoiseProfile, ft_noise_model.h). The generalised Anscombe
 * transform stabilises it to sigma 1, an orthonormal opponent split keeps that
 * (Y = (R+G+B)/sqrt3, C1 = (R-B)/sqrt2, C2 = (R-2G+B)/sqrt6), the a-trous
 *                 shrink runs on Y at k and on C1, C2 at kc, then the
 *                 opponent and the transform are inverted. The inverse is
 *                 the closed-form approximation of the exact unbiased
 *                 inverse (Makitalo & Foi, IEEE TIP 20(9) 2011 and 22(1)
 *                 2013): applied to a denoised value it returns the mean,
 *                 where the algebraic inverse is biased low at low counts
 *                 and would lighten dense negatives.
 *   ft_reduce_grain   film grain, image structure (default off): density
 *                 D = -log10(max(T, 1e-5)) per channel, the same opponent
 *                 split, the grain sigma of Y measured on the frame
 *                 (ft_measure_noise_sigma), Y shrunk at k, chroma at k / 2. The
 * film base needs no subtracting: a constant density changes no detail scale,
 * only c_5, and comes back on the way out.
 *
 * k = 0 returns the image untouched. Planes below FT_ATROUS_MIN_SIDE pass
 * through. Pure C99 (no I/O, no allocation): the caller provides
 * ft_get_noise_reduction_scratch_bytes(width, height) bytes, 5 float
 * planes. */
#ifndef FT_NR_H
#define FT_NR_H

#include <stddef.h>
#include <stdint.h>

/* The scratch the noise reduction needs, in bytes. */
size_t ft_get_noise_reduction_scratch_bytes(uint32_t width, uint32_t height);

/* The generalised Anscombe transform of count values: z = 2/S sqrt(S y +
 * 3/8 S^2 + O); S <= 0 is the Gaussian case z = y / sqrt(O). */
void ft_stabilise_variance(const float* values, size_t count, double slope,
                           double offset, float* stabilised);

/* Its closed-form unbiased inverse, for denoised values. */
void ft_unstabilise_variance(const float* stabilised, size_t count,
                             double slope, double offset, float* values);

/* The noise sigma of a width x height plane: MAD / 0.6745 of the finest
 * a-trous scale over the flattest quarter of its 32 x 32 blocks (by the
 * energy of scale 2, so edges and texture do not count), divided by
 * FT_ATROUS_NOISE_STD[0]. scratch: 4 planes. */
double ft_measure_noise_sigma(const float* plane, uint32_t width,
                              uint32_t height, void* scratch);

/* A constant-noise profile measured on the image (S = 0, O = sigma^2 per
 * channel), for frames without a NoiseProfile. */
void ft_estimate_noise_profile(const float* pixels, uint32_t width,
                               uint32_t height, double profile[6],
                               void* scratch);

/* The grain sigma of the Y opponent plane of the density. */
double ft_measure_grain_sigma(const float* pixels, uint32_t width,
                              uint32_t height, void* scratch);

/* Reduce sensor noise in place (RGB interleaved).
 *
 * profile   S,O per channel
 * k         the luminance threshold in sigmas
 * k_chroma  the chroma threshold: k x nr_chroma */
void ft_reduce_sensor_noise(float* pixels, uint32_t width, uint32_t height,
                            const double profile[6], double k, double k_chroma,
                            void* scratch);

/* Reduce film grain in place. */
void ft_reduce_grain(float* pixels, uint32_t width, uint32_t height, double k,
                     void* scratch);

#endif

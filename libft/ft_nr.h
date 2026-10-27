/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Noise reduction over linear transmittance, two stages, both T -> T (the
 * render core's per-pixel LUTs cannot express neighbourhoods, so NR runs
 * before it).
 *
 *   ft_nr_sensor  calibration-driven: var_c(T) = S_c T + O_c (the frame's
 *                 NoiseProfile, ft_noise_model.h). The generalised
 *                 Anscombe transform stabilises it to sigma 1, an
 *                 orthonormal opponent split keeps that (Y = (R+G+B)/sqrt3,
 *                 C1 = (R-B)/sqrt2, C2 = (R-2G+B)/sqrt6), the a-trous
 *                 shrink runs on Y at k and on C1, C2 at kc, then the
 *                 opponent and the transform are inverted. The inverse is
 *                 the closed-form approximation of the exact unbiased
 *                 inverse (Makitalo & Foi, IEEE TIP 20(9) 2011 and 22(1)
 *                 2013): applied to a denoised value it returns the mean,
 *                 where the algebraic inverse is biased low at low counts
 *                 and would lighten dense negatives.
 *   ft_nr_grain   film grain, image structure (default off): density
 *                 D = -log10(max(T, 1e-5)) per channel, the same opponent
 *                 split, the grain sigma of Y measured on the frame
 *                 (ft_nr_sigma), Y shrunk at k, chroma at k / 2. The film
 *                 base needs no subtracting: a constant density changes no
 *                 detail scale, only c_5, and comes back on the way out.
 *
 * k = 0 returns the image untouched. Planes below FT_ATROUS_MIN pass
 * through. Pure C99 (no I/O, no allocation): the caller provides
 * ft_nr_scratch_bytes(w, h) bytes, 5 float planes. */
#ifndef FT_NR_H
#define FT_NR_H
#include <stddef.h>

size_t ft_nr_scratch_bytes(int w, int h);

/* the generalised Anscombe transform, n values: z = 2/S sqrt(S y +
 * 3/8 S^2 + O); S <= 0 is the Gaussian case z = y / sqrt(O) */
void ft_nr_vst(const float* y, size_t n, double s, double o, float* z);
/* its closed-form unbiased inverse, for denoised z */
void ft_nr_vst_inv(const float* z, size_t n, double s, double o, float* y);

/* the noise sigma of a w x h plane: MAD / 0.6745 of the finest a-trous
 * scale over the flattest quarter of its 32 x 32 blocks (by the energy of
 * scale 2, so edges and texture do not count), divided by FT_ATROUS_F[0].
 * scratch: 4 planes. */
double ft_nr_sigma(const float* plane, int w, int h, void* scratch);

/* a constant-noise profile measured on the image (S = 0, O = sigma^2 per
 * channel), for frames without a NoiseProfile */
void ft_nr_estimate(const float* rgb, int w, int h, double prof[6],
                    void* scratch);
/* the grain sigma of the Y opponent plane of the density */
double ft_nr_grain_sigma(const float* rgb, int w, int h, void* scratch);

/* in place, rgb interleaved; prof = S,O per channel; kc = k * nr_chroma */
void ft_nr_sensor(float* rgb, int w, int h, const double prof[6], double k,
                  double kc, void* scratch);
/* in place */
void ft_nr_grain(float* rgb, int w, int h, double k, void* scratch);

#endif

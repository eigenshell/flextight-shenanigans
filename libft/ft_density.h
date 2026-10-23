/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Encoders for the detailed-scan files (sub-project 2b). Pure: libm and
 * mem* only, no allocation, so the browser preview runs the very code the
 * writers run (WASM) and the two can never disagree.
 *
 * Negative files (A) store transmittance as u16: round(T x 65535), the
 * rounding ft_dng_write uses.
 *
 * Archive positives (B) store density above the film base, per channel,
 * as Kodak's Cineon stored printing density: code = round((D - D_base +
 * FT_DENS_OFFSET) / FT_DENS_RANGE x 65535), D = -log10(T), clamped to
 * 0..65535. Denser film (a brighter scene) is a higher code, so the file
 * is a flat, log-looking positive with the mask removed. The base sits at
 * code 1638, not 0, so noise thinner than the base is kept (Cineon put it
 * at 95 of 1023 for the same reason). B is computed by table from the
 * same u16 the negative stores, so a B file is a pure function of the A
 * file of the same scan. */
#ifndef FT_DENSITY_H
#define FT_DENSITY_H
#include <stddef.h>

#define FT_DENS_OFFSET 0.1 /* D: the film base's place above 0 */
#define FT_DENS_RANGE 4.0  /* D: codes 0..65535 span this */

/* one table per channel: 3 x 128 KB, keep it off the stack */
typedef struct {
  unsigned short lut[3][65536];
  double base[3];
} ft_dens;

/* T (0..1) -> u16 as the negative files store it; NaN -> 0 */
void ft_dens_t16(const float *t, unsigned short *out, size_t n);
/* tables for film base densities base[3] (each clamped to 0..4.5) */
void ft_dens_init(ft_dens *d, const double base[3]);
/* colour: npx RGB pixels of u16 transmittance -> RGB codes */
void ft_dens_rgb(
    const ft_dens *d, const unsigned short *t16, unsigned short *out, size_t npx
);
/* b/w mono: one code per pixel, sum(w[c] x code[c]) rounded; w from
 * ft_dens_weights (sums to 1). The codes are affine in D - D_base with
 * one scale, so this is the weighted mean of the base-corrected
 * densities, encoded (up to rounding and the clamps). */
void ft_dens_mono(
    const ft_dens *d, const double w[3], const unsigned short *t16,
    unsigned short *out, size_t npx
);
/* inverse-noise weights from per-channel variances, normalised to sum
 * to 1; a variance <= 0 or not finite gives equal weights */
void ft_dens_weights(const double var[3], double w[3]);
/* a code back to D above the base (D - D_base) */
double ft_dens_code_to_d(unsigned code);

#endif

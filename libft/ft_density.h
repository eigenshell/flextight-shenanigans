/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Encoders for the detailed-scan files (sub-project 2b). Pure: libm and
 * mem* only, no allocation, so Quixote runs the very code the
 * writers run (WASM) and the two can never disagree.
 *
 * Negative files (A) store transmittance as uint16_t: round(T x 65535), the
 * rounding ft_dng_write uses.
 *
 * Archive positives (B) store density above the film base, per channel,
 * as Kodak's Cineon stored printing density: code = round((D - D_base +
 * FT_DENSITY_OFFSET) / FT_DENSITY_RANGE x 65535), D = -log10(T), clamped to
 * 0..65535. Denser film (a brighter scene) is a higher code, so the file
 * is a flat, log-looking positive with the mask removed. The base sits at
 * code 1638, not 0, so noise thinner than the base is kept (Cineon put it
 * at 95 of 1023 for the same reason). B is computed by table from the
 * same uint16_t the negative stores, so a B file is a pure function of the A
 * file of the same scan. */
#ifndef FT_DENSITY_H
#define FT_DENSITY_H

#include <stddef.h>
#include <stdint.h>

/* density: the film base's place above code 0 */
#define FT_DENSITY_OFFSET 0.1
/* density: the codes 0..65535 span this */
#define FT_DENSITY_RANGE 4.0

/* The tables that turn stored transmittance into density codes. One
 * table per channel: 3 x 128 KB, so keep it off the stack. */
typedef struct ft_density_encoder {
  /* the code for each stored transmittance, per channel */
  uint16_t tables[3][65536];
  /* the film base density per channel, clamped to 0..4.5 */
  double base_density[3];
} ft_density_encoder;

/* Transmittance (0..1) -> uint16_t as the negative files store it; NaN
 * gives 0. */
void ft_quantise_transmittance16(const float* transmittance, uint16_t* stored,
                                 size_t count);

/* Build the tables for the film base densities (each clamped to
 * 0..4.5). */
void ft_init_density_encoder(ft_density_encoder* encoder,
                             const double base_density[3]);

/* Colour: pixel_count RGB pixels of stored transmittance -> RGB codes. */
void ft_encode_density_rgb(const ft_density_encoder* encoder,
                           const uint16_t* stored, uint16_t* codes,
                           size_t pixel_count);

/* Black and white mono: one code per pixel, sum(weights[c] x code[c])
 * rounded; weights from ft_compute_noise_weights (they sum to 1). The
 * codes are affine in D - D_base with one scale, so this is the weighted
 * mean of the base-corrected densities, encoded (up to rounding and the
 * clamps). */
void ft_encode_density_mono(const ft_density_encoder* encoder,
                            const double weights[3], const uint16_t* stored,
                            uint16_t* codes, size_t pixel_count);

/* Inverse-noise weights from per-channel variances, normalised to sum to
 * 1. A variance that is <= 0 or not finite gives equal weights. */
void ft_compute_noise_weights(const double variance[3], double weights[3]);

/* A code back to density above the base (D - D_base). */
double ft_get_density_from_code(uint32_t code);

#endif

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Output sharpening of the resized image (linear Rec.2020), luminance
 * only, in perceptual lightness Lp = cbrt(Y), so one threshold spares
 * grain alike in shadows and highlights:
 *
 *   blur: Gaussian, sigma r = 0.5 + 0.3 min(1, 1/scale - 1) px (0.5 at
 *         scale 1, up to 0.8), half-width ceil(3 r), mirrored edges
 *   d = soft(Lp - blur, FT_SHARPEN_THRESHOLD); Lp' = Lp + amount d
 *   Lp' clamped to the 3 x 3 min/max of Lp widened by FT_SHARPEN_CLAMP of
 *   their range (no halos)
 *   RGB *= (Lp' / Lp)^3: hue and saturation stay, neutrals stay neutral
 *
 * amount 0 returns the image untouched; pixels with Lp <= 1e-6
 * (Y <= 1e-18) are left alone; NaN/inf -> 0 otherwise. Pure C99 (no I/O, no
 * allocation). */
#ifndef FT_SHARPEN_H
#define FT_SHARPEN_H

#include <stddef.h>
#include <stdint.h>

/* the soft threshold on Lp - blur (tune on hardware scans) */
#define FT_SHARPEN_THRESHOLD 0.004
/* the clamp widens the 3 x 3 min/max by this share of their range */
#define FT_SHARPEN_CLAMP 0.10

/* The scratch memory ft_sharpen needs, in bytes. */
size_t ft_get_sharpen_scratch_bytes(uint32_t width, uint32_t height);

/* Sharpen in place.
 *
 * pixels  width x height RGB values, interleaved
 * scale   the resize scale that made the image (<= 0 or > 1 count as 1) */
void ft_sharpen(float* pixels, uint32_t width, uint32_t height, double amount,
                double scale, void* scratch);

#endif

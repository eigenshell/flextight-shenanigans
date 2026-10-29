/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Linear DNG writer: uncompressed 16-bit LinearRaw RGB (DNG 1.4), the
 * image's transmittance scaled to 0..65535, with ColorMatrix1 from the
 * image (D65) and an as-shot neutral of 1,1,1: the open beam is white,
 * since the flat-field already divided the lamp out.
 *
 * The reader takes back exactly that subset (the share pipeline's working
 * raw): little-endian, LinearRaw, 3 x 16-bit chunky, uncompressed strips.
 * Anything else is refused with the reason. The file pair lives in
 * ft_dng_io.c; the rest is pure, so it runs in the browser. */
#ifndef FT_DNG_H
#define FT_DNG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ft.h"
#include "ft_image.h"

/* Write image as a DNG. Returns 0, or -1 with error filled. */
int32_t ft_write_dng(const char* path, const ft_image* image, ft_err* error);

/* A sample as the file stores it: 0..1 -> 0..65535, rounded. */
uint16_t ft_quantise_dng_sample(float value);

/* Is this NoiseProfile (6 values) one the writer keeps: finite, S >= 0,
 * O > 0? */
bool ft_is_noise_profile_usable(const double* noise_profile);

/* Change image, in place, to what ft_read_dng(ft_write_dng(image)) gives
 * back: samples through 16 bits, ppi through the resolution rational,
 * an unusable noise profile dropped. Pure: Quixote's stand-in for the
 * working raw `ftdev cut` writes. */
void ft_simulate_dng_storage(ft_image* image);

/* Read a DNG from memory.
 *
 * image  ft_init_image'd by the caller. Receives the pixels (allocated
 *        here, samples / 65535), ppi from XResolution, ColorMatrix1 into
 *        xyz_to_rgb, the noise profile, description, make and model.
 *
 * Returns 0, or -1 with error filled; image has no pixels then. */
int32_t ft_parse_dng(const uint8_t* bytes, size_t length, ft_image* image,
                     ft_err* error);

/* The same as ft_parse_dng, from a file. */
int32_t ft_read_dng(const char* path, ft_image* image, ft_err* error);

#endif

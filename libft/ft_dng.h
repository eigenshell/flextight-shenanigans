/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Linear DNG writer: uncompressed 16-bit LinearRaw RGB (DNG 1.4), the
 * image's transmittance scaled to 0..65535, with ColorMatrix1 from the
 * image (D65) and an as-shot neutral of 1,1,1: the open beam is white,
 * since the flat-field already divided the lamp out.
 * The reader takes back exactly that subset (the share pipeline's working
 * raw): little-endian, LinearRaw, 3 x 16-bit chunky, uncompressed strips.
 * Anything else is refused with the reason. The file pair lives in
 * ft_dng_io.c; the rest is pure (browser tier). */
#ifndef FT_DNG_H
#define FT_DNG_H
#include "ft.h"
#include "ft_image.h"
#include <stddef.h>

int ft_dng_write(const char *path, const ft_image *im, ft_err *e);
/* a sample as the file stores it (0..65535) */
unsigned ft_dng_q16(float v);
/* a NoiseProfile (6 values) the writer keeps: finite, S >= 0, O > 0 */
int ft_dng_noise_ok(const double *v);
/* im as ft_dng_read(ft_dng_write(im)) gives it back, in place: samples
 * through 16 bits, ppi through the resolution rational, an unusable
 * noise profile dropped. Pure: the page's stand-in for the working raw
 * ftdev cut writes. */
void ft_dng_as_stored(ft_image *im);
/* im: ft_image_init'd by the caller; pixels allocated here (/65535),
 * ppi from XResolution, ColorMatrix1 into xyz_to_rgb, description, make
 * and model. On failure im has no pixels. */
int ft_dng_parse(const unsigned char *b, size_t n, ft_image *im, ft_err *e);
int ft_dng_read(const char *path, ft_image *im, ft_err *e);

#endif

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ICC profiles for the output spaces (ft_output.h), built from the same
 * published primaries: version 2.1 display profiles (class mntr, RGB,
 * PCS XYZ) with the tags desc, cprt, wtpt (D50), rXYZ/gXYZ/bXYZ (the
 * colorants, Bradford-adapted D65 -> D50 as ICC requires) and one shared
 * TRC (curv: 1024 entries for the piecewise curves, a single gamma for
 * Adobe RGB). Version 2 for the widest reader support (phone galleries,
 * social sites, old viewers). No vendor profile is copied. Pure: writes
 * into the caller's buffer. */
#ifndef FT_ICC_H
#define FT_ICC_H
#include "ft_output.h"
#include <stddef.h>

#define FT_ICC_MAX 4096 /* every profile here fits */

/* the profile for s in buf; returns its size, 0 if cap is too small or
 * s unknown */
size_t ft_icc_build(ft_space s, unsigned char *buf, size_t cap);
/* A linear (gamma 1) display profile for a scanner's own RGB (sub-project
 * 2b): colorants from xyz_to_rgb (XYZ D65 -> RGB, as ft_image carries it;
 * inverted, Bradford-adapted to D50), one identity TRC (curv with no
 * entries). The 16-bit linear negative TIFFs carry it, so viewers do not
 * take their linear values for sRGB. desc is truncated to 63 characters.
 * Returns the size; 0 if cap is too small or the matrix is singular. */
size_t ft_icc_build_linear(
    const double xyz_to_rgb[9], const char *desc, unsigned char *buf, size_t cap
);
/* Bradford adaptation D65 (from its xy) -> ICC D50, 3x3 row-major */
void ft_icc_bradford(double m[9]);

#endif

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

#include <stddef.h>
#include <stdint.h>

#include "ft_output.h"

/* every profile built here fits in this many bytes */
#define FT_ICC_MAX_BYTES 4096

/* Build the profile for an output space into buffer.
 * Returns its size, or 0 when capacity is too small or the space is
 * unknown. */
size_t ft_build_icc_profile(ft_space space, uint8_t* buffer, size_t capacity);

/* Build a linear (gamma 1) display profile for a scanner's own RGB
 * (sub-project 2b): colorants from xyz_to_rgb (XYZ D65 -> RGB, as ft_image
 * carries it; inverted, Bradford-adapted to D50), one identity TRC (curv
 * with no entries). The 16-bit linear negative TIFFs carry it, so viewers
 * do not take their linear values for sRGB. description is cut to 63
 * characters.
 * Returns the size, or 0 when capacity is too small or the matrix is
 * singular. */
size_t ft_build_linear_icc_profile(const double xyz_to_rgb[9],
                                   const char* description, uint8_t* buffer,
                                   size_t capacity);

/* The Bradford adaptation D65 (from its xy) -> ICC D50, 3x3 row-major. */
void ft_get_bradford_matrix(double matrix[9]);

#endif

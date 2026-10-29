/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Output colour: a rendered image (linear Rec.2020, ft_render.h) -> an
 * encoded output space, ready to quantise and write.
 *
 *   spaces   sRGB; Display P3 (P3 primaries, D65, the sRGB curve: what
 *            phones and browsers mean by "P3"); Adobe RGB (1998). All
 *            D65 like the input, so the pixel path is one 3x3 matrix,
 *            computed from the published primaries (no white adaptation)
 *   gamut    soft (default): OkLab lightness and hue stay; chroma up to
 *            FT_OUTPUT_KNEE of the target's boundary at that lightness
 *            and hue is left exactly as it is (greys and ordinary colours
 *            are bit-identical), chroma above it is compressed so that
 *            the Rec.2020 boundary lands on the target's (the ACES gamut
 *            compressor's power curve, p = 1.2, in OkLCh). Colours near
 *            the edge move a little even when they fit: that keeps the
 *            mapping smooth and monotone across the edge (no band where
 *            a gradient leaves the gamut). Boundaries are tables over
 *            (L, h) built at compile time.
 *            clip: per-channel clamp to 0..1 (quick, shifts hues)
 *   encode   the space's transfer curve through a LUT indexed by sqrt(v)
 *   quantise 8-bit with deterministic TPDF dither (+-1 LSB from a hash of
 *            the pixel index: tiles and threads give the same bytes), or
 *            16-bit rounded
 *
 * Pure C99 like the render core (no I/O, no allocation): Quixote's
 * preview runs the same code as the export. */
#ifndef FT_OUTPUT_H
#define FT_OUTPUT_H

#include <stddef.h>
#include <stdint.h>

#include "ft.h"

/* The output colour spaces. Rec.2020 is the render's own space. */
typedef enum ft_space {
  FT_SPACE_SRGB,
  FT_SPACE_DISPLAY_P3,
  FT_SPACE_ADOBE_RGB,
  FT_SPACE_REC2020
} ft_space;

/* How colours outside the target space are brought in. */
typedef enum ft_gamut {
  /* compress chroma near the edge, keeping lightness and hue */
  FT_GAMUT_SOFT,
  /* clamp each channel to 0..1 */
  FT_GAMUT_CLIP
} ft_gamut;

/* chroma is kept untouched up to this share of the target boundary */
#define FT_OUTPUT_KNEE 0.8f
/* The boundary tables: lightness steps (0..1) and hue steps (0..2 pi).
 * The table stays within 8 % of the true boundary (worst near the bright
 * yellow-green cusp). */
#define FT_OUTPUT_LIGHTNESS_STEPS 128
#define FT_OUTPUT_HUE_STEPS 256
/* the encode table's entries, indexed by sqrt(value) */
#define FT_OUTPUT_CURVE_SIZE 16384
/* OkLab chroma at or below this is grey: never compressed */
#define FT_OUTPUT_GREY_CHROMA 1e-5

/* Find a space by name: "srgb", "p3", "adobe" or "rec2020" (the input
 * space; 16-bit outputs only, 8 bits band in it).
 * Returns 0 and fills *space, or -1 when the name is unknown. */
int32_t ft_find_space(const char* name, ft_space* space);

/* The space's short name ("srgb"); "?" when unknown. */
const char* ft_get_space_name(ft_space space);

/* The space's full name, for ICC profiles and descriptions; "?" when
 * unknown. */
const char* ft_describe_space(ft_space space);

/* Find a gamut mode by name: "soft" or "clip".
 * Returns 0 and fills *gamut, or -1 when the name is unknown. */
int32_t ft_find_gamut(const char* name, ft_gamut* gamut);

/* The CIE xy of red, green, blue and white (D65): 8 values. */
void ft_get_space_primaries(ft_space space, double xy[8]);

/* The linear RGB -> XYZ matrix (Y of white = 1) from xy primaries and
 * white. */
void ft_compute_rgb_to_xyz(const double xy[8], double matrix[9]);

/* Invert a 3x3 matrix. Returns 0, or -1 when it is singular. */
int32_t ft_invert_matrix3(const double matrix[9], double inverse[9]);

/* The space's transfer curve and its inverse; value clamped to 0..1. */
double ft_encode_space(ft_space space, double value);
double ft_decode_space(ft_space space, double value);

/* OkLab (Ottosson 2020) from XYZ D65, and back. */
void ft_convert_xyz_to_oklab(const double xyz[3], double lab[3]);
void ft_convert_oklab_to_xyz(const double lab[3], double xyz[3]);

/* A compiled output stage. ~330 KB: static or heap, not on a (wasm)
 * stack. */
typedef struct ft_output {
  ft_space space;
  ft_gamut gamut;
  /* linear Rec.2020 -> linear target */
  float rec2020_to_target[9];
  /* the target's linear RGB -> XYZ, and back */
  double to_xyz[9];
  double from_xyz[9];
  /* the largest chroma inside the target, per lightness and hue */
  float target_boundary[FT_OUTPUT_LIGHTNESS_STEPS + 1][FT_OUTPUT_HUE_STEPS + 1];
  /* the same for Rec.2020, the source */
  float source_boundary[FT_OUTPUT_LIGHTNESS_STEPS + 1][FT_OUTPUT_HUE_STEPS + 1];
  /* the transfer curve, indexed by sqrt(value) */
  float curve[FT_OUTPUT_CURVE_SIZE];
} ft_output;

/* Compile the output stage for a space and gamut mode.
 * Returns 0, or -1 with error filled on an unknown space or gamut. */
int32_t ft_compile_output(ft_space space, ft_gamut gamut, ft_output* output,
                          ft_err* error);

/* Run pixel_count interleaved RGB pixels through the output stage:
 * linear Rec.2020 in, the encoded target (0..1) out. input == output is
 * allowed. NaN or inf gives 0. Never fails. */
void ft_apply_output(const ft_output* output, const float* input,
                     float* encoded, size_t pixel_count);

/* The gamut step alone, on linear target RGB, in place (exposed for
 * tests and for writers that encode themselves). */
void ft_map_output_gamut(const ft_output* output, float rgb[3]);

/* Encoded 0..1 -> 8 bits, TPDF-dithered. first_pixel is the index of
 * encoded[0]'s pixel in the image (for the dither pattern). 0 and 1 stay
 * exact. */
void ft_quantise_output8(const float* encoded, uint8_t* bytes,
                         size_t pixel_count, size_t first_pixel);

/* Encoded 0..1 -> 16 bits, rounded. */
void ft_quantise_output16(const float* encoded, uint16_t* values,
                          size_t pixel_count);

#endif

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
 * Pure C99 like the render core (no I/O, no allocation): the browser's
 * preview runs the same code as the export. */
#ifndef FT_OUTPUT_H
#define FT_OUTPUT_H
#include <stddef.h>

#include "ft.h"

typedef enum {
  FT_SPACE_SRGB,
  FT_SPACE_DISPLAY_P3,
  FT_SPACE_ADOBE_RGB,
  FT_SPACE_REC2020
} ft_space;
typedef enum { FT_GAMUT_SOFT, FT_GAMUT_CLIP } ft_gamut;

#define FT_OUTPUT_KNEE                           \
  0.8f /* chroma kept untouched up to this share \
          of the target boundary */
#define FT_OUTPUT_NL                                                 \
  128                         /* boundary table: L steps (0..1); the \
                                 table stays within 8 % of the true  \
                                 boundary (worst near the bright     \
                                 yellow-green cusp) */
#define FT_OUTPUT_NH 256      /* boundary table: hue steps (0..2 pi) */
#define FT_OUTPUT_TRC_N 16384 /* encode LUT, indexed by sqrt(v) */
#define FT_OUTPUT_C_EPS 1e-5  /* OkLab chroma never compressed: greys */

/* "srgb", "p3", "adobe", "rec2020" (the input space; 16-bit outputs
 * only, 8 bits band in it): 0, or -1 if unknown */
int ft_space_find(const char* name, ft_space* s);
const char* ft_space_name(ft_space s);
const char* ft_space_what(ft_space s); /* human name, for ICC/desc */
int ft_gamut_find(const char* name, ft_gamut* g);
/* CIE xy of red, green, blue and white (D65), 8 values */
void ft_space_primaries(ft_space s, double xy[8]);
/* linear RGB -> XYZ (Y of white = 1) from xy primaries and white */
void ft_rgb_to_xyz(const double xy[8], double m[9]);
/* 3x3 inverse; -1 if singular */
int ft_mat3_inv(const double m[9], double inv[9]);
/* the space's transfer curve, v clamped to 0..1 */
double ft_space_encode(ft_space s, double v);
double ft_space_decode(ft_space s, double v);
/* OkLab (Ottosson 2020) from XYZ D65 and back */
void ft_xyz_to_oklab(const double xyz[3], double lab[3]);
void ft_oklab_to_xyz(const double lab[3], double xyz[3]);

/* ~330 KB: static or heap, not on a (WASM) stack */
typedef struct {
  ft_space space;
  ft_gamut gamut;
  float m[9];                    /* linear Rec.2020 -> linear target */
  double to_xyz[9], from_xyz[9]; /* target linear RGB <-> XYZ */
  float cmax[FT_OUTPUT_NL + 1][FT_OUTPUT_NH + 1]; /* target boundary */
  float csrc[FT_OUTPUT_NL + 1][FT_OUTPUT_NH + 1]; /* Rec.2020's */
  float trc[FT_OUTPUT_TRC_N];
} ft_output;

/* -1 with e filled on an unknown space or gamut */
int ft_output_compile(ft_space s, ft_gamut g, ft_output* o, ft_err* e);
/* npx interleaved RGB pixels, linear Rec.2020 in, encoded target (0..1)
 * out; in == out is allowed. NaN/inf -> 0. Never fails. */
void ft_output_apply(const ft_output* o, const float* in, float* out,
                     size_t npx);
/* the gamut step alone, linear target RGB in place (exposed for tests and
 * for writers that encode themselves) */
void ft_output_gamut(const ft_output* o, float rgb[3]);
/* encoded 0..1 -> 8 bits, TPDF-dithered; first_px = index of in[0]'s
 * pixel in the image (for the dither pattern). 0 and 1 stay exact. */
void ft_output_quantise8(const float* enc, unsigned char* out, size_t npx,
                         size_t first_px);
/* encoded 0..1 -> 16 bits, rounded */
void ft_output_quantise16(const float* enc, unsigned short* out, size_t npx);

#endif

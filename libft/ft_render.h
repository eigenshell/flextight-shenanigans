/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Render: a developed frame (linear transmittance, ft_image.h) -> a
 * positive in linear Rec.2020, set by a recipe of plain numbers.
 *
 *   negative  density, film base out, channel ranges balanced, print
 *             exposure and tint (darkroom filtration, CC units), virtual
 *             paper (sigmoid between paper white and paper black)
 *   slide     black point out, exposure and tint as gains
 *   b&w       the negative path on luminance, then the master curve,
 *             copied to R, G and B (no matrix, no channel curves: grey)
 *   all       3x3 matrix to linear Rec.2020, then the curves (master,
 *             then per channel) on sRGB-encoded values
 *
 * ft_render_compile turns a recipe into lookup tables (per-channel input
 * LUT indexed by the float bits of T, i.e. log-spaced; per-channel curve
 * LUT indexed by sqrt(v)); ft_render_apply streams pixels through them
 * with no transcendental calls, fast enough for live sliders in the
 * browser (this file compiles to WASM: no I/O, no allocation, no
 * threads). ft_render_direct evaluates the same maths per pixel in double;
 * the tests hold the LUT path to it. */
#ifndef FT_RENDER_H
#define FT_RENDER_H
#include "ft.h"
#include "ft_curve.h"
#include "ft_image.h"
#include <stddef.h>

typedef enum { FT_RENDER_NEG, FT_RENDER_SLIDE, FT_RENDER_BW } ft_render_mode;

#define FT_RENDER_T_MAX 1.26f /* LUT top: noise/flare above open beam */

typedef struct {
  ft_render_mode mode;
  float base[3];       /* D: film base (neg; b&w uses base[1]) or the
                          black of unexposed film (slide) */
  float range[3];      /* neg: D above base of the brightest highlight,
                          per channel (b&w uses range[1]) */
  float exposure;      /* stops, + = brighter */
  float tint[3];       /* CC (10 CC = 0.10 D): + = redder, greener,
                          bluer; b&w: must be 0 */
  float grade;         /* paper contrast, 1.5 = normal */
  float paper_black;   /* D of the paper's deepest black */
  float paper_white;   /* D of the paper base (white = 1.0 out) */
  float bw_weights[3]; /* b&w luminance from R, G, B; sum 1 */
  float matrix[9];     /* scanner RGB -> linear Rec.2020, row-major */
  ft_curve master, ch[3];
} ft_recipe;

/* neutral recipe for mode: base 0 (neg) / 3.0 (slide), range 1, exposure
 * and tint 0, grade 1.5, paper 0..2 D, Rec.709 b&w weights, the linear
 * sRGB -> Rec.2020 matrix, linear curves */
void ft_recipe_default(ft_recipe *r, ft_render_mode m);
/* -1 with e naming the field if any value is out of range */
int ft_recipe_validate(const ft_recipe *r, ft_err *e);
/* set base from per-channel film densities; b&w: folds them to the
 * luminance density of r->bw_weights and stores it in all three. If any
 * d[c] is not finite, r->base is left unchanged. */
void ft_recipe_set_base(ft_recipe *r, const float d[3]);

/* Accuracy: the compiled path is within 2/65535 (sRGB-encoded) of
 * ft_render_direct for grade/range up to ~25; up to ~15/65535 at the
 * validated extreme (grade 5, range 0.05): invisible at 8 bits.
 * LUT sizes: input LUT nodes are the floats whose bits >> 13 run from
 * FT_RENDER_IN_K0 (10^-4.5) to that of FT_RENDER_T_MAX, 1024 per octave */
#define FT_RENDER_IN_SHIFT 13
#define FT_RENDER_IN_K0 114725u
#define FT_RENDER_IN_N 15591
#define FT_RENDER_CURVE_N 16384

/* ~380 KB: keep it static or on the heap, not on a (WASM) stack */
typedef struct {
  ft_render_mode mode;
  float w[3];
  float in[3][FT_RENDER_IN_N];
  float m[9];
  float out[3][FT_RENDER_CURVE_N];
} ft_render;

/* an input LUT (in[] above) at transmittance t: linear between the two
 * nodes around t, clamped to the LUT's range (NaN and -inf: the floor) */
float ft_render_lut_in(const float *tab, float t);
/* validates r, then builds c; -1 with e filled on a bad recipe */
int ft_render_compile(const ft_recipe *r, ft_render *c, ft_err *e);
/* npx interleaved RGB pixels; in == out is allowed. NaN, -inf and values
 * below the floor -> the floor; +inf and values above FT_RENDER_T_MAX ->
 * T_MAX. Never fails, never writes NaN */
void ft_render_apply(
    const ft_render *c, const float *in, float *out, size_t npx
);
/* the same pipeline in double, one pixel; r must be valid */
void ft_render_direct(const ft_recipe *r, const float in[3], float out[3]);
/* apply to every pixel of im in place; im becomes linear Rec.2020
 * (xyz_to_rgb set to match) */
void ft_render_image(const ft_render *c, ft_image *im);

/* sRGB transfer curve, clamped to [0,1] (shared with the writers) */
double ft_srgb_encode(double v);
double ft_srgb_decode(double v);

#endif

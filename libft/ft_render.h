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
 * ft_compile_render turns a recipe into lookup tables (per-channel input
 * LUT indexed by the float bits of T, i.e. log-spaced; per-channel curve
 * LUT indexed by sqrt(v)); ft_apply_render streams pixels through them
 * with no transcendental calls, fast enough for live sliders in the
 * browser (this file compiles to WASM: no I/O, no allocation, no
 * threads). ft_render_pixel_directly evaluates the same maths per pixel in
 * double; the tests hold the LUT path to it. */
#ifndef FT_RENDER_H
#define FT_RENDER_H

#include <stddef.h>
#include <stdint.h>

#include "ft.h"
#include "ft_curve.h"
#include "ft_image.h"

/* What the film is, for the render. */
typedef enum ft_render_mode {
  FT_RENDER_NEGATIVE,
  FT_RENDER_SLIDE,
  FT_RENDER_BW
} ft_render_mode;

/* the input table's top: noise and flare reach above the open beam */
#define FT_RENDER_MAX_TRANSMITTANCE 1.26f

/* How to render: plain numbers a user's sliders set. */
typedef struct ft_recipe {
  ft_render_mode mode;
  /* density of the film base (negative; b&w uses [1]) or of the black of
   * unexposed film (slide) */
  float base_density[3];
  /* negative: the density above the base of the brightest highlight, per
   * channel (b&w uses [1]) */
  float range_density[3];
  /* print exposure, stops; + = brighter */
  float exposure;
  /* filtration, CC (10 CC = 0.10 D): + = redder, greener, bluer; b&w:
   * must be 0 */
  float tint[3];
  /* paper contrast, 1.5 = normal */
  float grade;
  /* density of the paper's deepest black */
  float paper_black;
  /* density of the paper base (white = 1.0 out) */
  float paper_white;
  /* b&w: luminance from R, G, B; they sum to 1 */
  float bw_weights[3];
  /* scanner RGB -> linear Rec.2020, row-major */
  float matrix[9];
  /* the master curve, then one per channel */
  ft_curve master;
  ft_curve channels[3];
} ft_recipe;

/* A neutral recipe for mode: base 0 (negative) / 3.0 (slide), range 1,
 * exposure and tint 0, grade 1.5, paper 0..2 D, Rec.709 b&w weights, the
 * linear sRGB -> Rec.2020 matrix, linear curves. */
void ft_init_recipe(ft_recipe* recipe, ft_render_mode mode);

/* Check every value is in range.
 * Returns 0, or -1 with error naming the field. */
int32_t ft_validate_recipe(const ft_recipe* recipe, ft_err* error);

/* Set the base from per-channel film densities. b&w folds them to the
 * luminance density of bw_weights and stores it in all three. When any
 * density is not finite, the base is left unchanged. */
void ft_set_recipe_base(ft_recipe* recipe, const float density[3]);

/* Accuracy: the compiled path is within 2/65535 (sRGB-encoded) of
 * ft_render_pixel_directly for grade/range up to ~25; up to ~15/65535 at
 * the validated extreme (grade 5, range 0.05): invisible at 8 bits.
 *
 * The input table's nodes are the floats whose bits >> 13 run from
 * FT_RENDER_INPUT_FIRST_KEY (10^-4.5) to that of
 * FT_RENDER_MAX_TRANSMITTANCE: 1024 per octave. */
#define FT_RENDER_INPUT_SHIFT 13
#define FT_RENDER_INPUT_FIRST_KEY 114725u
#define FT_RENDER_INPUT_SIZE 15591
/* the curve table's entries, indexed by sqrt(value) */
#define FT_RENDER_CURVE_SIZE 16384

/* A compiled recipe. ~380 KB: keep it static or on the heap, not on a
 * (wasm) stack. */
typedef struct ft_render {
  ft_render_mode mode;
  /* b&w: the luminance weights */
  float bw_weights[3];
  /* film -> linear, per channel, indexed by the bits of T */
  float input[3][FT_RENDER_INPUT_SIZE];
  /* the recipe's matrix */
  float matrix[9];
  /* the curves, per channel, indexed by sqrt(value) */
  float output[3][FT_RENDER_CURVE_SIZE];
} ft_render;

/* Look up an input table at transmittance: linear between the two nodes
 * around it, clamped to the table's range (NaN and -inf: the floor). */
float ft_look_up_render_input(const float* table, float transmittance);

/* Validate the recipe, then build the tables.
 * Returns 0, or -1 with error filled on a bad recipe. */
int32_t ft_compile_render(const ft_recipe* recipe, ft_render* render,
                          ft_err* error);

/* Render pixel_count interleaved RGB pixels; input == output is allowed.
 * NaN, -inf and values below the floor give the floor; +inf and values
 * above FT_RENDER_MAX_TRANSMITTANCE give that. Never fails, never writes
 * NaN. */
void ft_apply_render(const ft_render* render, const float* input, float* output,
                     size_t pixel_count);

/* The same pipeline in double, one pixel; the recipe must be valid. */
void ft_render_pixel_directly(const ft_recipe* recipe, const float input[3],
                              float output[3]);

/* Render every pixel of image in place; image becomes linear Rec.2020
 * (xyz_to_rgb set to match). */
void ft_render_image(const ft_render* render, ft_image* image);

/* The sRGB transfer curve and its inverse, clamped to [0,1] (shared with
 * the writers). */
double ft_encode_srgb(double value);
double ft_decode_srgb(double value);

#endif

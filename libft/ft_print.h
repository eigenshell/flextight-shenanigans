/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Print: scan to share as a darkroom / minilab print. The user picks an
 * emulsion per roll and nudges each frame with C/M/Y/D clicks; everything
 * else is the emulsion's science, compiled in (ft_emulsion.h).
 *
 * An emulsion is a chain of four modules (ft_pm.h), one per slot:
 *
 *   linear T -> [film] -> density -> (filtration: auto + clicks)
 *            -> [print] -> [output] -> linear Rec.2020 (ft_output.h)
 *
 *   film     base and contrast: the recipe's base and range
 *   balance  the per-frame auto filtration: exposure and tint
 *   print    the paper: grade and paper densities (ft_render), or a 3D
 *            table (lut3d)
 *   output   to linear Rec.2020: the recipe's matrix
 *
 * A frame resolves to an ft_recipe (ft_render.h) plus, for a lut3d print,
 * the table. Clicks act at the filtration point, after the auto: one
 * click = 0.025 D of print exposure (a printer point; 12 clicks = 1
 * stop). + C/M/Y = more of that colour in the print, + D = a denser
 * (darker) print; +1 C +1 M +1 Y = +1 D, as on a colour enlarger.
 *
 * Statistics (film ranges, balance means) are measured on a stats proxy:
 * the frame box downscaled to FT_PRINT_STATS_EDGE_PIXELS pixels on its long
 * side, so Sancho and Quixote compute the same auto from the same proxy.
 *
 * Pure C99 (libm, mem*; no allocation, no I/O): runs in the browser too. */
#ifndef FT_PRINT_H
#define FT_PRINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ft.h"
#include "ft_image.h"
#include "ft_render.h"

/* density of print exposure per click */
#define FT_PRINT_CLICK_DENSITY 0.025
/* the most clicks either way */
#define FT_PRINT_CLICKS_MAX 99
/* the stats proxy: pixels on its long side */
#define FT_PRINT_STATS_EDGE_PIXELS 512

/* A frame's clicks: + = more of that colour, + density = a darker
 * print. */
typedef struct ft_cmyd {
  int32_t cyan;
  int32_t magenta;
  int32_t yellow;
  int32_t density;
} ft_cmyd;

/* What one frame contributes to the roll. */
typedef struct ft_print_frame {
  /* the frame's own auto range (ft_estimate_recipe) */
  float range_density[3];
  /* balance: the mean film density per channel (green-referenced D
   * above base) */
  double mean_density[3];
  bool has_mean;
} ft_print_frame;

/* The roll: measured once over all its frames. */
typedef struct ft_print_roll {
  ft_render_mode mode;
  /* the film base (negative, b&w) or the slide's black, D */
  float base_density[3];
  /* film: a roll range (else per frame) */
  bool has_range;
  float range_density[3];
  /* balance: the roll's references */
  bool has_reference;
  double reference_density;
  double reference_colour[3];
  uint32_t frame_count;
} ft_print_roll;

/* The auto filtration, in recipe units. */
typedef struct ft_print_auto {
  /* stops, + = lighter */
  float exposure;
  /* CC, + = redder, greener, bluer */
  float tint[3];
} ft_print_auto;

/* A film module; parameters is the module's own parameter block. */
typedef struct ft_film_module {
  const char* name;
  /* the roll's range from the frames' own ranges; NULL = none */
  int32_t (*measure_roll)(const void* parameters, const ft_print_frame* frames,
                          uint32_t frame_count, ft_print_roll* roll,
                          ft_err* error);
  /* the frame's recipe range (base and mode are set) */
  void (*set_range)(const void* parameters, const ft_print_roll* roll,
                    const ft_print_frame* frame, ft_recipe* recipe);
} ft_film_module;

/* A balance module. */
typedef struct ft_balance_module {
  const char* name;
  /* frame->mean_density from the image through the recipe (base and
   * range set); NULL = none */
  int32_t (*measure)(const void* parameters, const ft_recipe* recipe,
                     const ft_image* image, ft_print_frame* frame,
                     ft_err* error);
  /* the roll's references from the frames' means; NULL = none */
  int32_t (*measure_roll)(const void* parameters, const ft_print_frame* frames,
                          uint32_t frame_count, ft_print_roll* roll,
                          ft_err* error);
  /* the auto filtration for one frame (recipe: base, range, paper set) */
  void (*set_frame)(const void* parameters, const ft_print_roll* roll,
                    const ft_print_frame* frame, const ft_recipe* recipe,
                    ft_print_auto* automatic);
} ft_balance_module;

typedef struct ft_print_render ft_print_render;

/* A print (paper) module. */
typedef struct ft_print_module {
  const char* name;
  /* set the recipe's paper fields */
  void (*set_recipe)(const void* parameters, ft_recipe* recipe);
  /* NULL = ft_compile_render; else the module's own compiled form */
  int32_t (*compile)(const void* parameters, const ft_recipe* recipe,
                     ft_print_render* render, ft_err* error);
} ft_print_module;

/* An output module. */
typedef struct ft_output_module {
  const char* name;
  /* set the recipe's matrix */
  void (*set_recipe)(const void* parameters, ft_recipe* recipe);
} ft_output_module;

/* An emulsion: one module per slot, with its parameters. */
typedef struct ft_emulsion {
  /* the .print file's name: "generic-neg" */
  const char* id;
  /* shown to users: "Generic colour negative" */
  const char* name;
  ft_render_mode mode;
  const ft_film_module* film;
  const void* film_parameters;
  const ft_balance_module* balance;
  const void* balance_parameters;
  const ft_print_module* print;
  const void* print_parameters;
  const ft_output_module* output;
  const void* output_parameters;
} ft_emulsion;

/* A 3D table: size^3 RGB nodes, red index fastest (the .cube order),
 * values sRGB-encoded 0..1. tools/cube2c.py writes these as
 * { size, nodes }: keep the field order. */
typedef struct ft_cube {
  int32_t size;
  const float* nodes;
} ft_cube;

/* the sRGB decode table's entries (lut3d) */
#define FT_PRINT_DECODE_SIZE 4096

/* lut3d's compiled form. */
typedef struct ft_print_lut {
  /* T -> cube coordinate 0..size-1, per channel */
  float input[3][FT_RENDER_INPUT_SIZE];
  const ft_cube* cube;
  float decode[FT_PRINT_DECODE_SIZE + 1];
  float matrix[9];
} ft_print_lut;

/* A compiled print: the render core's, or lut3d's. ~380 KB: keep it
 * static or on the heap. */
struct ft_print_render {
  /* compiled.lut is used, else compiled.render */
  bool is_lut;
  union {
    ft_render render;
    ft_print_lut lut;
  } compiled;
};

/* Measure the roll: each frame's range (pass 1), the film module's roll
 * step, each frame's balance means (pass 2), the balance's roll step.
 *
 * images  frame_count stats proxies
 * base    the film base (negative, b&w) or black (slide)
 * frames  receives frame_count results, kept for ft_make_print_recipe
 *
 * Returns 0, or -1 with error filled. */
int32_t ft_measure_print_roll(const ft_emulsion* emulsion, const float base[3],
                              const ft_image* const* images,
                              uint32_t frame_count, ft_print_frame* frames,
                              ft_print_roll* roll, ft_err* error);

/* Check clicks are in -FT_PRINT_CLICKS_MAX..+FT_PRINT_CLICKS_MAX, and
 * C/M/Y 0 for b&w. Returns 0, or -1 with error filled. */
int32_t ft_check_print_clicks(const ft_emulsion* emulsion, ft_cmyd clicks,
                              ft_err* error);

/* One frame's recipe: base and range (film), paper (print), matrix
 * (output), the auto (automatic, may be NULL) and then the clicks.
 * Exposure and tint beyond the recipe's limits (+-6 stops, +-50 CC) are
 * clamped and *was_clamped (may be NULL) set.
 * Returns 0, or -1 with error filled on bad clicks. */
int32_t ft_make_print_recipe(const ft_emulsion* emulsion,
                             const ft_print_roll* roll,
                             const ft_print_frame* frame, ft_cmyd clicks,
                             ft_recipe* recipe, ft_print_auto* automatic,
                             bool* was_clamped, ft_err* error);

/* Compile a recipe for the emulsion's print module.
 * Returns 0, or -1 with error filled. */
int32_t ft_compile_print(const ft_emulsion* emulsion, const ft_recipe* recipe,
                         ft_print_render* render, ft_err* error);

/* pixel_count interleaved RGB pixels of transmittance -> linear Rec.2020
 * 0..1; input == output is allowed. */
void ft_apply_print(const ft_print_render* render, const float* input,
                    float* output, size_t pixel_count);

/* The stats proxy's size for a width x height frame box (never
 * upscaled). */
void ft_get_print_proxy_size(uint32_t width, uint32_t height,
                             uint32_t* proxy_width, uint32_t* proxy_height);

#endif

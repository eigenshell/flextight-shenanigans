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
 * the frame box downscaled to FT_PRINT_STATS_EDGE px on its long side, so
 * the box and the browser compute the same auto from the same proxy.
 *
 * Pure C99 (libm, mem*; no allocation, no I/O): runs in the browser too. */
#ifndef FT_PRINT_H
#define FT_PRINT_H
#include <stddef.h>

#include "ft.h"
#include "ft_image.h"
#include "ft_render.h"

#define FT_PRINT_CLICK 0.025 /* D of print exposure per click */
#define FT_PRINT_CLICKS_MAX 99
#define FT_PRINT_STATS_EDGE 512 /* stats proxy: px on the long side */

typedef struct {
  int c, m, y, d;
} ft_cmyd;

/* what one frame contributes to the roll */
typedef struct {
  float range[3]; /* the frame's own auto range (ft_render_auto) */
  double mean[3]; /* balance: mean film density per channel
                     (green-referenced D above base) */
  int has_mean;
} ft_print_frame;

/* the roll: measured once over all its frames */
typedef struct {
  ft_render_mode mode;
  float base[3]; /* film base (neg, b&w) or slide black, D */
  int has_range; /* film: a roll range (else per frame) */
  float range[3];
  int has_ref; /* balance: roll references */
  double ref_d, ref_c[3];
  int nframes;
} ft_print_roll;

/* the auto filtration, in recipe units */
typedef struct {
  float exposure; /* stops, + = lighter */
  float tint[3];  /* CC, + = redder, greener, bluer */
} ft_print_auto;

/* one module per slot; par is the module's own parameter block */
typedef struct {
  const char* name;
  /* roll: roll->range from the frames' own ranges; NULL = none */
  int (*roll)(const void* par, const ft_print_frame* fr, int n,
              ft_print_roll* roll, ft_err* e);
  /* the frame's recipe range (base and mode are set) */
  void (*range)(const void* par, const ft_print_roll* roll,
                const ft_print_frame* fr, ft_recipe* r);
} ft_pm_film;

typedef struct {
  const char* name;
  /* fr->mean from the frame through r (base, range set); NULL = none */
  int (*measure)(const void* par, const ft_recipe* r, const ft_image* im,
                 ft_print_frame* fr, ft_err* e);
  /* roll references from the frames' means; NULL = none */
  int (*roll)(const void* par, const ft_print_frame* fr, int n,
              ft_print_roll* roll, ft_err* e);
  /* the auto filtration for one frame (r: base, range, paper set) */
  void (*frame)(const void* par, const ft_print_roll* roll,
                const ft_print_frame* fr, const ft_recipe* r, ft_print_auto* a);
} ft_pm_balance;

typedef struct ft_print_render ft_print_render;

typedef struct {
  const char* name;
  void (*recipe)(const void* par, ft_recipe* r); /* paper fields */
  /* NULL = ft_render_compile; else the module's own compiled form */
  int (*compile)(const void* par, const ft_recipe* r, ft_print_render* c,
                 ft_err* e);
} ft_pm_print;

typedef struct {
  const char* name;
  void (*recipe)(const void* par, ft_recipe* r); /* matrix */
} ft_pm_output;

typedef struct {
  const char* id;   /* the .print file's name: "generic-neg" */
  const char* name; /* shown to users: "Generic colour negative" */
  ft_render_mode mode;
  const ft_pm_film* film;
  const void* film_par;
  const ft_pm_balance* balance;
  const void* balance_par;
  const ft_pm_print* print;
  const void* print_par;
  const ft_pm_output* output;
  const void* output_par;
} ft_emulsion;

/* a 3D table: n^3 RGB nodes, red index fastest (the .cube order), values
 * sRGB-encoded 0..1 */
typedef struct {
  int n;
  const float* rgb;
} ft_cube;

#define FT_PRINT_DEC_N 4096 /* sRGB decode table (lut3d) */

typedef struct {
  float in[3][FT_RENDER_IN_N]; /* T -> cube coordinate 0..n-1 */
  const ft_cube* cube;
  float dec[FT_PRINT_DEC_N + 1];
  float m[9];
} ft_print_lut;

/* ~380 KB: keep it static or on the heap */
struct ft_print_render {
  int lut; /* 0: r, 1: l */
  union {
    ft_render r;
    ft_print_lut l;
  } u;
};

/* Measure the roll: each frame's range (pass 1), the film module's roll
 * step, each frame's balance means (pass 2), the balance's roll step.
 * im: n stats proxies; base: the film base (neg, b&w) or black (slide);
 * fr: n results, kept for ft_print_recipe. -1 with e filled. */
int ft_print_roll_measure(const ft_emulsion* em, const float base[3],
                          const ft_image* const* im, int n, ft_print_frame* fr,
                          ft_print_roll* roll, ft_err* e);

/* clicks in -FT_PRINT_CLICKS_MAX..+FT_PRINT_CLICKS_MAX; b&w: C/M/Y 0 */
int ft_print_clicks_ok(const ft_emulsion* em, ft_cmyd k, ft_err* e);

/* One frame's recipe: base and range (film), paper (print), matrix
 * (output), the auto (a, NULL ok) and then the clicks. Exposure and tint
 * beyond the recipe's limits (+-6 stops, +-50 CC) are clamped and
 * *clamped (NULL ok) set. -1 with e filled on bad clicks. */
int ft_print_recipe(const ft_emulsion* em, const ft_print_roll* roll,
                    const ft_print_frame* fr, ft_cmyd k, ft_recipe* r,
                    ft_print_auto* a, int* clamped, ft_err* e);

int ft_print_compile(const ft_emulsion* em, const ft_recipe* r,
                     ft_print_render* c, ft_err* e);
/* npx interleaved RGB pixels of transmittance -> linear Rec.2020 0..1;
 * in == out is allowed */
void ft_print_apply(const ft_print_render* c, const float* in, float* out,
                    size_t npx);

/* the stats proxy's size for a w x h frame box (never upscaled) */
void ft_print_proxy_dims(int w, int h, int* pw, int* ph);

#endif

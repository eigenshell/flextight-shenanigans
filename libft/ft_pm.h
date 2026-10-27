/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Print modules (ft_print.h): the science behind the emulsions. One file
 * per module (ft_pm_*.c); an emulsion (ft_emulsion.c) picks one per slot
 * and gives it its parameters. To add science: a new module file with its
 * parameter block here, then an emulsion entry that uses it. Every module
 * is pure C99 (libft/test/purity.sh). */
#ifndef FT_PM_H
#define FT_PM_H
#include "ft_print.h"

/* film: classic = the frame's own auto range (ft_render_auto: contrast
 * stretched per frame, today's ftdev export); stock = one range for the
 * roll, scale x ratio (no per-frame stretch) */
extern const ft_pm_film ft_pm_film_classic;
extern const ft_pm_film ft_pm_film_stock;
typedef struct {
  float scale;    /* green range, D: the emulsion's contrast */
  int has_ratio;  /* else the roll's median r/g and b/g ratio */
  float ratio[3]; /* range[c] / range[green]; green = 1 */
} ft_pm_stock_par;

/* balance: classic = ft_render_auto's exposure (highlight at paper
 * white), no tint; frontier = large-area density with correction slopes
 * below 1 (ft_pm_frontier.c) */
extern const ft_pm_balance ft_pm_balance_classic;
extern const ft_pm_balance ft_pm_balance_frontier;
typedef struct {
  double aim;      /* e (D) the frame's mean film density prints at */
  double k_d;      /* density: share of a frame's deviation from
                      the roll that is corrected */
  double k_c;      /* colour: the same for colour, < 1 */
  double clamp;    /* colour: max per-frame correction, D */
  double sat;      /* blocks this saturated (max |d_c - mean|, D)
                      are left out of the means */
  double aim_c[3]; /* colour reference for short rolls */
  int min_frames;  /* fewer frames: aim_c, not the roll median */
} ft_pm_frontier_par;

/* print: classic = ft_render's sigmoid paper; lut3d = a 3D table on the
 * filtered density e (ft_pm_lut3d.c) */
extern const ft_pm_print ft_pm_print_classic;
extern const ft_pm_print ft_pm_print_lut3d;
typedef struct {
  float grade, black, white; /* ft_recipe's grade, paper_black and
                                paper_white */
} ft_pm_paper_par;           /* NULL par: ft_recipe_default's */
typedef struct {
  const ft_cube* cube;
  float lo, hi; /* e (D) at cube coordinate 0 and 1 */
} ft_pm_lut3d_par;

/* output: classic = the recipe's matrix (par: 9 floats, NULL = the
 * default sRGB primaries -> Rec.2020) */
extern const ft_pm_output ft_pm_output_classic;

/* lut3d's compiled form applied (ft_print_apply) */
void ft_pm_lut3d_apply(const ft_print_lut* l, const float* in, float* out,
                       size_t npx);

/* median of v[0..n) (n >= 1); sorts v */
double ft_print_median(double* v, int n);

#endif

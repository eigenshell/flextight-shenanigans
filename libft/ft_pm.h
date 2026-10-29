/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Print modules (ft_print.h): the science behind the emulsions. One file
 * per module (ft_pm_*.c); an emulsion (ft_emulsion.c) picks one per slot
 * and gives it its parameters. To add science: a new module file with its
 * parameter block here, then an emulsion entry that uses it. Every module
 * is pure C99 (libft/test/purity.sh). */
#ifndef FT_PM_H
#define FT_PM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ft_print.h"

/* Film: classic = the frame's own auto range (ft_estimate_recipe:
 * contrast stretched per frame, today's ftdev export); stock = one range
 * for the roll, scale x ratio (no per-frame stretch). */
extern const ft_film_module ft_film_module_classic;
extern const ft_film_module ft_film_module_stock;

/* The stock film module's parameters. */
typedef struct ft_stock_film_parameters {
  /* the green range, D: the emulsion's contrast */
  float scale_density;
  /* ratio is given; else the roll's median r/g and b/g ratio */
  bool has_ratio;
  /* range[c] / range[green]; green = 1 */
  float ratio[3];
} ft_stock_film_parameters;

/* Balance: classic = ft_estimate_recipe's exposure (highlight at paper
 * white), no tint; frontier = large-area density with correction slopes
 * below 1 (ft_pm_frontier.c). */
extern const ft_balance_module ft_balance_module_classic;
extern const ft_balance_module ft_balance_module_frontier;

/* The frontier balance's parameters. */
typedef struct ft_frontier_parameters {
  /* the density e the frame's mean film density prints at */
  double aim_density;
  /* density: the share of a frame's deviation from the roll that is
   * corrected */
  double density_correction;
  /* colour: the same for colour, < 1 */
  double colour_correction;
  /* colour: the largest per-frame correction, D */
  double clamp_density;
  /* blocks this saturated (max |d_c - mean|, D) are left out of the
   * means */
  double saturation_limit;
  /* the colour reference for short rolls */
  double aim_colour[3];
  /* with fewer frames: aim_colour, not the roll median */
  uint32_t min_frames;
} ft_frontier_parameters;

/* Print: classic = ft_render's sigmoid paper; lut3d = a 3D table on the
 * filtered density e (ft_pm_lut3d.c). */
extern const ft_print_module ft_print_module_classic;
extern const ft_print_module ft_print_module_lut3d;

/* The classic paper's parameters: the recipe's grade, paper_black and
 * paper_white. NULL parameters: ft_init_recipe's. */
typedef struct ft_paper_parameters {
  float grade;
  float black;
  float white;
} ft_paper_parameters;

/* The lut3d print's parameters. */
typedef struct ft_lut3d_parameters {
  const ft_cube* cube;
  /* the density e at cube coordinate 0 and 1 */
  float low_density;
  float high_density;
} ft_lut3d_parameters;

/* Output: classic = the recipe's matrix (parameters: 9 floats, NULL =
 * the default sRGB primaries -> Rec.2020). */
extern const ft_output_module ft_output_module_classic;

/* Apply lut3d's compiled form (ft_apply_print). */
void ft_apply_lut3d(const ft_print_lut* lut, const float* input, float* output,
                    size_t pixel_count);

/* The median of values[0..count) (count >= 1); sorts values. */
double ft_get_print_median(double* values, uint32_t count);

#endif

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The classic modules: today's render core (ft_render, ft_render_auto) as
 * print modules. An all-classic chain gives exactly ftdev export's recipe
 * without overrides. */
#include <stddef.h>
#include <string.h>

#include "ft_pm.h"
#include "ft_render_auto.h"

/* the frame's own auto range */
static void set_frame_range(const void* parameters, const ft_print_roll* roll,
                            const ft_print_frame* frame, ft_recipe* recipe) {
  (void)parameters;
  (void)roll;
  memcpy(recipe->range_density, frame->range_density,
         sizeof recipe->range_density);
}

const ft_film_module ft_film_module_classic = {
  .name = "classic", .measure_roll = NULL, .set_range = set_frame_range};

/* the auto exposure (highlight at paper white), no tint */
static void set_classic_balance(const void* parameters,
                                const ft_print_roll* roll,
                                const ft_print_frame* frame,
                                const ft_recipe* recipe,
                                ft_print_auto* automatic) {
  (void)parameters;
  (void)roll;
  (void)frame;
  memset(automatic, 0, sizeof *automatic);
  if(recipe->mode != FT_RENDER_SLIDE) {
    automatic->exposure = ft_estimate_auto_exposure(recipe);
  }
}

const ft_balance_module ft_balance_module_classic = {
  .name = "classic",
  .measure = NULL,
  .measure_roll = NULL,
  .set_frame = set_classic_balance};

static void set_paper(const void* parameters, ft_recipe* recipe) {
  const ft_paper_parameters* paper = parameters;
  if(paper == NULL) return;
  recipe->grade = paper->grade;
  recipe->paper_black = paper->black;
  recipe->paper_white = paper->white;
}

const ft_print_module ft_print_module_classic = {
  .name = "classic", .set_recipe = set_paper, .compile = NULL};

static void set_matrix(const void* parameters, ft_recipe* recipe) {
  if(parameters != NULL) {
    memcpy(recipe->matrix, parameters, sizeof recipe->matrix);
  }
}

const ft_output_module ft_output_module_classic = {.name = "classic",
                                                   .set_recipe = set_matrix};

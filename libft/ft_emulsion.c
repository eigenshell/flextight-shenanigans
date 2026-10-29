/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_emulsion.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ft_emulsion_luts.h"
#include "ft_pm.h"

/* A generic C-41 roll: one contrast for the roll (a green range of 1.2 D,
 * about a normally exposed scene on a 0.6-gamma film) with the dye
 * layers' ratio from the roll itself; the frontier balance; the classic
 * paper, on which e = 0.70 D prints as 18 % grey. Tuned on real rolls. */
static const ft_stock_film_parameters NEGATIVE_FILM = {
  .scale_density = 1.2f, .has_ratio = false, .ratio = {1, 1, 1}};
static const ft_frontier_parameters NEGATIVE_BALANCE = {
  .aim_density = 0.70,
  .density_correction = 0.9,
  .colour_correction = 0.6,
  /* 12 clicks */
  .clamp_density = 0.3,
  .saturation_limit = 0.2,
  .aim_colour = {0, 0, 0},
  .min_frames = 6};

static const ft_emulsion EMULSIONS[] = {
  {.id = "generic-neg",
   .name = "Generic colour negative",
   .mode = FT_RENDER_NEGATIVE,
   .film = &ft_film_module_stock,
   .film_parameters = &NEGATIVE_FILM,
   .balance = &ft_balance_module_frontier,
   .balance_parameters = &NEGATIVE_BALANCE,
   .print = &ft_print_module_classic,
   .print_parameters = NULL,
   .output = &ft_output_module_classic,
   .output_parameters = NULL},
  {.id = "generic-slide",
   .name = "Generic slide",
   .mode = FT_RENDER_SLIDE,
   .film = &ft_film_module_classic,
   .film_parameters = NULL,
   .balance = &ft_balance_module_classic,
   .balance_parameters = NULL,
   .print = &ft_print_module_classic,
   .print_parameters = NULL,
   .output = &ft_output_module_classic,
   .output_parameters = NULL},
  {.id = "generic-bw",
   .name = "Generic black and white",
   .mode = FT_RENDER_BW,
   .film = &ft_film_module_stock,
   .film_parameters = &NEGATIVE_FILM,
   .balance = &ft_balance_module_frontier,
   .balance_parameters = &NEGATIVE_BALANCE,
   .print = &ft_print_module_classic,
   .print_parameters = NULL,
   .output = &ft_output_module_classic,
   .output_parameters = NULL},
};
#define EMULSION_COUNT (sizeof EMULSIONS / sizeof EMULSIONS[0])

const ft_emulsion* ft_find_emulsion(const char* id) {
  for(size_t index = 0; index < EMULSION_COUNT; index++) {
    if(strcmp(EMULSIONS[index].id, id) == 0) return &EMULSIONS[index];
  }
  return NULL;
}

const ft_emulsion* ft_get_emulsion_at(uint32_t index) {
  return index < EMULSION_COUNT ? &EMULSIONS[index] : NULL;
}

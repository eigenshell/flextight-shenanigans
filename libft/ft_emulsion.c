/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_emulsion.h"

#include <string.h>

#include "ft_emulsion_luts.h"
#include "ft_pm.h"

/* A generic C-41 roll: one contrast for the roll (a green range of 1.2 D,
 * about a normally exposed scene on a 0.6-gamma film) with the dye
 * layers' ratio from the roll itself; the frontier balance; the classic
 * paper, on which e = 0.70 D prints as 18 % grey. Tuned on real rolls. */
static const ft_pm_stock_par NEG_FILM = {
  1.2f, 0, {1, 1, 1}
};
static const ft_pm_frontier_par NEG_BALANCE = {
  0.70, /* aim */
  0.9, /* k_d */
  0.6, /* k_c */
  0.3, /* clamp: 12 clicks */
  0.2, /* sat */
  {0, 0, 0}, /* aim_c */
  6  /* min_frames */
};

static const ft_emulsion EMULSIONS[] = {
  {"generic-neg",   "Generic colour negative", FT_RENDER_NEG,   &ft_pm_film_stock,
   &NEG_FILM,                                                                              &ft_pm_balance_frontier, &NEG_BALANCE, &ft_pm_print_classic, NULL,
   &ft_pm_output_classic, NULL},
  {"generic-slide", "Generic slide",           FT_RENDER_SLIDE, &ft_pm_film_classic, NULL,
   &ft_pm_balance_classic,                                                                                          NULL,         &ft_pm_print_classic, NULL,
   &ft_pm_output_classic, NULL},
  {"generic-bw",    "Generic black and white", FT_RENDER_BW,    &ft_pm_film_stock,
   &NEG_FILM,                                                                              &ft_pm_balance_frontier, &NEG_BALANCE, &ft_pm_print_classic, NULL,
   &ft_pm_output_classic, NULL},
};
#define N_EMULSIONS (int)(sizeof EMULSIONS / sizeof EMULSIONS[0])

const ft_emulsion* ft_emulsion_find(const char* id) {
  int i;
  for (i = 0; i < N_EMULSIONS; i++)
    if (strcmp(EMULSIONS[i].id, id) == 0) return &EMULSIONS[i];
  return NULL;
}

const ft_emulsion* ft_emulsion_at(int i) {
  return i >= 0 && i < N_EMULSIONS ? &EMULSIONS[i] : NULL;
}

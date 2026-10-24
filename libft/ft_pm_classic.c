/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The classic modules: today's render core (ft_render, ft_render_auto) as
 * print modules. An all-classic chain gives exactly ftdev export's recipe
 * without overrides. */
#include <string.h>

#include "ft_pm.h"
#include "ft_render_auto.h"

static void film_range(const void* par, const ft_print_roll* roll,
                       const ft_print_frame* fr, ft_recipe* r) {
  (void)par;
  (void)roll;
  memcpy(r->range, fr->range, sizeof r->range);
}

const ft_pm_film ft_pm_film_classic = {"classic", NULL, film_range};

static void balance_frame(const void* par, const ft_print_roll* roll,
                          const ft_print_frame* fr, const ft_recipe* r,
                          ft_print_auto* a) {
  (void)par;
  (void)roll;
  (void)fr;
  memset(a, 0, sizeof *a);
  if (r->mode != FT_RENDER_SLIDE) a->exposure = ft_render_auto_exposure(r);
}

const ft_pm_balance ft_pm_balance_classic = {"classic", NULL, NULL,
                                             balance_frame};

static void paper(const void* par, ft_recipe* r) {
  const ft_pm_paper_par* p = par;
  if (!p) return;
  r->grade = p->grade;
  r->paper_black = p->black;
  r->paper_white = p->white;
}

const ft_pm_print ft_pm_print_classic = {"classic", paper, NULL};

static void output(const void* par, ft_recipe* r) {
  if (par) memcpy(r->matrix, par, sizeof r->matrix);
}

const ft_pm_output ft_pm_output_classic = {"classic", output};

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The stock film module: one contrast for the whole roll, as film and
 * paper give it in a darkroom. range[c] = scale x ratio[c]: the scale is
 * the emulsion's, the ratio (how the dye layers' ranges compare) the
 * emulsion's or the roll's median. No frame is stretched to its own
 * range. */
#include "ft_pm.h"
#include <string.h>

static int stock_roll(
    const void *par, const ft_print_frame *fr, int n, ft_print_roll *roll,
    ft_err *e
) {
  const ft_pm_stock_par *p = par;
  double v[FT_MAX_FRAMES];
  int c, i;
  (void)e;
  if(roll->mode == FT_RENDER_SLIDE)
    return 0; /* a slide has no range */
  for(c = 0; c < 3; c++) {
    double ratio = 1;
    if(roll->mode == FT_RENDER_NEG && c != 1) {
      if(p->has_ratio) {
        ratio = p->ratio[c];
      } else {
        for(i = 0; i < n; i++)
          v[i] = fr[i].range[c] / fr[i].range[1];
        ratio = ft_print_median(v, n);
      }
    }
    roll->range[c] = (float)(p->scale * ratio);
  }
  roll->has_range = 1;
  return 0;
}

static void stock_range(
    const void *par, const ft_print_roll *roll, const ft_print_frame *fr,
    ft_recipe *r
) {
  (void)par;
  memcpy(r->range, roll->has_range ? roll->range : fr->range, sizeof r->range);
}

const ft_pm_film ft_pm_film_stock = {"stock", stock_roll, stock_range};

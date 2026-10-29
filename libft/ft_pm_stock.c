/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The stock film module: one contrast for the whole roll, as film and
 * paper give it in a darkroom. range[c] = scale x ratio[c]: the scale is
 * the emulsion's, the ratio (how the dye layers' ranges compare) the
 * emulsion's or the roll's median. No frame is stretched to its own
 * range. */
#include <stdint.h>
#include <string.h>

#include "ft_pm.h"

/* the green channel: the ratios are relative to it */
#define GREEN 1

static int32_t measure_stock_roll(const void* parameters,
                                  const ft_print_frame* frames,
                                  uint32_t frame_count, ft_print_roll* roll,
                                  ft_err* error) {
  const ft_stock_film_parameters* stock = parameters;
  (void)error;
  /* a slide has no range */
  if(roll->mode == FT_RENDER_SLIDE) return 0;
  double ratios[FT_MAX_FRAMES];
  for(uint32_t channel = 0; channel < 3; channel++) {
    double ratio = 1;
    if(roll->mode == FT_RENDER_NEGATIVE && channel != GREEN) {
      if(stock->has_ratio) {
        ratio = stock->ratio[channel];
      } else {
        for(uint32_t index = 0; index < frame_count; index++) {
          ratios[index] = frames[index].range_density[channel] /
                          frames[index].range_density[GREEN];
        }
        ratio = ft_get_print_median(ratios, frame_count);
      }
    }
    roll->range_density[channel] = (float)(stock->scale_density * ratio);
  }
  roll->has_range = true;
  return 0;
}

static void set_stock_range(const void* parameters, const ft_print_roll* roll,
                            const ft_print_frame* frame, ft_recipe* recipe) {
  (void)parameters;
  const float* range =
    roll->has_range ? roll->range_density : frame->range_density;
  memcpy(recipe->range_density, range, sizeof recipe->range_density);
}

const ft_film_module ft_film_module_stock = {.name = "stock",
                                             .measure_roll = measure_stock_roll,
                                             .set_range = set_stock_range};

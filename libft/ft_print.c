/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_print.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ft_pm.h"
#include "ft_render_auto.h"
#include "ft_resize.h"

/* the recipe's limits: exposure in stops, tint in CC */
#define MAX_EXPOSURE 6
#define MAX_TINT 50

double ft_get_print_median(double* values, uint32_t count) {
  /* insertion sort: the counts are small */
  for(uint32_t index = 1; index < count; index++) {
    double value = values[index];
    uint32_t position = index;
    while(position > 0 && values[position - 1] > value) {
      values[position] = values[position - 1];
      position--;
    }
    values[position] = value;
  }
  if(count % 2 == 1) return values[count / 2];
  return (values[count / 2 - 1] + values[count / 2]) / 2;
}

void ft_get_print_proxy_size(uint32_t width, uint32_t height,
                             uint32_t* proxy_width, uint32_t* proxy_height) {
  uint32_t long_side = width > height ? width : height;
  if(long_side <= FT_PRINT_STATS_EDGE_PIXELS) {
    *proxy_width = width;
    *proxy_height = height;
    return;
  }
  ft_get_resized_size(width, height,
                      (double)FT_PRINT_STATS_EDGE_PIXELS / long_side,
                      proxy_width, proxy_height);
}

/* The emulsion's recipe before film and balance: mode, base, paper,
 * matrix. */
static void make_skeleton(const ft_emulsion* emulsion, const float base[3],
                          ft_recipe* recipe) {
  ft_init_recipe(recipe, emulsion->mode);
  memcpy(recipe->base_density, base, sizeof recipe->base_density);
  emulsion->print->set_recipe(emulsion->print_parameters, recipe);
  emulsion->output->set_recipe(emulsion->output_parameters, recipe);
}

int32_t ft_measure_print_roll(const ft_emulsion* emulsion, const float base[3],
                              const ft_image* const* images,
                              uint32_t frame_count, ft_print_frame* frames,
                              ft_print_roll* roll, ft_err* error) {
  if(frame_count < 1 || frame_count > FT_MAX_FRAMES) {
    return ft_fail(error, "print: %u frames (1..%d)", (unsigned)frame_count,
                   FT_MAX_FRAMES);
  }
  memset(roll, 0, sizeof *roll);
  roll->mode = emulsion->mode;
  roll->frame_count = frame_count;
  ft_recipe skeleton;
  ft_init_recipe(&skeleton, emulsion->mode);
  /* b&w: folded to luminance */
  ft_set_recipe_base(&skeleton, base);
  memcpy(roll->base_density, skeleton.base_density, sizeof roll->base_density);
  make_skeleton(emulsion, roll->base_density, &skeleton);

  /* pass 1: each frame's own range */
  for(uint32_t index = 0; index < frame_count; index++) {
    const ft_image* one[1] = {images[index]};
    memset(&frames[index], 0, sizeof frames[index]);
    ft_recipe recipe = skeleton;
    if(ft_estimate_recipe(one, 1, &recipe, error) != 0) return -1;
    memcpy(frames[index].range_density, recipe.range_density,
           sizeof frames[index].range_density);
  }
  const ft_film_module* film = emulsion->film;
  if(film->measure_roll != NULL &&
     film->measure_roll(emulsion->film_parameters, frames, frame_count, roll,
                        error) != 0) {
    return -1;
  }
  /* pass 2: the balance's means */
  const ft_balance_module* balance = emulsion->balance;
  if(balance->measure != NULL) {
    for(uint32_t index = 0; index < frame_count; index++) {
      ft_recipe recipe = skeleton;
      film->set_range(emulsion->film_parameters, roll, &frames[index], &recipe);
      if(balance->measure(emulsion->balance_parameters, &recipe, images[index],
                          &frames[index], error) != 0) {
        return -1;
      }
    }
  }
  if(balance->measure_roll != NULL &&
     balance->measure_roll(emulsion->balance_parameters, frames, frame_count,
                           roll, error) != 0) {
    return -1;
  }
  return 0;
}

int32_t ft_check_print_clicks(const ft_emulsion* emulsion, ft_cmyd clicks,
                              ft_err* error) {
  const int32_t values[4] = {clicks.cyan, clicks.magenta, clicks.yellow,
                             clicks.density};
  static const char LETTERS[] = "CMYD";
  for(uint32_t index = 0; index < 4; index++) {
    if(values[index] < -FT_PRINT_CLICKS_MAX ||
       values[index] > FT_PRINT_CLICKS_MAX) {
      return ft_fail(error, "cmyd: %c %d is out of range (-%d..%d)",
                     LETTERS[index], (int)values[index], FT_PRINT_CLICKS_MAX,
                     FT_PRINT_CLICKS_MAX);
    }
  }
  bool has_colour =
    clicks.cyan != 0 || clicks.magenta != 0 || clicks.yellow != 0;
  if(emulsion->mode == FT_RENDER_BW && has_colour) {
    return ft_fail(error, "cmyd: %s is black and white: C, M and Y must be 0",
                   emulsion->id);
  }
  return 0;
}

/* value clamped to -limit..limit; *was_hit set when it was */
static float clamp_symmetric(float value, float limit, bool* was_hit) {
  if(value > limit) {
    *was_hit = true;
    return limit;
  }
  if(value < -limit) {
    *was_hit = true;
    return -limit;
  }
  return value;
}

int32_t ft_make_print_recipe(const ft_emulsion* emulsion,
                             const ft_print_roll* roll,
                             const ft_print_frame* frame, ft_cmyd clicks,
                             ft_recipe* recipe, ft_print_auto* automatic,
                             bool* was_clamped, ft_err* error) {
  const int32_t colour_clicks[3] = {clicks.cyan, clicks.magenta, clicks.yellow};
  if(ft_check_print_clicks(emulsion, clicks, error) != 0) return -1;
  make_skeleton(emulsion, roll->base_density, recipe);
  emulsion->film->set_range(emulsion->film_parameters, roll, frame, recipe);
  ft_print_auto auto_filtration;
  memset(&auto_filtration, 0, sizeof auto_filtration);
  emulsion->balance->set_frame(emulsion->balance_parameters, roll, frame,
                               recipe, &auto_filtration);
  if(automatic != NULL) *automatic = auto_filtration;
  /* A click is 0.025 D less print exposure (+D: denser) or less light of
   * the complementary colour (+C: less red, so more cyan). */
  bool was_hit = false;
  recipe->exposure = clamp_symmetric(
    auto_filtration.exposure -
      (float)(clicks.density * FT_PRINT_CLICK_DENSITY / log10(2)),
    MAX_EXPOSURE, &was_hit);
  for(uint32_t channel = 0; channel < 3; channel++) {
    if(emulsion->mode == FT_RENDER_BW) {
      recipe->tint[channel] = 0;
    } else {
      recipe->tint[channel] = clamp_symmetric(
        auto_filtration.tint[channel] -
          (float)(colour_clicks[channel] * 100 * FT_PRINT_CLICK_DENSITY),
        MAX_TINT, &was_hit);
    }
  }
  if(was_clamped != NULL) *was_clamped = was_hit;
  return 0;
}

int32_t ft_compile_print(const ft_emulsion* emulsion, const ft_recipe* recipe,
                         ft_print_render* render, ft_err* error) {
  if(emulsion->print->compile != NULL) {
    return emulsion->print->compile(emulsion->print_parameters, recipe, render,
                                    error);
  }
  render->is_lut = false;
  return ft_compile_render(recipe, &render->compiled.render, error);
}

void ft_apply_print(const ft_print_render* render, const float* input,
                    float* output, size_t pixel_count) {
  if(render->is_lut) {
    ft_apply_lut3d(&render->compiled.lut, input, output, pixel_count);
  } else {
    ft_apply_render(&render->compiled.render, input, output, pixel_count);
  }
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_render.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ITU-R BT.2087: linear sRGB (BT.709) -> linear Rec.2020 */
static const float SRGB_TO_2020[9] = {
  0.6274038959f, 0.3292830384f, 0.0433130657f, 0.0690972894f, 0.9195403951f,
  0.0113623156f, 0.0163914389f, 0.0880133079f, 0.8955952532f,
};
/* XYZ (D65) -> linear Rec.2020 */
static const double XYZ_TO_2020[9] = {
  1.7166511880, -0.3556707838, -0.2533662814, -0.6666843518, 1.6164812366,
  0.0157685458, 0.0176398574,  -0.0427706133, 0.9421031212,
};
/* the channel letters, for messages */
static const char CHANNEL_LETTERS[] = "rgb";
/* the green channel: b&w keeps its values there */
#define GREEN 1
/* the neutral recipe's slide black */
#define DEFAULT_SLIDE_BASE 3.0f

double ft_encode_srgb(double value) {
  if(!(value > 0)) return 0;
  if(value >= 1) return 1;
  if(value <= 0.0031308) return 12.92 * value;
  return 1.055 * pow(value, 1 / 2.4) - 0.055;
}

double ft_decode_srgb(double value) {
  if(!(value > 0)) return 0;
  if(value >= 1) return 1;
  if(value <= 0.04045) return value / 12.92;
  return pow((value + 0.055) / 1.055, 2.4);
}

void ft_init_recipe(ft_recipe* recipe, ft_render_mode mode) {
  memset(recipe, 0, sizeof *recipe);
  recipe->mode = mode;
  for(uint32_t channel = 0; channel < 3; channel++) {
    recipe->base_density[channel] =
      mode == FT_RENDER_SLIDE ? DEFAULT_SLIDE_BASE : 0;
    recipe->range_density[channel] = 1;
    ft_init_identity_curve(&recipe->channels[channel]);
  }
  recipe->grade = 1.5f;
  recipe->paper_black = 2.0f;
  recipe->paper_white = 0;
  /* Rec.709 luminance */
  recipe->bw_weights[0] = 0.2126f;
  recipe->bw_weights[1] = 0.7152f;
  recipe->bw_weights[2] = 0.0722f;
  memcpy(recipe->matrix, SRGB_TO_2020, sizeof recipe->matrix);
  ft_init_identity_curve(&recipe->master);
}

/* low <= value <= high; false for NaN */
static bool is_within(double value, double low, double high) {
  return value >= low && value <= high;
}

/* Returns 0, or -1 with error naming the curve. */
static int32_t check_curve(const ft_curve* curve, const char* name,
                           ft_err* error) {
  ft_err inner_error;
  if(ft_validate_curve(curve, &inner_error) == 0) return 0;
  return ft_fail(error, "recipe: %s %s", name, inner_error.message);
}

/* The per-channel values. Returns 0, or -1 with error filled. */
static int32_t check_channels(const ft_recipe* recipe, ft_err* error) {
  double weight_sum = 0;
  for(uint32_t channel = 0; channel < 3; channel++) {
    char letter = CHANNEL_LETTERS[channel];
    double base = recipe->base_density[channel];
    double range = recipe->range_density[channel];
    double tint = recipe->tint[channel];
    double weight = recipe->bw_weights[channel];
    if(is_within(base, 0, 4.5) == false) {
      return ft_fail(error, "recipe: base %c = %g (0..4.5 D)", letter, base);
    }
    if(recipe->mode == FT_RENDER_SLIDE && base < 0.1) {
      return ft_fail(error, "recipe: base %c = %g: a slide's black is >= 0.1 D",
                     letter, base);
    }
    if(is_within(range, 0.05, 4.5) == false) {
      return ft_fail(error, "recipe: range %c = %g (0.05..4.5 D)", letter,
                     range);
    }
    if(is_within(tint, -50, 50) == false) {
      return ft_fail(error, "recipe: tint %c = %g (-50..50 CC)", letter, tint);
    }
    if(recipe->mode == FT_RENDER_BW && tint != 0) {
      return ft_fail(error, "recipe: tint must be 0 for b&w");
    }
    if(is_within(weight, 0, 1) == false) {
      return ft_fail(error, "recipe: bw weight %c = %g (0..1)", letter, weight);
    }
    weight_sum += weight;
  }
  if(is_within(weight_sum, 1 - 1e-3, 1 + 1e-3) == false) {
    return ft_fail(error, "recipe: bw weights sum to %g, not 1", weight_sum);
  }
  return 0;
}

int32_t ft_validate_recipe(const ft_recipe* recipe, ft_err* error) {
  static const char* const CURVE_NAMES[3] = {"red curve", "green curve",
                                             "blue curve"};
  if((int)recipe->mode < FT_RENDER_NEGATIVE ||
     (int)recipe->mode > FT_RENDER_BW) {
    return ft_fail(error, "recipe: unknown mode %d", (int)recipe->mode);
  }
  if(check_channels(recipe, error) != 0) return -1;
  if(is_within(recipe->exposure, -6, 6) == false) {
    return ft_fail(error, "recipe: exposure %g (-6..6 stops)",
                   recipe->exposure);
  }
  if(is_within(recipe->grade, 0.2, 5) == false) {
    return ft_fail(error, "recipe: grade %g (0.2..5)", recipe->grade);
  }
  if(is_within(recipe->paper_white, 0, 4) == false ||
     is_within(recipe->paper_black, 0, 4) == false ||
     !(recipe->paper_black > recipe->paper_white)) {
    return ft_fail(error,
                   "recipe: paper white %g, black %g (0 <= white < "
                   "black <= 4 D)",
                   recipe->paper_white, recipe->paper_black);
  }
  for(uint32_t index = 0; index < 9; index++) {
    if(isfinite(recipe->matrix[index]) == false) {
      return ft_fail(error, "recipe: matrix entry %u is not finite",
                     (unsigned)index);
    }
  }
  if(check_curve(&recipe->master, "master curve", error) != 0) return -1;
  for(uint32_t channel = 0; channel < 3; channel++) {
    if(check_curve(&recipe->channels[channel], CURVE_NAMES[channel], error) !=
       0) {
      return -1;
    }
  }
  return 0;
}

void ft_set_recipe_base(ft_recipe* recipe, const float density[3]) {
  for(uint32_t channel = 0; channel < 3; channel++) {
    if(isfinite(density[channel]) == false) return;
  }
  if(recipe->mode != FT_RENDER_BW) {
    for(uint32_t channel = 0; channel < 3; channel++) {
      recipe->base_density[channel] = density[channel];
    }
    return;
  }
  /* b&w: the luminance of the three transmittances, as a density */
  double transmittance = 0;
  for(uint32_t channel = 0; channel < 3; channel++) {
    transmittance += recipe->bw_weights[channel] * pow(10, -density[channel]);
  }
  if(!(transmittance > FT_TRANSMITTANCE_FLOOR)) {
    transmittance = FT_TRANSMITTANCE_FLOOR;
  }
  float luminance_density = (float)-log10(transmittance);
  for(uint32_t channel = 0; channel < 3; channel++) {
    recipe->base_density[channel] = luminance_density;
  }
}

/* Film to linear for a channel (b&w: luminance, the green slots): the
 * negative's print model, or the slide's black point and gains. */
static double render_film(const ft_recipe* recipe, uint32_t channel,
                          double transmittance) {
  if(!(transmittance >= FT_TRANSMITTANCE_FLOOR)) {
    transmittance = FT_TRANSMITTANCE_FLOOR;
  }
  if(transmittance > FT_RENDER_MAX_TRANSMITTANCE) {
    transmittance = FT_RENDER_MAX_TRANSMITTANCE;
  }
  if(recipe->mode == FT_RENDER_SLIDE) {
    double black = pow(10, -recipe->base_density[channel]);
    return (transmittance - black) /
           (1 - black) *
           pow(2, recipe->exposure) *
           pow(10, recipe->tint[channel] / 100.0);
  }
  /* 0 = the film base, 1 = the brightest highlight */
  double normalised = (-log10(transmittance) - recipe->base_density[channel]) /
                      recipe->range_density[channel];
  /* print exposure: + exposure or + tint = less light on the paper */
  double lift = recipe->exposure * log10(2) + recipe->tint[channel] / 100.0;
  double position = 0.5 - normalised - lift / recipe->range_density[GREEN];
  /* the paper's sigmoid: 0 = white, 1 = black */
  double sigmoid = 1 / (1 + exp(-4 * recipe->grade * position));
  double paper_density =
    recipe->paper_white + (recipe->paper_black - recipe->paper_white) * sigmoid;
  return pow(10, -(paper_density - recipe->paper_white));
}

/* the curves, on the sRGB-encoded value; linear in, linear out */
static double apply_curves(const ft_recipe* recipe, uint32_t channel,
                           double value) {
  double encoded = ft_encode_srgb(value);
  encoded = ft_evaluate_curve(&recipe->master, encoded);
  if(recipe->mode != FT_RENDER_BW) {
    encoded = ft_evaluate_curve(&recipe->channels[channel], encoded);
  }
  return ft_decode_srgb(encoded);
}

void ft_render_pixel_directly(const ft_recipe* recipe, const float input[3],
                              float output[3]) {
  if(recipe->mode == FT_RENDER_BW) {
    double luminance = recipe->bw_weights[0] * (double)input[0] +
                       recipe->bw_weights[1] * (double)input[1] +
                       recipe->bw_weights[2] * (double)input[2];
    /* grey: the matrix cannot change it, so skip it (exactly grey) */
    float value =
      (float)apply_curves(recipe, GREEN, render_film(recipe, GREEN, luminance));
    output[0] = value;
    output[1] = value;
    output[2] = value;
    return;
  }
  double linear[3];
  for(uint32_t channel = 0; channel < 3; channel++) {
    linear[channel] = render_film(recipe, channel, input[channel]);
  }
  const float* m = recipe->matrix;
  for(uint32_t channel = 0; channel < 3; channel++) {
    double mixed = m[3 * channel] * linear[0] +
                   m[3 * channel + 1] * linear[1] +
                   m[3 * channel + 2] * linear[2];
    output[channel] = (float)apply_curves(recipe, channel, mixed);
  }
}

int32_t ft_compile_render(const ft_recipe* recipe, ft_render* render,
                          ft_err* error) {
  if(ft_validate_recipe(recipe, error) != 0) return -1;
  render->mode = recipe->mode;
  memcpy(render->bw_weights, recipe->bw_weights, sizeof render->bw_weights);
  memcpy(render->matrix, recipe->matrix, sizeof render->matrix);
  for(uint32_t channel = 0; channel < 3; channel++) {
    bool is_bw = recipe->mode == FT_RENDER_BW;
    /* b&w: apply reads only the green tables; zero the rest so the
     * struct contents stay deterministic */
    if(is_bw && channel != GREEN) {
      memset(render->input[channel], 0, sizeof render->input[channel]);
      memset(render->output[channel], 0, sizeof render->output[channel]);
      continue;
    }
    uint32_t source = is_bw ? GREEN : channel;
    for(uint32_t index = 0; index < FT_RENDER_INPUT_SIZE; index++) {
      /* node index is the float whose bits are (first key + index)
       * shifted up: memcpy reads those bits as a float */
      uint32_t bits = (FT_RENDER_INPUT_FIRST_KEY + index)
                      << FT_RENDER_INPUT_SHIFT;
      float transmittance;
      memcpy(&transmittance, &bits, sizeof transmittance);
      render->input[channel][index] =
        (float)render_film(recipe, source, transmittance);
    }
    for(uint32_t index = 0; index < FT_RENDER_CURVE_SIZE; index++) {
      /* entry index holds value x^2: the table is indexed by sqrt */
      double x = (double)index / (FT_RENDER_CURVE_SIZE - 1);
      render->output[channel][index] =
        (float)apply_curves(recipe, channel, x * x);
    }
  }
  return 0;
}

/* The input table: node k is the float with bits (first key + k) <<
 * SHIFT, so a float's bits >> SHIFT give its node, and its low SHIFT
 * mantissa bits are the exact linear position between two nodes. */
float ft_look_up_render_input(const float* table, float transmittance) {
  /* written as "not >=" so NaN and -inf go to the floor too */
  if(!(transmittance >= (float)FT_TRANSMITTANCE_FLOOR)) {
    transmittance = (float)FT_TRANSMITTANCE_FLOOR;
  }
  if(transmittance > FT_RENDER_MAX_TRANSMITTANCE) {
    transmittance = FT_RENDER_MAX_TRANSMITTANCE;
  }
  uint32_t bits;
  memcpy(&bits, &transmittance, sizeof bits);
  uint32_t node = (bits >> FT_RENDER_INPUT_SHIFT) - FT_RENDER_INPUT_FIRST_KEY;
  /* the low bits as a fraction 0..1 */
  float fraction = (float)(bits & ((1u << FT_RENDER_INPUT_SHIFT) - 1)) *
                   (1.0f / (1u << FT_RENDER_INPUT_SHIFT));
  return table[node] + (table[node + 1] - table[node]) * fraction;
}

/* The curve table, indexed by sqrt(value): fine steps in the shadows. */
static float look_up_output(const float* table, float value) {
  if(!(value > 0)) return table[0];
  if(value >= 1) return table[FT_RENDER_CURVE_SIZE - 1];
  float position = sqrtf(value) * (FT_RENDER_CURVE_SIZE - 1);
  int32_t index = (int32_t)position;
  if(index > FT_RENDER_CURVE_SIZE - 2) index = FT_RENDER_CURVE_SIZE - 2;
  return table[index] +
         (table[index + 1] - table[index]) * (position - (float)index);
}

void ft_apply_render(const ft_render* render, const float* input, float* output,
                     size_t pixel_count) {
  for(size_t pixel = 0; pixel < pixel_count; pixel++) {
    const float* source = input + 3 * pixel;
    float* target = output + 3 * pixel;
    if(render->mode == FT_RENDER_BW) {
      const float* w = render->bw_weights;
      float luminance = w[0] * source[0] + w[1] * source[1] + w[2] * source[2];
      float value = look_up_output(
        render->output[GREEN],
        ft_look_up_render_input(render->input[GREEN], luminance));
      target[0] = value;
      target[1] = value;
      target[2] = value;
      continue;
    }
    /* copied first: input and output may be the same buffer */
    float transmittance[3] = {source[0], source[1], source[2]};
    float linear[3];
    for(uint32_t channel = 0; channel < 3; channel++) {
      linear[channel] =
        ft_look_up_render_input(render->input[channel], transmittance[channel]);
    }
    const float* m = render->matrix;
    for(uint32_t channel = 0; channel < 3; channel++) {
      target[channel] = look_up_output(render->output[channel],
                                       m[3 * channel] * linear[0] +
                                         m[3 * channel + 1] * linear[1] +
                                         m[3 * channel + 2] * linear[2]);
    }
  }
}

void ft_render_image(const ft_render* render, ft_image* image) {
  ft_apply_render(render, image->pixels, image->pixels,
                  (size_t)image->width * image->height);
  for(uint32_t index = 0; index < 9; index++) {
    image->xyz_to_rgb[index] = XYZ_TO_2020[index];
  }
}

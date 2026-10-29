/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The lut3d print module: the paper (and any look) as a 3D table, e.g. a
 * preset fitted by measuring another tool's output. The table's input is
 * the print exposure e_c (D: green-referenced film density above base
 * plus the filtration, clicks included; larger = lighter print), mapped
 * from low_density..high_density to 0..1; its output sRGB-encoded RGB (sRGB
 * primaries), then the recipe's matrix to linear Rec.2020. Colour negatives
 * only. */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "ft_pm.h"

/* the green channel: densities are referenced to its range */
#define GREEN 1

/* lut3d has no paper fields: the table is the paper */
static void set_lut_recipe(const void* parameters, ft_recipe* recipe) {
  (void)parameters;
  (void)recipe;
}

static int32_t compile_lut(const void* parameters, const ft_recipe* recipe,
                           ft_print_render* render, ft_err* error) {
  const ft_lut3d_parameters* lut3d = parameters;
  ft_print_lut* lut = &render->compiled.lut;
  if(ft_validate_recipe(recipe, error) != 0) return -1;
  if(recipe->mode != FT_RENDER_NEGATIVE) {
    return ft_fail(error, "lut3d: colour negatives only");
  }
  if(lut3d->cube == NULL ||
     lut3d->cube->size < 2 ||
     !(lut3d->high_density > lut3d->low_density)) {
    return ft_fail(error, "lut3d: bad table");
  }
  int32_t size = lut3d->cube->size;
  render->is_lut = true;
  lut->cube = lut3d->cube;
  memcpy(lut->matrix, recipe->matrix, sizeof lut->matrix);
  for(uint32_t channel = 0; channel < 3; channel++) {
    double lift = recipe->exposure * log10(2) + recipe->tint[channel] / 100.0;
    for(uint32_t index = 0; index < FT_RENDER_INPUT_SIZE; index++) {
      /* the input node's transmittance: see ft_compile_render */
      uint32_t bits = (FT_RENDER_INPUT_FIRST_KEY + index)
                      << FT_RENDER_INPUT_SHIFT;
      float node;
      memcpy(&node, &bits, sizeof node);
      double transmittance = node;
      if(transmittance > FT_RENDER_MAX_TRANSMITTANCE) {
        transmittance = FT_RENDER_MAX_TRANSMITTANCE;
      }
      /* the print exposure e, then its place 0..1 in the table */
      double exposure =
        (-log10(transmittance) - recipe->base_density[channel]) /
          recipe->range_density[channel] *
          recipe->range_density[GREEN] +
        lift;
      double position = (exposure - lut3d->low_density) /
                        (lut3d->high_density - lut3d->low_density);
      if(position < 0) position = 0;
      if(position > 1) position = 1;
      lut->input[channel][index] = (float)(position * (size - 1));
    }
  }
  for(uint32_t index = 0; index <= FT_PRINT_DECODE_SIZE; index++) {
    lut->decode[index] =
      (float)ft_decode_srgb((double)index / FT_PRINT_DECODE_SIZE);
  }
  return 0;
}

const ft_print_module ft_print_module_lut3d = {
  .name = "lut3d", .set_recipe = set_lut_recipe, .compile = compile_lut};

/* Tetrahedral interpolation at position (0..size-1 per axis): the cube
 * cell is split into six tetrahedra by the order of the fractions, and
 * the value is a weighted sum along the edges of the one that holds the
 * point. */
static void interpolate_tetrahedral(const ft_cube* cube,
                                    const float position[3], float out[3]) {
  int32_t size = cube->size;
  int32_t corner[3];
  float fraction[3];
  for(uint32_t axis = 0; axis < 3; axis++) {
    corner[axis] = (int32_t)position[axis];
    if(corner[axis] > size - 2) corner[axis] = size - 2;
    fraction[axis] = position[axis] - (float)corner[axis];
  }
  /* the node at corner + (red, green, blue), red index fastest */
#define NODE(red, green, blue)                                               \
  (cube->nodes +                                                             \
   3 * (((size_t)(corner[2] + (blue)) * size + corner[1] + (green)) * size + \
        corner[0] +                                                          \
        (red)))
  const float* c000 = NODE(0, 0, 0);
  const float* c100 = NODE(1, 0, 0);
  const float* c010 = NODE(0, 1, 0);
  const float* c001 = NODE(0, 0, 1);
  const float* c110 = NODE(1, 1, 0);
  const float* c101 = NODE(1, 0, 1);
  const float* c011 = NODE(0, 1, 1);
  const float* c111 = NODE(1, 1, 1);
#undef NODE
  float r = fraction[0];
  float g = fraction[1];
  float b = fraction[2];
  for(uint32_t channel = 0; channel < 3; channel++) {
    uint32_t k = channel;
    if(r >= g) {
      if(g >= b) {
        out[k] = c000[k] +
                 r * (c100[k] - c000[k]) +
                 g * (c110[k] - c100[k]) +
                 b * (c111[k] - c110[k]);
      } else if(r >= b) {
        out[k] = c000[k] +
                 r * (c100[k] - c000[k]) +
                 b * (c101[k] - c100[k]) +
                 g * (c111[k] - c101[k]);
      } else {
        out[k] = c000[k] +
                 b * (c001[k] - c000[k]) +
                 r * (c101[k] - c001[k]) +
                 g * (c111[k] - c101[k]);
      }
    } else {
      if(b > g) {
        out[k] = c000[k] +
                 b * (c001[k] - c000[k]) +
                 g * (c011[k] - c001[k]) +
                 r * (c111[k] - c011[k]);
      } else if(b > r) {
        out[k] = c000[k] +
                 g * (c010[k] - c000[k]) +
                 b * (c011[k] - c010[k]) +
                 r * (c111[k] - c011[k]);
      } else {
        out[k] = c000[k] +
                 g * (c010[k] - c000[k]) +
                 r * (c110[k] - c010[k]) +
                 b * (c111[k] - c110[k]);
      }
    }
  }
}

/* sRGB decode through the table, linear between entries */
static float decode_value(const float* table, float value) {
  if(!(value > 0)) return 0;
  if(value >= 1) return table[FT_PRINT_DECODE_SIZE];
  float position = value * FT_PRINT_DECODE_SIZE;
  int32_t index = (int32_t)position;
  return table[index] +
         (table[index + 1] - table[index]) * (position - (float)index);
}

void ft_apply_lut3d(const ft_print_lut* lut, const float* input, float* output,
                    size_t pixel_count) {
  const float* m = lut->matrix;
  for(size_t pixel = 0; pixel < pixel_count; pixel++) {
    const float* source = input + 3 * pixel;
    float* target = output + 3 * pixel;
    float position[3];
    float encoded[3];
    float linear[3];
    for(uint32_t channel = 0; channel < 3; channel++) {
      position[channel] =
        ft_look_up_render_input(lut->input[channel], source[channel]);
    }
    interpolate_tetrahedral(lut->cube, position, encoded);
    for(uint32_t channel = 0; channel < 3; channel++) {
      linear[channel] = decode_value(lut->decode, encoded[channel]);
    }
    for(uint32_t channel = 0; channel < 3; channel++) {
      float value = m[3 * channel] * linear[0] +
                    m[3 * channel + 1] * linear[1] +
                    m[3 * channel + 2] * linear[2];
      if(!(value > 0)) {
        value = 0;
      } else if(!(value < 1)) {
        value = 1;
      }
      target[channel] = value;
    }
  }
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The frontier balance: how photofinishing printers set density and
 * colour per frame. Each frame's large-area mean film density per channel
 * (strongly coloured areas left out, so a lawn or a red wall barely
 * counts) is compared with the roll: the roll's median is printed at the
 * aim in full, a frame's deviation from it only by the slopes
 * density_correction and colour_correction (< 1, at most clamp_density
 * D on any channel), so a
 * sunset keeps much of its warmth. Film density d_c is green-referenced D
 * above base (the recipe's n_c x range[green]); the print exposure e_c = d_c +
 * lift_c, lift = exposure log10(2) + tint / 100 (ft_render.c). */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ft_pm.h"

/* means on 4x4 blocks: the noise / 4 */
#define BLOCK 4
/* 2 % in from each edge (the rebate) */
#define EDGE_FRACTION 50
/* open beam in all three channels */
#define CLEAR_TRANSMITTANCE 0.9
/* 4.0 D in all three: not film */
#define OPAQUE_TRANSMITTANCE 1e-4
/* the green channel: densities are referenced to its range */
#define GREEN 1

/* the mean of a block's 3 x 16 values; false when one is not finite */
static bool measure_block(const ft_image* image, uint32_t x, uint32_t y,
                          double means[3]) {
  means[0] = 0;
  means[1] = 0;
  means[2] = 0;
  bool is_finite = true;
  for(uint32_t row = 0; row < BLOCK; row++) {
    for(uint32_t column = 0; column < BLOCK; column++) {
      const float* pixel =
        image->pixels + 3 * ((size_t)(y + row) * image->width + x + column);
      for(uint32_t channel = 0; channel < 3; channel++) {
        if(isfinite(pixel[channel]) == false) is_finite = false;
        means[channel] += pixel[channel];
      }
    }
  }
  for(uint32_t channel = 0; channel < 3; channel++) {
    means[channel] /= BLOCK * BLOCK;
  }
  return is_finite;
}

/* -log10, floored at the transmittance floor */
static double get_density(double transmittance) {
  if(!(transmittance > FT_TRANSMITTANCE_FLOOR)) {
    transmittance = FT_TRANSMITTANCE_FLOOR;
  }
  return -log10(transmittance);
}

static int32_t measure_frontier(const void* parameters, const ft_recipe* recipe,
                                const ft_image* image, ft_print_frame* frame,
                                ft_err* error) {
  const ft_frontier_parameters* frontier = parameters;
  bool is_bw = recipe->mode == FT_RENDER_BW;
  if(recipe->mode == FT_RENDER_SLIDE) {
    return ft_fail(error, "frontier: colour negatives and b&w only");
  }
  /* the sums over the kept (not saturated) blocks and over all blocks */
  double kept_sums[3] = {0, 0, 0};
  double all_sums[3] = {0, 0, 0};
  uint64_t kept_count = 0;
  uint64_t all_count = 0;
  uint32_t first_x = image->width / EDGE_FRACTION;
  uint32_t end_x = image->width - image->width / EDGE_FRACTION;
  uint32_t first_y = image->height / EDGE_FRACTION;
  uint32_t end_y = image->height - image->height / EDGE_FRACTION;
  for(uint32_t y = first_y; y + BLOCK <= end_y; y += BLOCK) {
    for(uint32_t x = first_x; x + BLOCK <= end_x; x += BLOCK) {
      double means[3];
      if(measure_block(image, x, y, means) == false) continue;
      bool is_clear = means[0] > CLEAR_TRANSMITTANCE &&
                      means[1] > CLEAR_TRANSMITTANCE &&
                      means[2] > CLEAR_TRANSMITTANCE;
      bool is_opaque = means[0] < OPAQUE_TRANSMITTANCE &&
                       means[1] < OPAQUE_TRANSMITTANCE &&
                       means[2] < OPAQUE_TRANSMITTANCE;
      if(is_clear || is_opaque) continue;
      /* green-referenced density above base */
      double densities[3];
      if(is_bw) {
        double luminance = recipe->bw_weights[0] * means[0] +
                           recipe->bw_weights[1] * means[1] +
                           recipe->bw_weights[2] * means[2];
        double density = get_density(luminance) - recipe->base_density[GREEN];
        densities[0] = density;
        densities[1] = density;
        densities[2] = density;
      } else {
        for(uint32_t channel = 0; channel < 3; channel++) {
          densities[channel] =
            (get_density(means[channel]) - recipe->base_density[channel]) /
            recipe->range_density[channel] *
            recipe->range_density[GREEN];
        }
      }
      double mean = (densities[0] + densities[1] + densities[2]) / 3;
      all_count++;
      for(uint32_t channel = 0; channel < 3; channel++) {
        all_sums[channel] += densities[channel];
      }
      /* strongly coloured blocks are left out of the kept means */
      double limit = frontier->saturation_limit;
      if(fabs(densities[0] - mean) > limit ||
         fabs(densities[1] - mean) > limit ||
         fabs(densities[2] - mean) > limit) {
        continue;
      }
      kept_count++;
      for(uint32_t channel = 0; channel < 3; channel++) {
        kept_sums[channel] += densities[channel];
      }
    }
  }
  if(all_count == 0) {
    return ft_fail(error, "frontier: no film pixels to measure");
  }
  for(uint32_t channel = 0; channel < 3; channel++) {
    /* all saturated: use everything */
    if(kept_count > 0) {
      frame->mean_density[channel] = kept_sums[channel] / kept_count;
    } else {
      frame->mean_density[channel] = all_sums[channel] / all_count;
    }
  }
  frame->has_mean = true;
  return 0;
}

/* the neutral part: the mean of the three channels */
static double get_neutral(const double values[3]) {
  return (values[0] + values[1] + values[2]) / 3;
}

static int32_t measure_frontier_roll(const void* parameters,
                                     const ft_print_frame* frames,
                                     uint32_t frame_count, ft_print_roll* roll,
                                     ft_err* error) {
  const ft_frontier_parameters* frontier = parameters;
  double values[FT_MAX_FRAMES];
  (void)error;
  for(uint32_t index = 0; index < frame_count; index++) {
    values[index] = get_neutral(frames[index].mean_density);
  }
  roll->reference_density = ft_get_print_median(values, frame_count);
  for(uint32_t channel = 0; channel < 3; channel++) {
    if(frame_count < frontier->min_frames || roll->mode == FT_RENDER_BW) {
      roll->reference_colour[channel] =
        roll->mode == FT_RENDER_BW ? 0 : frontier->aim_colour[channel];
      continue;
    }
    for(uint32_t index = 0; index < frame_count; index++) {
      values[index] = frames[index].mean_density[channel] -
                      get_neutral(frames[index].mean_density);
    }
    roll->reference_colour[channel] = ft_get_print_median(values, frame_count);
  }
  roll->has_reference = true;
  return 0;
}

static void set_frontier_frame(const void* parameters,
                               const ft_print_roll* roll,
                               const ft_print_frame* frame,
                               const ft_recipe* recipe,
                               ft_print_auto* automatic) {
  const ft_frontier_parameters* frontier = parameters;
  (void)recipe;
  memset(automatic, 0, sizeof *automatic);
  double frame_neutral = get_neutral(frame->mean_density);
  /* density: the roll at the aim, the frame's deviation by the density
   * correction */
  double density_lift =
    (frontier->aim_density - roll->reference_density) +
    frontier->density_correction * (roll->reference_density - frame_neutral);
  automatic->exposure = (float)(density_lift / log10(2));
  if(roll->mode == FT_RENDER_BW) return;

  /* Colour (centred: density is above): the roll's cast in full, the
   * frame's own deviation by the colour correction, scaled down as a
   * whole so that no channel moves more than the clamp (the correction
   * keeps its hue). */
  double reference_neutral = get_neutral(roll->reference_colour);
  double deviations[3];
  for(uint32_t channel = 0; channel < 3; channel++) {
    deviations[channel] = (frame->mean_density[channel] - frame_neutral) -
                          (roll->reference_colour[channel] - reference_neutral);
  }
  double deviation_neutral = get_neutral(deviations);
  double corrections[3];
  double largest = 0;
  for(uint32_t channel = 0; channel < 3; channel++) {
    corrections[channel] =
      frontier->colour_correction * (deviations[channel] - deviation_neutral);
    if(fabs(corrections[channel]) > largest) {
      largest = fabs(corrections[channel]);
    }
  }
  for(uint32_t channel = 0; channel < 3; channel++) {
    if(largest > frontier->clamp_density) {
      corrections[channel] *= frontier->clamp_density / largest;
    }
    automatic->tint[channel] =
      (float)(-100 * ((roll->reference_colour[channel] - reference_neutral) +
                      corrections[channel]));
  }
}

const ft_balance_module ft_balance_module_frontier = {
  .name = "frontier",
  .measure = measure_frontier,
  .measure_roll = measure_frontier_roll,
  .set_frame = set_frontier_frame};

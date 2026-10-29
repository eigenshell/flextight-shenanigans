/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_density.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/* the largest code and stored value */
#define CODE_MAX 65535
/* the densest film base the tables take */
#define MAX_BASE_DENSITY 4.5

void ft_quantise_transmittance16(const float* transmittance, uint16_t* stored,
                                 size_t count) {
  for(size_t index = 0; index < count; index++) {
    float value = transmittance[index];
    /* written as "not > 0" so that NaN gives 0 too */
    if(!(value > 0)) {
      stored[index] = 0;
    } else if(value >= 1) {
      stored[index] = CODE_MAX;
    } else {
      /* + 0.5 rounds to the nearest */
      stored[index] = (uint16_t)(value * 65535.0f + 0.5f);
    }
  }
}

void ft_init_density_encoder(ft_density_encoder* encoder,
                             const double base_density[3]) {
  for(uint32_t channel = 0; channel < 3; channel++) {
    double base = base_density[channel];
    if(!(base >= 0)) base = 0;
    if(base > MAX_BASE_DENSITY) base = MAX_BASE_DENSITY;
    encoder->base_density[channel] = base;
    /* stored 0: no light, the densest code */
    encoder->tables[channel][0] = CODE_MAX;
    for(uint32_t stored = 1; stored <= CODE_MAX; stored++) {
      double density = -log10(stored / 65535.0);
      double code = floor(
        (density - base + FT_DENSITY_OFFSET) / FT_DENSITY_RANGE * 65535.0 +
        0.5);
      if(code < 0) code = 0;
      if(code > CODE_MAX) code = CODE_MAX;
      encoder->tables[channel][stored] = (uint16_t)code;
    }
  }
}

void ft_encode_density_rgb(const ft_density_encoder* encoder,
                           const uint16_t* stored, uint16_t* codes,
                           size_t pixel_count) {
  for(size_t pixel = 0; pixel < pixel_count; pixel++) {
    codes[3 * pixel + 0] = encoder->tables[0][stored[3 * pixel + 0]];
    codes[3 * pixel + 1] = encoder->tables[1][stored[3 * pixel + 1]];
    codes[3 * pixel + 2] = encoder->tables[2][stored[3 * pixel + 2]];
  }
}

void ft_encode_density_mono(const ft_density_encoder* encoder,
                            const double weights[3], const uint16_t* stored,
                            uint16_t* codes, size_t pixel_count) {
  float red_weight = (float)weights[0];
  float green_weight = (float)weights[1];
  float blue_weight = (float)weights[2];
  for(size_t pixel = 0; pixel < pixel_count; pixel++) {
    /* + 0.5f, then the cast truncates: rounding to the nearest */
    float value = red_weight * encoder->tables[0][stored[3 * pixel + 0]] +
                  green_weight * encoder->tables[1][stored[3 * pixel + 1]] +
                  blue_weight * encoder->tables[2][stored[3 * pixel + 2]] +
                  0.5f;
    codes[pixel] = (uint16_t)(value >= 65535.0f ? CODE_MAX : value);
  }
}

void ft_compute_noise_weights(const double variance[3], double weights[3]) {
  bool are_usable = true;
  for(uint32_t channel = 0; channel < 3; channel++) {
    if(!(variance[channel] > 0) || isfinite(variance[channel]) == false) {
      are_usable = false;
    }
  }
  if(are_usable == false) {
    weights[0] = 1.0 / 3;
    weights[1] = 1.0 / 3;
    weights[2] = 1.0 / 3;
    return;
  }
  double inverse_sum = 0;
  for(uint32_t channel = 0; channel < 3; channel++) {
    inverse_sum += 1 / variance[channel];
  }
  for(uint32_t channel = 0; channel < 3; channel++) {
    weights[channel] = 1 / variance[channel] / inverse_sum;
  }
}

double ft_get_density_from_code(uint32_t code) {
  return code / 65535.0 * FT_DENSITY_RANGE - FT_DENSITY_OFFSET;
}

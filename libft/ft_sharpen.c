/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_sharpen.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* the largest kernel half-width: ceil(3 x 0.8) */
#define MAX_HALF_WIDTH 3
/* Rec.2020 luminance weights */
#define LUMINANCE_RED 0.2627
#define LUMINANCE_GREEN 0.6780
#define LUMINANCE_BLUE 0.0593
/* pixels this dark (Lp) are left alone */
#define MIN_LIGHTNESS 1e-6f

size_t ft_get_sharpen_scratch_bytes(uint32_t width, uint32_t height) {
  /* lightness, blur, and one row */
  return ((size_t)2 * width * height + width) * sizeof(float);
}

/* Mirror an index into 0..count-1: -1 -> 1, count -> count - 2. */
static uint32_t reflect(int64_t index, uint32_t count) {
  while(index < 0 || index >= count) {
    if(index < 0) {
      index = -index;
    } else {
      index = 2 * (int64_t)count - 2 - index;
    }
  }
  return (uint32_t)index;
}

void ft_sharpen(float* pixels, uint32_t width, uint32_t height, double amount,
                double scale, void* scratch) {
  if(!(amount > 0) || width < 1 || height < 1) return;
  size_t pixel_count = (size_t)width * height;
  float* lightness = scratch;
  float* blurred = lightness + pixel_count;
  float* row = blurred + pixel_count;
  if(!(scale > 0) || scale > 1) scale = 1;

  /* the Gaussian kernel, normalised to sum 1 */
  double extra = 1 / scale - 1 < 1 ? 1 / scale - 1 : 1;
  double radius = 0.5 + 0.3 * extra;
  int32_t half_width = (int32_t)ceil(3 * radius);
  float kernel[2 * MAX_HALF_WIDTH + 1];
  double kernel_sum = 0;
  for(int32_t tap = -half_width; tap <= half_width; tap++) {
    kernel[tap + half_width] = (float)exp(-0.5 * tap * tap / (radius * radius));
    kernel_sum += kernel[tap + half_width];
  }
  for(int32_t tap = 0; tap <= 2 * half_width; tap++) {
    kernel[tap] = (float)(kernel[tap] / kernel_sum);
  }

  for(size_t index = 0; index < 3 * pixel_count; index++) {
    if(isfinite(pixels[index]) == false) pixels[index] = 0;
  }
  /* perceptual lightness: the cube root of luminance */
  for(size_t index = 0; index < pixel_count; index++) {
    double luminance = LUMINANCE_RED * pixels[3 * index] +
                       LUMINANCE_GREEN * pixels[3 * index + 1] +
                       LUMINANCE_BLUE * pixels[3 * index + 2];
    lightness[index] = (float)cbrt(luminance);
  }
  /* blur: vertical lightness -> blurred, then horizontal in place
   * through row */
  for(uint32_t y = 0; y < height; y++) {
    for(uint32_t x = 0; x < width; x++) {
      float value = 0;
      for(int32_t tap = -half_width; tap <= half_width; tap++) {
        value +=
          kernel[tap + half_width] *
          lightness[(size_t)reflect((int64_t)y + tap, height) * width + x];
      }
      blurred[(size_t)y * width + x] = value;
    }
  }
  for(uint32_t y = 0; y < height; y++) {
    float* blurred_row = blurred + (size_t)y * width;
    memcpy(row, blurred_row, (size_t)width * sizeof *row);
    for(uint32_t x = 0; x < width; x++) {
      float value = 0;
      for(int32_t tap = -half_width; tap <= half_width; tap++) {
        value +=
          kernel[tap + half_width] * row[reflect((int64_t)x + tap, width)];
      }
      blurred_row[x] = value;
    }
  }

  for(uint32_t y = 0; y < height; y++) {
    for(uint32_t x = 0; x < width; x++) {
      size_t at = (size_t)y * width + x;
      float light = lightness[at];
      if(!(light > MIN_LIGHTNESS)) continue;
      /* the soft threshold: detail below it is grain, kept as it is */
      float detail = light - blurred[at];
      if(detail > FT_SHARPEN_THRESHOLD) {
        detail = detail - (float)FT_SHARPEN_THRESHOLD;
      } else if(detail < -FT_SHARPEN_THRESHOLD) {
        detail = detail + (float)FT_SHARPEN_THRESHOLD;
      } else {
        detail = 0.f;
      }
      if(detail == 0) continue;
      /* the 3 x 3 range around the pixel */
      float lowest = light;
      float highest = light;
      for(int32_t dy = -1; dy <= 1; dy++) {
        for(int32_t dx = -1; dx <= 1; dx++) {
          float value =
            lightness[(size_t)reflect((int64_t)y + dy, height) * width +
                      reflect((int64_t)x + dx, width)];
          if(value < lowest) lowest = value;
          if(value > highest) highest = value;
        }
      }
      float sharpened = light + (float)amount * detail;
      /* clamped to that range, a little widened: no halos */
      float margin = (float)FT_SHARPEN_CLAMP * (highest - lowest);
      if(sharpened > highest + margin) sharpened = highest + margin;
      if(sharpened < lowest - margin) sharpened = lowest - margin;
      if(sharpened < 0) sharpened = 0;
      /* scale RGB by the luminance ratio, (Lp' / Lp)^3: hue and
       * saturation stay */
      float factor = sharpened / light;
      factor = factor * factor * factor;
      pixels[3 * at] *= factor;
      pixels[3 * at + 1] *= factor;
      pixels[3 * at + 2] *= factor;
    }
  }
}

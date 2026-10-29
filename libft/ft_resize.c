/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_resize.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define PI 3.14159265358979323846
/* Lanczos-3: three lobes each side */
#define LOBES 3

/* the Lanczos kernel: sinc(x) sinc(x / LOBES), 0 past LOBES */
static double get_lanczos(double x) {
  if(x < 0) x = -x;
  if(x < 1e-9) return 1;
  if(x >= LOBES) return 0;
  double angle = PI * x;
  return LOBES * sin(angle) * sin(angle / LOBES) / (angle * angle);
}

void ft_get_resized_size(uint32_t width, uint32_t height, double scale,
                         uint32_t* output_width, uint32_t* output_height) {
  double scaled_width = floor(width * scale + 0.5);
  double scaled_height = floor(height * scale + 0.5);
  *output_width = scaled_width < 1 ? 1 : (uint32_t)scaled_width;
  *output_height = scaled_height < 1 ? 1 : (uint32_t)scaled_height;
}

size_t ft_get_resize_scratch_bytes(uint32_t width, uint32_t height,
                                   uint32_t output_width,
                                   uint32_t output_height) {
  /* the horizontal pass's result: height rows of output_width pixels */
  (void)width;
  (void)output_height;
  return (size_t)height * output_width * 3 * sizeof(float);
}

/* One output sample along a run of input_count inputs (stride floats
 * apart): the filter centred on output pixel index of output_count; RGB
 * at once. */
static void sample(const float* source, uint32_t input_count, size_t stride,
                   uint32_t index, uint32_t output_count, float out[3]) {
  /* <= 1 */
  double scale = (double)output_count / input_count;
  /* the centre in input pixels */
  double centre = (index + 0.5) / scale - 0.5;
  double radius = LOBES / scale;
  int64_t first = (int64_t)ceil(centre - radius);
  int64_t last = (int64_t)floor(centre + radius);
  if(first < 0) first = 0;
  if(last > (int64_t)input_count - 1) last = input_count - 1;
  double sums[3] = {0, 0, 0};
  double weight_sum = 0;
  for(int64_t input = first; input <= last; input++) {
    double weight = get_lanczos((input - centre) * scale);
    const float* pixel = source + (size_t)input * stride;
    sums[0] += weight * pixel[0];
    sums[1] += weight * pixel[1];
    sums[2] += weight * pixel[2];
    weight_sum += weight;
  }
  if(weight_sum == 0) weight_sum = 1;
  out[0] = (float)(sums[0] / weight_sum);
  out[1] = (float)(sums[1] / weight_sum);
  out[2] = (float)(sums[2] / weight_sum);
}

int32_t ft_resize(const float* input, uint32_t width, uint32_t height,
                  float* output, uint32_t output_width, uint32_t output_height,
                  void* scratch, ft_err* error) {
  float* rows = scratch;
  if(width < 1 || height < 1 || output_width < 1 || output_height < 1) {
    return ft_fail(error, "resize: empty image");
  }
  if(output_width > width || output_height > height) {
    return ft_fail(error, "resize: %ux%u -> %ux%u would upscale",
                   (unsigned)width, (unsigned)height, (unsigned)output_width,
                   (unsigned)output_height);
  }
  if(output_width == width && output_height == height) {
    memmove(output, input, (size_t)width * height * 3 * sizeof *output);
    return 0;
  }
  /* rows: width -> output_width */
  for(uint32_t y = 0; y < height; y++) {
    for(uint32_t x = 0; x < output_width; x++) {
      sample(input + (size_t)y * width * 3, width, 3, x, output_width,
             rows + ((size_t)y * output_width + x) * 3);
    }
  }
  /* columns: height -> output_height */
  for(uint32_t x = 0; x < output_width; x++) {
    for(uint32_t y = 0; y < output_height; y++) {
      sample(rows + (size_t)x * 3, height, (size_t)output_width * 3, y,
             output_height, output + ((size_t)y * output_width + x) * 3);
    }
  }
  return 0;
}

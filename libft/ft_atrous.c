/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_atrous.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

/* sqrt(sum((a_j - a_{j+1})^2)) over the 2-D equivalent filters a_j of
 * c_j: the exact per-scale std for unit white noise */
const double FT_ATROUS_NOISE_STD[FT_ATROUS_SCALES] = {
  0.890796, 0.200664, 0.085508, 0.041217, 0.020425};
const double FT_ATROUS_DEFAULT_WEIGHTS[FT_ATROUS_SCALES] = {1.0, 0.8, 0.5, 0.25,
                                                            0.1};

/* the B3 spline kernel [1 4 6 4 1] / 16 */
static const float B3[5] = {1.f / 16, 4.f / 16, 6.f / 16, 4.f / 16, 1.f / 16};

/* Mirror without repeating the edge sample. One reflection is enough for
 * |index| < count + count - 1 (count >= FT_ATROUS_MIN_SIDE covers the
 * hole 16 x 2). */
static uint32_t reflect(int64_t index, uint32_t count) {
  if(index < 0) return (uint32_t)-index;
  if(index >= count) return (uint32_t)(2 * (int64_t)count - 2 - index);
  return (uint32_t)index;
}

/* NaN and inf count as 0 */
static float clean(float value) { return isfinite(value) ? value : 0.f; }

size_t ft_get_atrous_scratch_bytes(uint32_t width, uint32_t band_height) {
  /* two planes of the band and its halos, and one row */
  size_t rows = (size_t)band_height + 2 * FT_ATROUS_HALO_ROWS;
  return ((size_t)2 * rows * width + width) * sizeof(float);
}

void ft_smooth_atrous(const float* input, uint32_t width, uint32_t height,
                      uint32_t scale, float* output, float* row) {
  /* the distance between taps: 2^scale */
  int64_t spacing = (int64_t)1 << scale;
  /* vertical input -> output, then horizontal in place through row */
  for(uint32_t y = 0; y < height; y++) {
    const float* rows[5];
    float* target = output + (size_t)y * width;
    for(int64_t tap = 0; tap < 5; tap++) {
      rows[tap] =
        input + (size_t)reflect(y + (tap - 2) * spacing, height) * width;
    }
    for(uint32_t x = 0; x < width; x++) {
      target[x] = B3[0] * rows[0][x] +
                  B3[1] * rows[1][x] +
                  B3[2] * rows[2][x] +
                  B3[3] * rows[3][x] +
                  B3[4] * rows[4][x];
    }
  }
  for(uint32_t y = 0; y < height; y++) {
    float* target = output + (size_t)y * width;
    memcpy(row, target, (size_t)width * sizeof *row);
    for(uint32_t x = 0; x < width; x++) {
      target[x] = B3[0] * row[reflect(x - 2 * spacing, width)] +
                  B3[1] * row[reflect(x - spacing, width)] +
                  B3[2] * row[x] +
                  B3[3] * row[reflect(x + spacing, width)] +
                  B3[4] * row[reflect(x + 2 * spacing, width)];
    }
  }
}

/* soft threshold: shrink towards 0 by threshold, 0 inside it */
static float shrink(float detail, float threshold) {
  if(detail > threshold) return detail - threshold;
  if(detail < -threshold) return detail + threshold;
  return 0.f;
}

void ft_shrink_atrous_band(const float* input, uint32_t width, uint32_t height,
                           int32_t first_row, int32_t end_row, float* output,
                           double k, double sigma, const double* weights,
                           void* scratch) {
  if(first_row < 0) first_row = 0;
  if(end_row > (int64_t)height) end_row = (int32_t)height;
  if(first_row >= end_row) return;
  if(width < FT_ATROUS_MIN_SIDE || height < FT_ATROUS_MIN_SIDE) {
    for(int32_t y = first_row; y < end_row; y++) {
      for(uint32_t x = 0; x < width; x++) {
        output[(size_t)y * width + x] = clean(input[(size_t)y * width + x]);
      }
    }
    return;
  }
  if(weights == NULL) weights = FT_ATROUS_DEFAULT_WEIGHTS;
  /* the rows the band depends on; mirrored at their ends, which only
   * matters (and is exact) where they are the image's edges */
  int32_t region_start =
    first_row - FT_ATROUS_HALO_ROWS < 0 ? 0 : first_row - FT_ATROUS_HALO_ROWS;
  int32_t region_end = end_row + FT_ATROUS_HALO_ROWS > (int64_t)height
                         ? (int32_t)height
                         : end_row + FT_ATROUS_HALO_ROWS;
  uint32_t region_rows = (uint32_t)(region_end - region_start);
  /* current is c_j, smoothed receives c_{j+1}; they swap each scale */
  float* current = scratch;
  float* smoothed = current + (size_t)region_rows * width;
  float* row = smoothed + (size_t)region_rows * width;
  for(uint32_t y = 0; y < region_rows; y++) {
    for(uint32_t x = 0; x < width; x++) {
      current[(size_t)y * width + x] =
        clean(input[((size_t)region_start + y) * width + x]);
    }
  }
  for(int32_t y = first_row; y < end_row; y++) {
    memset(output + (size_t)y * width, 0, (size_t)width * sizeof *output);
  }
  for(uint32_t scale = 0; scale < FT_ATROUS_SCALES; scale++) {
    float threshold =
      (float)(k * sigma * FT_ATROUS_NOISE_STD[scale] * weights[scale]);
    ft_smooth_atrous(current, width, region_rows, scale, smoothed, row);
    /* the detail w_j = c_j - c_{j+1}, shrunk, added up */
    for(int32_t y = first_row; y < end_row; y++) {
      const float* coarse = current + (size_t)(y - region_start) * width;
      const float* smooth = smoothed + (size_t)(y - region_start) * width;
      float* target = output + (size_t)y * width;
      for(uint32_t x = 0; x < width; x++) {
        target[x] += shrink(coarse[x] - smooth[x], threshold);
      }
    }
    float* swap = current;
    current = smoothed;
    smoothed = swap;
  }
  /* plus the last smooth plane, c_5 */
  for(int32_t y = first_row; y < end_row; y++) {
    const float* coarse = current + (size_t)(y - region_start) * width;
    float* target = output + (size_t)y * width;
    for(uint32_t x = 0; x < width; x++) target[x] += coarse[x];
  }
}

void ft_shrink_atrous(const float* input, uint32_t width, uint32_t height,
                      float* output, double k, double sigma,
                      const double* weights, void* scratch) {
  /* the band copies the whole input first, so output may equal input */
  ft_shrink_atrous_band(input, width, height, 0, (int32_t)height, output, k,
                        sigma, weights, scratch);
}

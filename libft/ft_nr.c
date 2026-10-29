/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_nr.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "ft_atrous.h"

/* the blocks the noise sigma is measured in */
#define BLOCK 32
/* 1/sqrt(3), 1/sqrt(2), 1/sqrt(6): the opponent split's scales */
#define INVERSE_SQRT3 0.57735026918962576
#define INVERSE_SQRT2 0.70710678118654752
#define INVERSE_SQRT6 0.40824829046386302
/* MAD / 0.6745 estimates sigma for normal noise */
#define MAD_TO_SIGMA_DIVISOR 0.6745
/* the smallest offset the transform takes */
#define MIN_OFFSET 1e-30
/* the density floor for grain: T = 1e-5 */
#define MIN_GRAIN_TRANSMITTANCE 1e-5

size_t ft_get_noise_reduction_scratch_bytes(uint32_t width, uint32_t height) {
  /* 3 opponent planes + the a-trous run (2 planes + a row) */
  return (size_t)3 * width * height * sizeof(float) +
         ft_get_atrous_scratch_bytes(width, height);
}

void ft_stabilise_variance(const float* values, size_t count, double slope,
                           double offset, float* stabilised) {
  if(offset < MIN_OFFSET) offset = MIN_OFFSET;
  for(size_t index = 0; index < count; index++) {
    double value = values[index];
    if(slope > 0) {
      double inside = slope * value + 0.375 * slope * slope + offset;
      stabilised[index] = (float)(2 / slope * sqrt(inside > 0 ? inside : 0));
    } else {
      stabilised[index] = (float)(value / sqrt(offset));
    }
  }
}

void ft_unstabilise_variance(const float* stabilised, size_t count,
                             double slope, double offset, float* values) {
  /* Normalised: y' = y / S, sigma'^2 = O / S^2, z = 2 sqrt(y' + 3/8 +
   * sigma'^2); Makitalo & Foi's closed form of E[z] -> y', clamped at 0.
   * Below z = 2 sqrt(3/8) (y' = 0 without read noise) the series is
   * meaningless and y' = 0. */
  const double c = sqrt(1.5);
  const double min_z = 2 * sqrt(0.375);
  if(offset < MIN_OFFSET) offset = MIN_OFFSET;
  if(!(slope > 0)) {
    for(size_t index = 0; index < count; index++) {
      values[index] = (float)(stabilised[index] * sqrt(offset));
    }
    return;
  }
  double sigma2 = offset / (slope * slope);
  for(size_t index = 0; index < count; index++) {
    double z = stabilised[index];
    double expected = 0;
    if(z > min_z) {
      double r = 1 / z;
      expected = 0.25 * z * z +
                 0.25 * c * r -
                 1.375 * r * r +
                 0.625 * c * r * r * r -
                 0.125 -
                 sigma2;
      if(expected < 0) expected = 0;
    }
    values[index] = (float)(expected * slope);
  }
}

/* The k-th smallest of values[0..count), by quickselect (Hoare
 * partition). Reorders values. */
static float select_kth(float* values, int64_t count, int64_t k) {
  int64_t low = 0;
  int64_t high = count - 1;
  while(low < high) {
    float pivot = values[low + (high - low) / 2];
    int64_t left = low;
    int64_t right = high;
    while(left <= right) {
      while(values[left] < pivot) left++;
      while(values[right] > pivot) right--;
      if(left <= right) {
        float swap = values[left];
        values[left] = values[right];
        values[right] = swap;
        left++;
        right--;
      }
    }
    if(k <= right) {
      high = right;
    } else if(k >= left) {
      low = left;
    } else {
      break;
    }
  }
  return values[k];
}

double ft_measure_noise_sigma(const float* plane, uint32_t width,
                              uint32_t height, void* scratch) {
  size_t pixel_count = (size_t)width * height;
  float* smooth1 = scratch;
  float* smooth2 = smooth1 + pixel_count;
  float* smooth3 = smooth2 + pixel_count;
  float* energies = smooth3 + pixel_count;
  uint32_t blocks_across = width / BLOCK;
  uint32_t blocks_down = height / BLOCK;
  int64_t block_count = (int64_t)blocks_across * blocks_down;
  if(width < FT_ATROUS_MIN_SIDE ||
     height < FT_ATROUS_MIN_SIDE ||
     block_count < 1) {
    return 0;
  }
  /* c1, c2, c3; the rows of energies serve as the smoothing row buffer */
  ft_smooth_atrous(plane, width, height, 0, smooth1, energies);
  ft_smooth_atrous(smooth1, width, height, 1, smooth2, energies);
  ft_smooth_atrous(smooth2, width, height, 2, smooth3, energies);
  /* each block's energy of scale 2 (c2 - c3) into energies[0..count),
   * a copy for the cut into energies[count..2 count) */
  for(int64_t block = 0; block < block_count; block++) {
    uint32_t block_x = (uint32_t)(block % blocks_across);
    uint32_t block_y = (uint32_t)(block / blocks_across);
    double energy = 0;
    for(uint32_t y = block_y * BLOCK; y < (block_y + 1) * BLOCK; y++) {
      for(uint32_t x = block_x * BLOCK; x < (block_x + 1) * BLOCK; x++) {
        size_t at = (size_t)y * width + x;
        double detail = (double)smooth2[at] - smooth3[at];
        energy += detail * detail;
      }
    }
    energies[block] = (float)energy;
    energies[block_count + block] = (float)energy;
  }
  /* the flattest quarter (or all, with fewer than 4 blocks) */
  int64_t kept_blocks = block_count < 4 ? block_count : block_count / 4;
  float cut = select_kth(energies + block_count, block_count, kept_blocks - 1);
  /* |the finest scale| of the kept blocks into smooth3 (no longer
   * needed) */
  int64_t sample_count = 0;
  for(int64_t block = 0;
      block < block_count && sample_count < kept_blocks * BLOCK * BLOCK;
      block++) {
    if(energies[block] > cut) continue;
    uint32_t block_x = (uint32_t)(block % blocks_across);
    uint32_t block_y = (uint32_t)(block / blocks_across);
    for(uint32_t y = block_y * BLOCK; y < (block_y + 1) * BLOCK; y++) {
      for(uint32_t x = block_x * BLOCK; x < (block_x + 1) * BLOCK; x++) {
        size_t at = (size_t)y * width + x;
        smooth3[sample_count] = fabsf(plane[at] - smooth1[at]);
        sample_count++;
      }
    }
  }
  return select_kth(smooth3, sample_count, sample_count / 2) /
         MAD_TO_SIGMA_DIVISOR /
         FT_ATROUS_NOISE_STD[0];
}

/* NaN and inf count as 0 */
static float clean(float value) { return isfinite(value) ? value : 0.f; }

void ft_estimate_noise_profile(const float* pixels, uint32_t width,
                               uint32_t height, double profile[6],
                               void* scratch) {
  size_t pixel_count = (size_t)width * height;
  float* plane = scratch;
  for(uint32_t channel = 0; channel < 3; channel++) {
    for(size_t index = 0; index < pixel_count; index++) {
      plane[index] = clean(pixels[3 * index + channel]);
    }
    double sigma =
      ft_measure_noise_sigma(plane, width, height, plane + pixel_count);
    profile[2 * channel] = 0;
    profile[2 * channel + 1] = sigma * sigma;
  }
}

/* density, floored at MIN_GRAIN_TRANSMITTANCE */
static double get_density(float transmittance) {
  double value = clean(transmittance);
  return -log10(value > MIN_GRAIN_TRANSMITTANCE ? value
                                                : MIN_GRAIN_TRANSMITTANCE);
}

double ft_measure_grain_sigma(const float* pixels, uint32_t width,
                              uint32_t height, void* scratch) {
  size_t pixel_count = (size_t)width * height;
  float* luminance = scratch;
  for(size_t index = 0; index < pixel_count; index++) {
    luminance[index] = (float)((get_density(pixels[3 * index]) +
                                get_density(pixels[3 * index + 1]) +
                                get_density(pixels[3 * index + 2])) *
                               INVERSE_SQRT3);
  }
  return ft_measure_noise_sigma(luminance, width, height,
                                luminance + pixel_count);
}

/* the opponent planes Y, C1, C2 of three channel values, at index */
static void split_opponent(double red, double green, double blue,
                           float* luminance, float* chroma1, float* chroma2,
                           size_t index) {
  luminance[index] = (float)((red + green + blue) * INVERSE_SQRT3);
  chroma1[index] = (float)((red - blue) * INVERSE_SQRT2);
  chroma2[index] = (float)((red - 2 * green + blue) * INVERSE_SQRT6);
}

/* the three channel values back from the opponent planes */
static void join_opponent(const float* luminance, const float* chroma1,
                          const float* chroma2, size_t index,
                          double values[3]) {
  double y = luminance[index] * INVERSE_SQRT3;
  double c1 = chroma1[index] * INVERSE_SQRT2;
  double c2 = chroma2[index] * INVERSE_SQRT6;
  values[0] = y + c1 + c2;
  values[1] = y - 2 * c2;
  values[2] = y - c1 + c2;
}

void ft_reduce_sensor_noise(float* pixels, uint32_t width, uint32_t height,
                            const double profile[6], double k, double k_chroma,
                            void* scratch) {
  size_t pixel_count = (size_t)width * height;
  float* luminance = scratch;
  float* chroma1 = luminance + pixel_count;
  float* chroma2 = chroma1 + pixel_count;
  float* work = chroma2 + pixel_count;
  if(!(k > 0) && !(k_chroma > 0)) return;
  if(width < FT_ATROUS_MIN_SIDE || height < FT_ATROUS_MIN_SIDE) return;
  /* the transform channel by channel (through work), then opponent */
  for(uint32_t channel = 0; channel < 3; channel++) {
    for(size_t index = 0; index < pixel_count; index++) {
      work[index] = clean(pixels[3 * index + channel]);
    }
    ft_stabilise_variance(work, pixel_count, profile[2 * channel],
                          profile[2 * channel + 1], work);
    for(size_t index = 0; index < pixel_count; index++) {
      pixels[3 * index + channel] = work[index];
    }
  }
  for(size_t index = 0; index < pixel_count; index++) {
    split_opponent(pixels[3 * index], pixels[3 * index + 1],
                   pixels[3 * index + 2], luminance, chroma1, chroma2, index);
  }
  /* sigma 1: the transform made the noise unit */
  ft_shrink_atrous(luminance, width, height, luminance, k, 1, NULL, work);
  ft_shrink_atrous(chroma1, width, height, chroma1, k_chroma, 1, NULL, work);
  ft_shrink_atrous(chroma2, width, height, chroma2, k_chroma, 1, NULL, work);
  for(size_t index = 0; index < pixel_count; index++) {
    double values[3];
    join_opponent(luminance, chroma1, chroma2, index, values);
    pixels[3 * index] = (float)values[0];
    pixels[3 * index + 1] = (float)values[1];
    pixels[3 * index + 2] = (float)values[2];
  }
  for(uint32_t channel = 0; channel < 3; channel++) {
    for(size_t index = 0; index < pixel_count; index++) {
      work[index] = pixels[3 * index + channel];
    }
    ft_unstabilise_variance(work, pixel_count, profile[2 * channel],
                            profile[2 * channel + 1], work);
    for(size_t index = 0; index < pixel_count; index++) {
      pixels[3 * index + channel] = work[index];
    }
  }
}

void ft_reduce_grain(float* pixels, uint32_t width, uint32_t height, double k,
                     void* scratch) {
  size_t pixel_count = (size_t)width * height;
  float* luminance = scratch;
  float* chroma1 = luminance + pixel_count;
  float* chroma2 = chroma1 + pixel_count;
  float* work = chroma2 + pixel_count;
  if(!(k > 0) || width < FT_ATROUS_MIN_SIDE || height < FT_ATROUS_MIN_SIDE) {
    return;
  }
  /* uses all 5 planes as scratch */
  double sigma = ft_measure_grain_sigma(pixels, width, height, luminance);
  for(size_t index = 0; index < pixel_count; index++) {
    split_opponent(
      get_density(pixels[3 * index]), get_density(pixels[3 * index + 1]),
      get_density(pixels[3 * index + 2]), luminance, chroma1, chroma2, index);
  }
  ft_shrink_atrous(luminance, width, height, luminance, k, sigma, NULL, work);
  ft_shrink_atrous(chroma1, width, height, chroma1, 0.5 * k, sigma, NULL, work);
  ft_shrink_atrous(chroma2, width, height, chroma2, 0.5 * k, sigma, NULL, work);
  for(size_t index = 0; index < pixel_count; index++) {
    double values[3];
    join_opponent(luminance, chroma1, chroma2, index, values);
    for(uint32_t channel = 0; channel < 3; channel++) {
      pixels[3 * index + channel] = (float)pow(10, -values[channel]);
    }
  }
}

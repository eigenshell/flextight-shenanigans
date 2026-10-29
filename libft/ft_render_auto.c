/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_render_auto.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* the histogram: densities -0.2 .. 4.6 in 0.001 D bins */
#define HISTOGRAM_FIRST_DENSITY -0.2
#define HISTOGRAM_STEP 0.001
#define HISTOGRAM_BINS 4800
/* no estimate beyond the T floor (4.5 D) */
#define MAX_DENSITY 4.5
/* the fewest range a channel gets */
#define MIN_RANGE 0.05
/* pixels read per call: ~250 k blocks on a 44 Mpx frame, still ~1 k
 * highlight blocks for the median */
#define MAX_SAMPLES 4e6
/* statistics on 4x4 block means: the noise / 4 */
#define BLOCK 4
/* the channel codes beyond 0..2: luminance density, and the mean of the
 * three densities */
#define CHANNEL_LUMINANCE 3
#define CHANNEL_MEAN 4
/* no selection: every block */
#define NO_SELECTION (-1)
/* a highlight's neighbour is in the same area within this density */
#define SAME_AREA_DENSITY 0.15
/* Blocks that are not film: holder metal, an unexposed slide border in a
 * negative crop (all three channels at or beyond 4.0 D) must not become
 * the highlight; the open beam through a sprocket hole or a clear gap
 * (T above 0.9 in all three channels) must not become the film base. For
 * the slide black, the densest film is wanted, so only blocks at the T
 * floor (4.4 D and beyond in all channels) are metal. */
#define OPAQUE_TRANSMITTANCE 1e-4
#define SLIDE_OPAQUE_TRANSMITTANCE 3.98107e-5
/* Holder metal reads T ~ 0.001 +- 0.0005 per pixel, far above the block-
 * mean limit. A metal block (or a block with a metal column at an edge)
 * almost always has >= 4 of its 16 pixels at T <= 0.002 in all three
 * channels; film up to ~2.6 D almost never does (noise would have to dip
 * 2 sigma in three channels at once, four times). Beyond ~2.8 D film and
 * metal look the same to the scanner: no estimate can tell them apart. */
#define METAL_TRANSMITTANCE 0.002
#define METAL_MIN_PIXELS 4
/* the pixel metal test switched off */
#define NO_METAL_TEST (-1.0)
#define CLEAR_TRANSMITTANCE 0.9
/* the clear test switched off */
#define NO_CLEAR_TEST 1e30
/* the highlight sits this far above paper white */
#define HIGHLIGHT_ABOVE_WHITE 0.03
/* the largest exposure, stops */
#define MAX_EXPOSURE 6

/* The limits that tell film from what is not film. */
typedef struct not_film_limits {
  /* a block mean this dense in every channel is not film */
  double opaque_transmittance;
  /* pixels at or below this in every channel count as metal; negative:
   * no test */
  double metal_transmittance;
  /* a pixel above this in every channel is open beam */
  double clear_transmittance;
} not_film_limits;

/* The block means of transmittance for the BLOCK x BLOCK block at x, y:
 * dense highlights are a few DN above dark and one pixel's density is
 * mostly noise there. Floored (also NaN) so logs are finite.
 * Returns true when the block is not film: it has a non-finite value,
 * >= METAL_MIN_PIXELS pixels at T <= metal in all three channels (holder
 * metal, also the edge of one), or any pixel above clear in all three (a
 * hole, the open beam): a block that straddles such an edge is a mix,
 * not the highlight or the base. */
static bool measure_block(const ft_image* image, uint32_t x, uint32_t y,
                          const not_film_limits* limits, double means[3]) {
  bool is_bad = false;
  uint32_t dark_count = 0;
  means[0] = 0;
  means[1] = 0;
  means[2] = 0;
  for(uint32_t row = 0; row < BLOCK; row++) {
    for(uint32_t column = 0; column < BLOCK; column++) {
      const float* pixel =
        image->pixels + 3 * ((size_t)(y + row) * image->width + x + column);
      for(uint32_t channel = 0; channel < 3; channel++) {
        means[channel] += pixel[channel];
      }
      double clear = limits->clear_transmittance;
      double metal = limits->metal_transmittance;
      if(isfinite(pixel[0]) == false ||
         isfinite(pixel[1]) == false ||
         isfinite(pixel[2]) == false ||
         (pixel[0] > clear && pixel[1] > clear && pixel[2] > clear)) {
        is_bad = true;
      }
      if(pixel[0] <= metal && pixel[1] <= metal && pixel[2] <= metal) {
        dark_count++;
      }
    }
  }
  for(uint32_t channel = 0; channel < 3; channel++) {
    means[channel] /= BLOCK * BLOCK;
    if(!(means[channel] >= FT_TRANSMITTANCE_FLOOR)) {
      means[channel] = FT_TRANSMITTANCE_FLOOR;
    }
  }
  return is_bad || dark_count >= METAL_MIN_PIXELS;
}

/* is the block's mean not film either (the same test on the means)? */
static bool is_mean_not_film(const double means[3],
                             const not_film_limits* limits) {
  double opaque = limits->opaque_transmittance;
  double clear = limits->clear_transmittance;
  bool is_opaque =
    means[0] <= opaque && means[1] <= opaque && means[2] <= opaque;
  bool is_clear = means[0] > clear && means[1] > clear && means[2] > clear;
  return is_opaque || is_clear;
}

/* The density of a channel (0..2, CHANNEL_LUMINANCE by weights, or
 * CHANNEL_MEAN) from block means. */
static double get_density(const double means[3], uint32_t channel,
                          const float weights[3]) {
  if(channel < 3) return -log10(means[channel]);
  if(channel == CHANNEL_LUMINANCE) {
    double luminance =
      weights[0] * means[0] + weights[1] * means[1] + weights[2] * means[2];
    if(!(luminance > FT_TRANSMITTANCE_FLOOR)) {
      luminance = FT_TRANSMITTANCE_FLOOR;
    }
    return -log10(luminance);
  }
  /* the mean density: the product is at least 3e-14, so the log is
   * finite */
  return -log10(means[0] * means[1] * means[2]) / 3;
}

/* What histogram counts. */
typedef struct histogram_request {
  /* the channel whose density is counted */
  uint32_t channel;
  /* the luminance weights */
  const float* weights;
  /* NO_SELECTION, or the channel a block must reach minimum in */
  int32_t selection;
  double minimum;
  not_film_limits limits;
} histogram_request;

/* The histogram of block densities over every frame, 2 % in from each
 * edge, strided so that at most MAX_SAMPLES pixels are read in all.
 * Blocks that are not film are left out. With a selection: only where
 * the block's density of the selection channel is >= minimum, measuring
 * the block 2 x BLOCK to its right: the same scene (highlights are
 * areas), but independent noise and grain (dye clouds of 10-30 um span a
 * few pixels at 6300 ppi, so the adjacent block is correlated); selecting
 * a block for its own value would pick its noise. Returns the count. */
static uint64_t count_histogram(const ft_image* const* images,
                                uint32_t image_count,
                                const histogram_request* request,
                                uint32_t* histogram) {
  double pixel_count = 0;
  for(uint32_t index = 0; index < image_count; index++) {
    if(images[index]->pixels != NULL) {
      pixel_count += (double)images[index]->width * images[index]->height;
    }
  }
  uint32_t step = (uint32_t)ceil(BLOCK * sqrt(pixel_count / MAX_SAMPLES));
  if(step < BLOCK) step = BLOCK;
  memset(histogram, 0, HISTOGRAM_BINS * sizeof *histogram);
  bool is_selecting = request->selection != NO_SELECTION;
  /* a selected block reads its neighbour two blocks to the right too */
  uint32_t blocks_wide = is_selecting ? 3 : 1;
  uint64_t total = 0;
  for(uint32_t index = 0; index < image_count; index++) {
    const ft_image* image = images[index];
    if(image->pixels == NULL) continue;
    uint32_t first_x = image->width / 50;
    uint32_t end_x = image->width - image->width / 50;
    uint32_t first_y = image->height / 50;
    uint32_t end_y = image->height - image->height / 50;
    for(uint32_t y = first_y; y + BLOCK <= end_y; y += step) {
      for(uint32_t x = first_x; x + blocks_wide * BLOCK <= end_x; x += step) {
        double means[3];
        if(measure_block(image, x, y, &request->limits, means) ||
           is_mean_not_film(means, &request->limits)) {
          continue;
        }
        if(is_selecting) {
          uint32_t selection = (uint32_t)request->selection;
          if(get_density(means, selection, request->weights) <
             request->minimum) {
            continue;
          }
          /* a neighbour in other scene content: skip it. The margin is
           * ~5x the block noise, so this hardly selects on noise. */
          if(measure_block(image, x + 2 * BLOCK, y, &request->limits, means) ||
             is_mean_not_film(means, &request->limits) ||
             get_density(means, selection, request->weights) <
               request->minimum - SAME_AREA_DENSITY) {
            continue;
          }
        }
        double bin = (get_density(means, request->channel, request->weights) -
                      HISTOGRAM_FIRST_DENSITY) /
                     HISTOGRAM_STEP;
        /* also -inf and NaN */
        if(!(bin >= 0)) bin = 0;
        if(bin > HISTOGRAM_BINS - 1) bin = HISTOGRAM_BINS - 1;
        histogram[(uint32_t)bin]++;
        total++;
      }
    }
  }
  return total;
}

/* the density at quantile 0..1 of the histogram */
static double get_quantile(const uint32_t* histogram, uint64_t total,
                           double quantile) {
  /* defensive: an empty histogram has no quantile (callers check) */
  if(total == 0) return HISTOGRAM_FIRST_DENSITY;
  double wanted = quantile * (double)(total - 1);
  double cumulative = 0;
  uint32_t bin = 0;
  for(; bin < HISTOGRAM_BINS; bin++) {
    cumulative += histogram[bin];
    if(cumulative > wanted) break;
  }
  if(bin == HISTOGRAM_BINS) bin = HISTOGRAM_BINS - 1;
  return HISTOGRAM_FIRST_DENSITY + (bin + 0.5) * HISTOGRAM_STEP;
}

int32_t ft_estimate_base_from_frames(const ft_image* const* images,
                                     uint32_t image_count, ft_render_mode mode,
                                     float density[3], ft_err* error) {
  uint32_t histogram[HISTOGRAM_BINS];
  bool is_slide = mode == FT_RENDER_SLIDE;
  for(uint32_t channel = 0; channel < 3; channel++) {
    histogram_request request = {
      .channel = channel,
      .weights = NULL,
      .selection = NO_SELECTION,
      .minimum = 0,
      .limits = {
        .opaque_transmittance = is_slide ? SLIDE_OPAQUE_TRANSMITTANCE : 0,
        .metal_transmittance = NO_METAL_TEST,
        .clear_transmittance = is_slide ? NO_CLEAR_TEST : CLEAR_TRANSMITTANCE}};
    uint64_t total = count_histogram(images, image_count, &request, histogram);
    if(total == 0) {
      return ft_fail(error, "auto: no pixels for a film base estimate");
    }
    double quantile = get_quantile(histogram, total, is_slide ? 0.995 : 0.005);
    if(quantile > MAX_DENSITY) quantile = MAX_DENSITY;
    if(!(quantile > 0)) quantile = 0;
    density[channel] = (float)quantile;
  }
  return 0;
}

int32_t ft_estimate_recipe(const ft_image* const* images, uint32_t image_count,
                           ft_recipe* recipe, ft_err* error) {
  uint32_t histogram[HISTOGRAM_BINS];
  bool is_bw = recipe->mode == FT_RENDER_BW;
  for(uint32_t channel = 0; channel < 3; channel++) recipe->tint[channel] = 0;
  if(recipe->mode == FT_RENDER_SLIDE) {
    recipe->exposure = 0;
    return 0;
  }
  /* The brightest 0.5 % of the scene (densest film), picked once on the
   * mean density so every channel sees the same blocks (this favours
   * blocks dense in the high-gamma, blue layer); each channel's range is
   * its median on the neighbours of those blocks. A percentile per
   * channel would pick each channel's own noise tail, the blue most: a
   * colour cast. */
  uint32_t selection = is_bw ? CHANNEL_LUMINANCE : CHANNEL_MEAN;
  not_film_limits limits = {.opaque_transmittance = OPAQUE_TRANSMITTANCE,
                            .metal_transmittance = METAL_TRANSMITTANCE,
                            .clear_transmittance = NO_CLEAR_TEST};
  histogram_request request = {.channel = selection,
                               .weights = recipe->bw_weights,
                               .selection = NO_SELECTION,
                               .minimum = 0,
                               .limits = limits};
  uint64_t total = count_histogram(images, image_count, &request, histogram);
  if(total == 0) return ft_fail(error, "auto: no pixels to estimate from");
  double minimum = get_quantile(histogram, total, 0.995) - HISTOGRAM_STEP / 2;

  uint32_t channel_count = is_bw ? 1 : 3;
  for(uint32_t channel = 0; channel < channel_count; channel++) {
    request.channel = is_bw ? CHANNEL_LUMINANCE : channel;
    request.selection = (int32_t)selection;
    request.minimum = minimum;
    total = count_histogram(images, image_count, &request, histogram);
    double range;
    if(total == 0) {
      /* a thin highlight with no same-area neighbour to measure: use the
       * channel's own 99.5th percentile instead, slightly noise-biased
       * but better than nothing */
      request.selection = NO_SELECTION;
      request.minimum = 0;
      total = count_histogram(images, image_count, &request, histogram);
      range =
        get_quantile(histogram, total, 0.995) - recipe->base_density[channel];
    } else {
      range =
        get_quantile(histogram, total, 0.5) - recipe->base_density[channel];
    }
    /* also NaN */
    if(!(range > MIN_RANGE)) range = MIN_RANGE;
    if(range > MAX_DENSITY) range = MAX_DENSITY;
    recipe->range_density[channel] = (float)range;
  }
  if(is_bw) {
    recipe->range_density[1] = recipe->range_density[0];
    recipe->range_density[2] = recipe->range_density[0];
  }
  recipe->exposure = ft_estimate_auto_exposure(recipe);
  return 0;
}

float ft_estimate_auto_exposure(const ft_recipe* recipe) {
  /* The highlight (normalised 1) at paper white + 0.03 D:
   * s = 0.03 / (black - white), z = logit(s) = 4 grade u,
   * u = -0.5 - lift / range[1], lift = exposure log10(2). */
  double share =
    HIGHLIGHT_ABOVE_WHITE / (recipe->paper_black - recipe->paper_white);
  if(!(share > 1e-6)) share = 1e-6;
  if(share > 1 - 1e-6) share = 1 - 1e-6;
  double exposure = -(log(share / (1 - share)) / (4 * recipe->grade) + 0.5) *
                    recipe->range_density[1] /
                    log10(2);
  if(!(recipe->grade > 0) || isfinite(exposure) == false) exposure = 0;
  if(exposure > MAX_EXPOSURE) exposure = MAX_EXPOSURE;
  if(exposure < -MAX_EXPOSURE) exposure = -MAX_EXPOSURE;
  return (float)exposure;
}

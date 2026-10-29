/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Frame detection on a flat-fielded preview. Outline (see the design,
 * "Frame detection"):
 *  1. lines: open holder / holder metal / film; film = first..last film line
 *  2. columns over film lines: image band between the hole rows
 *  3. base level per ~10 mm along, from the rebate between the holes
 *     (pixels well away from open beam), robust median + MAD noise
 *  4. per pixel: exposed / base-like / skip (open beam and its halo)
 *  5. strip tilt from the two hole rows; everything below works on lines
 *     deskewed by that tilt
 *  6. gap lines (>= 97 % of image columns base-like) and straight edges
 *  7. exposed runs between trusted gaps -> frames; long untrusted
 *     stretches are gridded and flagged; long gaps count blank frames
 *
 * Line and column numbers are int32_t here: the deskew moves them by
 * signed offsets, and the edge search steps one line past the film. */
#include "ft_detect.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846
/* all channels above: open beam */
#define OPEN_TRANSMITTANCE 0.85f
/* All channels below: holder metal (or Dmax). Metal reads ~0.0006-0.0014
 * per channel with noise on the hardware (2026-10-12); at 0.005, 95 % of
 * its pixels count, against at most 14 % on any colour-negative film line.
 * Slide Dmax lines can be as dark: hole-row light keeps them film. */
#define METAL_TRANSMITTANCE 0.005f
/* a line is open / metal when this share of it is */
#define LINE_SHARE 0.80
/* the open share of a line that marks hole rows */
#define HOLE_LINE_SHARE 0.02
/* Narrowest hole row in view. The 24x217 window hides most of each 2.8 mm
 * hole row: 0.7-1.6 mm of them show, the narrow side moving with where
 * the strip lies across (hardware 2026-10-12/13). Blur and the open halo
 * are ~0.3 mm; no other column run is 25-90 % open over the film. */
#define HOLE_MIN_MM 0.5
/* log10 density within this of the base counts as base */
#define BASE_DENSITY 0.08
/* Additive noise floor on the base-like test, in units of the base noise
 * (MAD of the rebate). Needed for slide Dmax (~0.0005) where sensor noise
 * is larger than 0.08 density; 4 sigma keeps false exposed pixels < 1e-4. */
#define NOISE_SIGMAS 4.0
/* Open-beam halo excluded from the base sample and the gap test: blur
 * spreads hole and speck light ~0.1-0.15 mm; 0.3 mm leaves margin. */
#define HALO_MM 0.3
/* Local base segment: absorbs base drift along the roll (+-10 % over the
 * strip changes the base < 3 % within 10 mm, well inside BASE_DENSITY). */
#define SEGMENT_MM 10.0
/* samples for a segment to count */
#define SEGMENT_MIN_SAMPLES 200
/* image columns base-like on a gap line */
#define GAP_SHARE 0.97
/* a gap score at least this counts as near a gap */
#define NEAR_GAP_SHARE 0.85
/* how far past a gap the edge search looks */
#define EDGE_WINDOW_MM 3.0
/* inlier columns needed for an edge */
#define EDGE_FOUND_SHARE 0.80
/* the largest inlier rms of an edge line */
#define EDGE_RMS_MM 0.25
/* inliers of an outermost-cluster edge */
#define EDGE_OUTER_SHARE 0.40
/* columns allowed past an outermost-cluster edge, into the gap */
#define EDGE_BEYOND_SHARE 0.02
/* Smallest inlier band of the robust edge fit: blur + grain jitter a
 * straight edge by 1-2 lines per column; 0.15 mm (3 lines) keeps those. */
#define EDGE_CLIP_MM 0.15
/* the shortest gap that counts */
#define MIN_GAP_MM 0.2
/* the shortest exposed run that counts as a frame */
#define MIN_RUN_MM 6.0
/* a run at a strip end shorter than this is dropped */
#define END_SLIVER_MM 3.0
/* Strip ends: lines within END_CUT_MM whose image band is at least
 * CUT_OPEN_SHARE open beam are past the film (a cut on the diagonal,
 * hardware 2026-10-12), never frame. Dust specks cover ~1 % of the band. */
#define END_CUT_MM 10.0
#define CUT_OPEN_SHARE 0.05
/* A line whose image band is at least this much open beam has no film on
 * it (open holder between loose pieces, hardware 2026-10-12): it counts as
 * a gap line, never as a frame or a blank frame. */
#define NO_FILM_OPEN_SHARE 0.50
/* a trusted gap longer than this holds blank frames */
#define BLANK_MM 12.0
/* a strip longer than this without any trusted edge uses the holder grid */
#define FALLBACK_MM 45.0
/* A run longer than this without a trusted edge inside is gridded and
 * flagged. Design suggests ~70 mm, but swing-lens panoramas reach 66 mm
 * and the test strip has a 100 mm panorama; 110 mm separates that from
 * three touching frames (114 mm). */
#define GRID_RUN_MM 110.0
/* Real panoramas on 35 mm: Horizon/Widelux 58-59 mm, XPan 65, Noblex 66.
 * A longer run between trusted gaps is more likely frames whose gap was
 * missed (touching frames, a light leak across the gap): flagged low. */
#define PANORAMA_MIN_MM 55.0
#define PANORAMA_MAX_MM 70.0
/* the largest strip tilt the hole-row search looks for */
#define MAX_TILT_DEGREES 2.0
/* an edge-based tilt below this is ignored */
#define MIN_TILT_DEGREES 0.05
/* 35 mm frame lengths: half frames 15.5..21, full frames 33..39.5 mm;
 * nominal 18 and 36 mm, within 1.5 mm at a strip end */
#define HALF_MIN_MM 15.5
#define HALF_MAX_MM 21.0
#define FULL_MIN_MM 33.0
#define FULL_MAX_MM 39.5
#define HALF_LENGTH_MM 18.0
#define FULL_LENGTH_MM 36.0
#define END_LENGTH_TOLERANCE_MM 1.5
/* a run between types counts as half below this, else full */
#define HALF_OR_FULL_MM 27.0
/* the frame pitch for counting blank frames */
#define FULL_PITCH_MM 38.0
#define HALF_PITCH_MM 19.0
/* a half frame this close to a gap sets the half pitch */
#define NEAR_GAP_MM 3.0
/* the largest film base density the job reader accepts */
#define MAX_FILM_BASE_DENSITY 4.5
/* 1.4826 x MAD estimates sigma for normal noise */
#define MAD_TO_SIGMA 1.4826
/* a base level never goes below this */
#define MIN_BASE_TRANSMITTANCE 1e-5

/* What a pixel shows. */
typedef enum pixel_state {
  /* exposed film: image content */
  STATE_EXPOSED = 0,
  /* film like the unexposed base */
  STATE_BASE = 1,
  /* open beam or its halo: not used */
  STATE_SKIP = 2
} pixel_state;

/* A run of gap lines between frames, with the frame edges around it. */
typedef struct gap {
  /* the gap's lines [start, end), deskewed */
  int32_t start;
  int32_t end;
  /* a straight edge was found before / after it */
  bool has_lead_edge;
  bool has_trail_edge;
  /* at least one edge was found: the gap is trusted */
  bool is_trusted;
  /* open holder in it: not blank film */
  bool has_no_film;
  /* the edge lines at the image centre */
  double lead_position;
  double trail_position;
  /* their residual slopes, mm along per mm across */
  double lead_slope;
  double trail_slope;
} gap;

/* Everything the detection works with. */
typedef struct detector {
  const float* transmittance;
  int32_t lines;
  int32_t pixels;
  /* mm per line along, mm per pixel across */
  double mm_per_line;
  double mm_per_pixel;
  ft_film film;
  /* the film lines [film_start, film_end) */
  int32_t film_start;
  int32_t film_end;
  /* the image band's columns [band_start, band_end), between the hole
   * rows */
  int32_t band_start;
  int32_t band_end;
  /* the outer hole-row columns: holes_start..band_start and
   * band_end..holes_end are the hole rows */
  int32_t holes_start;
  int32_t holes_end;
  /* the image columns [image_start, image_end) */
  int32_t image_start;
  int32_t image_end;
  /* a pixel_state per pixel (film lines only) */
  uint8_t* states;
  /* the deskew offset per column, lines */
  int32_t* deskew_offsets;
  /* tan(the strip's tilt) */
  double tilt_tangent;
  /* the gap score per deskewed line: the base-like share, 1 without
   * film, -1 when too little of the line can be judged */
  double* gap_scores;
  /* per line: the band is mostly open beam */
  uint8_t* is_no_film;
  gap* gaps;
  int32_t gap_count;
} detector;

static const float* get_pixel(const float* transmittance, int32_t pixels,
                              int32_t line, int32_t column) {
  return transmittance + ((int64_t)line * pixels + column) * 3;
}

static bool is_open(const float* pixel) {
  return pixel[0] > OPEN_TRANSMITTANCE &&
         pixel[1] > OPEN_TRANSMITTANCE &&
         pixel[2] > OPEN_TRANSMITTANCE;
}

static bool is_dark(const float* pixel) {
  return pixel[0] < METAL_TRANSMITTANCE &&
         pixel[1] < METAL_TRANSMITTANCE &&
         pixel[2] < METAL_TRANSMITTANCE;
}

/* the open-beam share of line across the image band */
static double get_band_open_share(const detector* d, int32_t line) {
  int32_t open_count = 0;
  for(int32_t column = d->band_start; column < d->band_end; column++) {
    open_count += is_open(get_pixel(d->transmittance, d->pixels, line, column));
  }
  if(d->band_end <= d->band_start) return 0;
  return (double)open_count / (d->band_end - d->band_start);
}

/* the state at deskewed line, column; STATE_SKIP off the film */
static pixel_state get_state(const detector* d, int32_t line, int32_t column) {
  int32_t raw_line = line + d->deskew_offsets[column];
  if(raw_line < d->film_start || raw_line >= d->film_end) return STATE_SKIP;
  return (pixel_state)d->states[(int64_t)raw_line * d->pixels + column];
}

/* The k-th smallest of values[0..count), by quickselect (Hoare
 * partition). Reorders values. */
static double select_kth_float(float* values, int64_t count, int64_t k) {
  int64_t low = 0;
  int64_t high = count - 1;
  while(low < high) {
    float pivot = values[(low + high) / 2];
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
    /* keep searching only the side that holds position k */
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

/* the same as select_kth_float, for doubles */
static double select_kth_double(double* values, int32_t count, int32_t k) {
  int32_t low = 0;
  int32_t high = count - 1;
  while(low < high) {
    double pivot = values[(low + high) / 2];
    int32_t left = low;
    int32_t right = high;
    while(left <= right) {
      while(values[left] < pivot) left++;
      while(values[right] > pivot) right--;
      if(left <= right) {
        double swap = values[left];
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

/* The median and the noise (1.4826 x MAD) of values[0..count).
 * Reorders values. */
static void get_median_and_noise(float* values, int64_t count, double* median,
                                 double* noise) {
  *median = select_kth_float(values, count, count / 2);
  for(int64_t index = 0; index < count; index++) {
    values[index] = (float)fabs(values[index] - *median);
  }
  *noise = MAD_TO_SIGMA * select_kth_float(values, count, count / 2);
}

/* The widest run of indexes in [0, count) with low <= values[i] < high,
 * as [*run_start, *run_end). Returns its length. */
static int32_t find_widest_run(const double* values, int32_t count, double low,
                               double high, int32_t* run_start,
                               int32_t* run_end) {
  int32_t start = -1;
  int32_t widest = 0;
  *run_start = 0;
  *run_end = 0;
  /* one step past the end, so a run reaching it is closed too */
  for(int32_t index = 0; index <= count; index++) {
    bool is_inside =
      index < count && values[index] >= low && values[index] < high;
    if(is_inside && start < 0) start = index;
    if(is_inside == false && start >= 0) {
      if(index - start > widest) {
        widest = index - start;
        *run_start = start;
        *run_end = index;
      }
      start = -1;
    }
  }
  return widest;
}

/* Walk from index from in direction (-1 or +1) while low <= values < high.
 * Returns the first index outside. */
static int32_t walk_while_in_range(const double* values, int32_t count,
                                   int32_t from, int32_t direction, double low,
                                   double high) {
  int32_t index = from;
  while(index >= 0 &&
        index < count &&
        values[index] >= low &&
        values[index] < high) {
    index += direction;
  }
  return index;
}

/* Mark the pixels within HALO_MM of an open pixel (film lines only), as a
 * separable box dilation with running counts: first along each line,
 * then along each column. Returns the marks (the caller frees them), or
 * NULL when memory runs out. */
static uint8_t* mark_open_halo(const detector* d) {
  int32_t pixels = d->pixels;
  int32_t reach_x = (int32_t)ceil(HALO_MM / d->mm_per_pixel);
  int32_t reach_y = (int32_t)ceil(HALO_MM / d->mm_per_line);
  size_t count = (size_t)d->lines * pixels;
  uint8_t* across = calloc(count, 1);
  uint8_t* halo = calloc(count, 1);
  if(across == NULL || halo == NULL) {
    free(across);
    free(halo);
    return NULL;
  }
  /* across: open pixels within reach_x on the same line; the count
   * slides along, adding the pixel entering the window and removing the
   * one leaving it */
  for(int32_t line = d->film_start; line < d->film_end; line++) {
    int32_t open_count = 0;
    for(int32_t column = 0; column < reach_x && column < pixels; column++) {
      open_count += is_open(get_pixel(d->transmittance, pixels, line, column));
    }
    for(int32_t column = 0; column < pixels; column++) {
      if(column + reach_x < pixels) {
        open_count +=
          is_open(get_pixel(d->transmittance, pixels, line, column + reach_x));
      }
      if(column - reach_x - 1 >= 0) {
        open_count -= is_open(
          get_pixel(d->transmittance, pixels, line, column - reach_x - 1));
      }
      across[(int64_t)line * pixels + column] = open_count > 0;
    }
  }
  /* then the same along each column, over the across marks */
  for(int32_t column = 0; column < pixels; column++) {
    int32_t marked_count = 0;
    for(int32_t line = d->film_start;
        line < d->film_start + reach_y && line < d->film_end; line++) {
      marked_count += across[(int64_t)line * pixels + column];
    }
    for(int32_t line = d->film_start; line < d->film_end; line++) {
      if(line + reach_y < d->film_end) {
        marked_count += across[(int64_t)(line + reach_y) * pixels + column];
      }
      if(line - reach_y - 1 >= d->film_start) {
        marked_count -= across[(int64_t)(line - reach_y - 1) * pixels + column];
      }
      halo[(int64_t)line * pixels + column] = marked_count > 0;
    }
  }
  free(across);
  return halo;
}

/* free the buffers of measure_base_levels */
static void free_base_buffers(double* segment_bases, double* segment_noises,
                              double* scratch, bool* has_samples,
                              float* samples[3]) {
  free(segment_bases);
  free(segment_noises);
  free(scratch);
  free(has_samples);
  for(uint32_t channel = 0; channel < 3; channel++) free(samples[channel]);
}

/* Base level per line. With hole rows: the rebate between the holes in the
 * hole-row columns is unexposed film everywhere along the strip (frames do
 * not reach it), so its median per SEGMENT_MM segment is the local base
 * and its MAD the base noise. Without hole rows (cropped preview): one
 * global value, the median of the extreme cluster (brightest on negative,
 * darkest on slide) of all band pixels.
 *
 * line_bases  receives 3 values per film line
 * base        receives the median base over the segments
 * noise       receives the median noise over the segments
 *
 * Returns 0, or -1 when there are too few samples or no memory. */
static int32_t measure_base_levels(const detector* d, const uint8_t* halo,
                                   bool has_holes, float* line_bases,
                                   float base[3], double noise[3]) {
  int32_t pixels = d->pixels;
  int32_t film_lines = d->film_end - d->film_start;
  int32_t segment_lines = (int32_t)(SEGMENT_MM / d->mm_per_line + 0.5);
  int32_t sample_columns =
    has_holes ? (d->band_start - d->holes_start) + (d->holes_end - d->band_end)
              : d->band_end - d->band_start;
  if(segment_lines < 1) segment_lines = 1;
  int32_t segment_count = (film_lines + segment_lines - 1) / segment_lines;
  if(has_holes == false) {
    segment_lines = 0;
    segment_count = 0;
  }
  /* room for one segment's samples (with holes) or the whole band's */
  int64_t capacity = (int64_t)(has_holes ? segment_lines : film_lines) *
                     (sample_columns > 0 ? sample_columns : 1);
  /* per segment: the base and the noise of each channel */
  double* segment_bases =
    calloc((size_t)(segment_count + 1) * 3, sizeof *segment_bases);
  double* segment_noises =
    calloc((size_t)(segment_count + 1) * 3, sizeof *segment_noises);
  double* scratch = calloc((size_t)(segment_count + 1), sizeof *scratch);
  bool* has_samples = calloc((size_t)(segment_count + 1), sizeof *has_samples);
  float* samples[3];
  for(uint32_t channel = 0; channel < 3; channel++) {
    samples[channel] = malloc((size_t)capacity * sizeof(float));
  }
  if(segment_bases == NULL ||
     segment_noises == NULL ||
     scratch == NULL ||
     has_samples == NULL ||
     samples[0] == NULL ||
     samples[1] == NULL ||
     samples[2] == NULL) {
    free_base_buffers(segment_bases, segment_noises, scratch, has_samples,
                      samples);
    return -1;
  }

  int32_t good_segments = 0;
  if(has_holes) {
    for(int32_t segment = 0; segment < segment_count; segment++) {
      int64_t sample_count = 0;
      int32_t first_line = d->film_start + segment * segment_lines;
      int32_t end_line = first_line + segment_lines > d->film_end
                           ? d->film_end
                           : first_line + segment_lines;
      for(int32_t line = first_line; line < end_line; line++) {
        for(int32_t column = d->holes_start; column < d->holes_end; column++) {
          /* jump over the image band: only the hole rows count */
          if(column == d->band_start) column = d->band_end;
          const float* pixel =
            get_pixel(d->transmittance, pixels, line, column);
          if(halo[(int64_t)line * pixels + column] != 0 || is_open(pixel)) {
            continue;
          }
          for(uint32_t channel = 0; channel < 3; channel++) {
            samples[channel][sample_count] = pixel[channel];
          }
          sample_count++;
        }
      }
      if(sample_count < SEGMENT_MIN_SAMPLES) continue;
      for(uint32_t channel = 0; channel < 3; channel++) {
        get_median_and_noise(samples[channel], sample_count,
                             &segment_bases[segment * 3 + channel],
                             &segment_noises[segment * 3 + channel]);
      }
      has_samples[segment] = true;
      good_segments++;
    }
  }

  if(good_segments == 0) {
    /* one global value: the extreme cluster over the band */
    int64_t sample_count = 0;
    /* a line stride so the whole strip fits the sample buffer */
    int32_t line_stride =
      (int32_t)(((int64_t)film_lines * (d->band_end - d->band_start) +
                 capacity -
                 1) /
                capacity);
    has_holes = false;
    for(int32_t line = d->film_start; line < d->film_end;
        line += line_stride > 0 ? line_stride : 1) {
      for(int32_t column = d->band_start; column < d->band_end; column++) {
        const float* pixel = get_pixel(d->transmittance, pixels, line, column);
        if(halo[(int64_t)line * pixels + column] != 0 ||
           is_open(pixel) ||
           sample_count >= capacity) {
          continue;
        }
        for(uint32_t channel = 0; channel < 3; channel++) {
          samples[channel][sample_count] = pixel[channel];
        }
        sample_count++;
      }
    }
    if(sample_count < SEGMENT_MIN_SAMPLES) {
      free_base_buffers(segment_bases, segment_noises, scratch, has_samples,
                        samples);
      return -1;
    }
    bool is_negative = d->film == FT_FILM_NEGATIVE;
    for(uint32_t channel = 0; channel < 3; channel++) {
      /* the 99th percentile on a negative (the base is the brightest),
       * the 1st on a slide (the darkest), and the samples within 0.1
       * density of it */
      int64_t position = is_negative ? (int64_t)(sample_count * 0.99)
                                     : (int64_t)(sample_count * 0.01);
      double extreme =
        select_kth_float(samples[channel], sample_count, position);
      double low = is_negative ? extreme * pow(10, -0.1) : 0;
      double high = is_negative ? 2 : extreme * pow(10, 0.1);
      int64_t kept = 0;
      for(int64_t index = 0; index < sample_count; index++) {
        float value = samples[channel][index];
        if(value >= low && value <= high) {
          samples[channel][kept] = value;
          kept++;
        }
      }
      get_median_and_noise(samples[channel], kept, &segment_bases[channel],
                           &segment_noises[channel]);
    }
    segment_count = 1;
    has_samples[0] = true;
  }

  /* a segment without samples takes the nearest good one's values */
  for(int32_t segment = 0; segment < segment_count; segment++) {
    if(has_samples[segment]) continue;
    int32_t nearest = -1;
    for(int32_t other = 0; other < segment_count; other++) {
      if(has_samples[other] &&
         (nearest < 0 || abs(other - segment) < abs(nearest - segment))) {
        nearest = other;
      }
    }
    for(uint32_t channel = 0; channel < 3; channel++) {
      segment_bases[segment * 3 + channel] =
        segment_bases[nearest * 3 + channel];
      segment_noises[segment * 3 + channel] =
        segment_noises[nearest * 3 + channel];
    }
  }
  for(uint32_t channel = 0; channel < 3; channel++) {
    for(int32_t segment = 0; segment < segment_count; segment++) {
      scratch[segment] = segment_bases[segment * 3 + channel];
    }
    base[channel] =
      (float)select_kth_double(scratch, segment_count, segment_count / 2);
    for(int32_t segment = 0; segment < segment_count; segment++) {
      scratch[segment] = segment_noises[segment * 3 + channel];
    }
    noise[channel] =
      select_kth_double(scratch, segment_count, segment_count / 2);
  }

  /* per line: linear between the segment centres */
  for(int32_t line = d->film_start; line < d->film_end; line++) {
    for(uint32_t channel = 0; channel < 3; channel++) {
      double value;
      if(has_holes == false) {
        value = segment_bases[channel];
      } else {
        /* the line's position in segments, from the first centre */
        double position = (double)(line - d->film_start) / segment_lines - 0.5;
        int32_t below = (int32_t)floor(position);
        double weight = position - below;
        if(below < 0) {
          below = 0;
          weight = 0;
        }
        if(below >= segment_count - 1) {
          below = segment_count - 1;
          weight = 0;
        }
        value = segment_bases[below * 3 + channel] * (1 - weight);
        if(weight > 0) {
          value += segment_bases[(below + 1) * 3 + channel] * weight;
        }
      }
      if(!(value > MIN_BASE_TRANSMITTANCE)) value = MIN_BASE_TRANSMITTANCE;
      line_bases[(line - d->film_start) * 3 + channel] = (float)value;
    }
  }
  free_base_buffers(segment_bases, segment_noises, scratch, has_samples,
                    samples);
  return 0;
}

/* The open share of each film line in the columns [first, end). */
static void measure_open_profile(const detector* d, int32_t first, int32_t end,
                                 double* profile, double* mean) {
  int32_t film_lines = d->film_end - d->film_start;
  *mean = 0;
  for(int32_t line = 0; line < film_lines; line++) {
    int32_t open_count = 0;
    for(int32_t column = first; column < end; column++) {
      open_count += is_open(
        get_pixel(d->transmittance, d->pixels, d->film_start + line, column));
    }
    profile[line] = (double)open_count / (end - first);
    *mean += profile[line];
  }
  *mean /= film_lines;
}

/* Strip tilt from the two hole rows: the along-shift that best aligns the
 * right row's open profile with the left row's (sub-line by parabola).
 * Returns tan(tilt); *is_found is false (and the tilt 0) when the strip
 * is too short or nothing correlates. */
static double measure_holes_tilt(const detector* d, bool* is_found) {
  int32_t film_lines = d->film_end - d->film_start;
  /* the hole rows' centres across, mm */
  double left_mm = 0.5 * (d->holes_start + d->band_start) * d->mm_per_pixel;
  double right_mm = 0.5 * (d->band_end + d->holes_end) * d->mm_per_pixel;
  double* left_profile = calloc(film_lines, sizeof *left_profile);
  double* right_profile = calloc(film_lines, sizeof *right_profile);
  *is_found = false;
  if(left_profile == NULL || right_profile == NULL || right_mm - left_mm < 5) {
    free(left_profile);
    free(right_profile);
    return 0;
  }
  /* the largest shift to try, lines */
  int32_t max_shift = (int32_t)ceil((right_mm - left_mm) *
                                    tan(MAX_TILT_DEGREES * PI / 180) /
                                    d->mm_per_line) +
                      1;
  /* too short to correlate: no tilt */
  if(film_lines < 4 * max_shift) {
    free(left_profile);
    free(right_profile);
    return 0;
  }
  double left_mean;
  double right_mean;
  measure_open_profile(d, d->holes_start, d->band_start, left_profile,
                       &left_mean);
  measure_open_profile(d, d->band_end, d->holes_end, right_profile,
                       &right_mean);

  /* the cross-correlation at every shift; best_shift has the largest */
  double* correlations = calloc(2 * max_shift + 1, sizeof *correlations);
  if(correlations == NULL) {
    free(left_profile);
    free(right_profile);
    return 0;
  }
  int32_t best_shift = 0;
  double best_correlation = -1e300;
  for(int32_t shift = -max_shift; shift <= max_shift; shift++) {
    double sum = 0;
    for(int32_t line = max_shift; line < film_lines - max_shift; line++) {
      sum += (left_profile[line] - left_mean) *
             (right_profile[line + shift] - right_mean);
    }
    correlations[shift + max_shift] = sum;
    if(sum > best_correlation) {
      best_correlation = sum;
      best_shift = shift;
    }
  }
  /* the neighbours of the best shift, for the parabola */
  bool is_inside = best_shift > -max_shift && best_shift < max_shift;
  double before = 0;
  double after = 0;
  if(is_inside) {
    before = correlations[best_shift - 1 + max_shift];
    after = correlations[best_shift + 1 + max_shift];
  }
  free(correlations);
  free(left_profile);
  free(right_profile);
  /* no hole pattern match: no tilt */
  if(!(best_correlation > 0)) return 0;

  *is_found = true;
  double shift = best_shift;
  double curvature = before - 2 * best_correlation + after;
  if(is_inside && curvature < 0) shift += 0.5 * (before - after) / curvature;
  return shift * d->mm_per_line / (right_mm - left_mm);
}

/* A robust line y = intercept + slope x, from a start intercept (slope
 * 0): refit on the points within 3 rms (at least clip) until the set
 * settles. The first band is 3 x the MAD about the start, or just clip
 * when is_tight (a second cluster would widen the MAD over both).
 *
 * is_inlier  receives the points kept
 *
 * Returns the inlier count, 0 when degenerate. */
static int32_t fit_line_robustly(const double* x, const double* y,
                                 int32_t count, double intercept, double clip,
                                 bool is_tight, uint8_t* is_inlier,
                                 double* intercept_out, double* slope_out,
                                 double* rms_out) {
  const uint32_t MAX_ITERATIONS = 8;
  double slope = 0;
  double rms = 0;
  int32_t inlier_count = 0;

  if(is_tight == false) {
    double* residuals = malloc(count * sizeof *residuals);
    if(residuals == NULL) return 0;
    for(int32_t index = 0; index < count; index++) {
      residuals[index] = fabs(y[index] - intercept);
    }
    rms = MAD_TO_SIGMA * select_kth_double(residuals, count, count / 2);
    free(residuals);
  }
  for(uint32_t iteration = 0; iteration < MAX_ITERATIONS; iteration++) {
    double threshold = 3 * rms > clip ? 3 * rms : clip;
    bool has_changed = false;
    for(int32_t index = 0; index < count; index++) {
      uint8_t is_close =
        fabs(y[index] - (intercept + slope * x[index])) <= threshold;
      has_changed |= iteration == 0 || is_close != is_inlier[index];
      is_inlier[index] = is_close;
    }
    if(has_changed == false) break;

    /* least squares over the inliers */
    double sum_x = 0;
    double sum_y = 0;
    double sum_xx = 0;
    double sum_xy = 0;
    int32_t used = 0;
    for(int32_t index = 0; index < count; index++) {
      if(is_inlier[index] == 0) continue;
      sum_x += x[index];
      sum_y += y[index];
      sum_xx += x[index] * x[index];
      sum_xy += x[index] * y[index];
      used++;
    }
    double denominator = used * sum_xx - sum_x * sum_x;
    if(used < 3 || denominator <= 0) return 0;
    slope = (used * sum_xy - sum_x * sum_y) / denominator;
    intercept = (sum_y - slope * sum_x) / used;
    double square_sum = 0;
    for(int32_t index = 0; index < count; index++) {
      if(is_inlier[index] == 0) continue;
      double residual = y[index] - (intercept + slope * x[index]);
      square_sum += residual * residual;
    }
    rms = sqrt(square_sum / used);
    inlier_count = used;
  }
  *intercept_out = intercept;
  *slope_out = slope;
  *rms_out = rms;
  return inlier_count;
}

/* is (line, column) and the two lines beyond it in direction exposed? */
static bool is_exposed_start(const detector* d, int32_t line, int32_t column,
                             int32_t direction) {
  return get_state(d, line, column) == STATE_EXPOSED &&
         get_state(d, line + direction, column) == STATE_EXPOSED &&
         get_state(d, line + 2 * direction, column) == STATE_EXPOSED;
}

/* A frame edge of a gap. Per image column, walk from the gap middle
 * outward (direction -1: towards the frame before, +1: the frame after)
 * up to EDGE_WINDOW_MM past the gap boundary and take the first switch to
 * exposed that holds 3 lines (nearest the gap). Robust line fit: start
 * from the median, then iterate least squares on inliers within
 * max(3 rms, EDGE_CLIP_MM). Accepted with EDGE_FOUND_SHARE of the image
 * columns as inliers and inlier rms <= EDGE_RMS_MM: a straight edge
 * across the band. Curved boundaries (round shadows) have no such line.
 * Failing that, the outermost straight cluster with EDGE_OUTER_SHARE of
 * the columns and at most EDGE_BEYOND_SHARE past it (see below).
 *
 * position  receives the edge line at the image centre
 * slope     receives its slope, mm along per mm across
 *
 * Returns true when an edge was found. */
static bool fit_edge(const detector* d, const gap* the_gap, int32_t direction,
                     double* position, double* slope) {
  int32_t column_count = d->image_end - d->image_start;
  int32_t window_lines = (int32_t)(EDGE_WINDOW_MM / d->mm_per_line + 0.5);
  int32_t middle = (the_gap->start + the_gap->end) / 2;
  double centre_column = 0.5 * (d->image_start + d->image_end);
  double clip = EDGE_CLIP_MM / d->mm_per_line;
  double* x = malloc(column_count * sizeof *x);
  double* y = malloc(column_count * sizeof *y);
  double* scratch = malloc(column_count * sizeof *scratch);
  uint8_t* is_inlier = malloc(column_count);
  double intercept = 0;
  double fitted_slope = 0;
  double rms = 0;
  bool is_found = false;
  int32_t point_count = 0;

  if(x == NULL || y == NULL || scratch == NULL || is_inlier == NULL) {
    goto cleanup;
  }
  for(int32_t column = d->image_start; column < d->image_end; column++) {
    if(direction < 0) {
      /* stop 2 lines inside the film: is_exposed_start looks 2 back */
      int32_t lowest = the_gap->start - window_lines < d->film_start + 2
                         ? d->film_start + 2
                         : the_gap->start - window_lines;
      for(int32_t line = middle; line >= lowest; line--) {
        if(is_exposed_start(d, line, column, -1)) {
          x[point_count] = column - centre_column;
          /* the edge lies between line and line + 1 */
          y[point_count] = line + 1;
          point_count++;
          break;
        }
      }
    } else {
      int32_t highest = the_gap->end + window_lines > d->film_end - 3
                          ? d->film_end - 3
                          : the_gap->end + window_lines;
      for(int32_t line = middle; line <= highest; line++) {
        if(is_exposed_start(d, line, column, +1)) {
          x[point_count] = column - centre_column;
          y[point_count] = line;
          point_count++;
          break;
        }
      }
    }
  }
  if(point_count < 3 || point_count < EDGE_FOUND_SHARE * column_count) {
    goto cleanup;
  }

  /* start: the median, slope 0 (the lines are deskewed) */
  for(int32_t index = 0; index < point_count; index++)
    scratch[index] = y[index];
  int32_t inlier_count = fit_line_robustly(
    x, y, point_count, select_kth_double(scratch, point_count, point_count / 2),
    clip, false, is_inlier, &intercept, &fitted_slope, &rms);
  if(inlier_count >= EDGE_FOUND_SHARE * column_count &&
     rms * d->mm_per_line <= EDGE_RMS_MM) {
    is_found = true;
    goto done;
  }

  /* Thin content at the frame end only moves the found edge into the
   * frame, never into the gap: the true edge is the outermost straight
   * cluster. Start on the gap side; accept a smaller cluster when hardly
   * anything lies beyond it (hardware 2026-10-12: 53-56 % of columns on
   * a clean line, the rest up to 3 mm inside). */
  for(int32_t index = 0; index < point_count; index++)
    scratch[index] = y[index];
  int32_t start_position =
    direction < 0 ? point_count * 9 / 10 : point_count / 10;
  inlier_count = fit_line_robustly(
    x, y, point_count, select_kth_double(scratch, point_count, start_position),
    clip, true, is_inlier, &intercept, &fitted_slope, &rms);
  /* the points past the line, on the gap side */
  int32_t beyond_count = 0;
  for(int32_t index = 0; index < point_count; index++) {
    double residual = y[index] - (intercept + fitted_slope * x[index]);
    if(is_inlier[index] == 0 && residual * -direction > 0) beyond_count++;
  }
  if(inlier_count >= EDGE_OUTER_SHARE * column_count &&
     beyond_count <= EDGE_BEYOND_SHARE * column_count &&
     rms * d->mm_per_line <= EDGE_RMS_MM) {
    is_found = true;
  }

done:
  if(is_found) {
    /* at the image centre */
    *position = intercept;
    *slope = fitted_slope * d->mm_per_line / d->mm_per_pixel;
  }
cleanup:
  free(x);
  free(y);
  free(scratch);
  free(is_inlier);
  return is_found;
}

/* The deskew offsets, the gap score per line, the gap runs and their
 * edges. Returns how many gaps are trusted. */
static int32_t analyse_gaps(detector* d) {
  int32_t min_gap_lines = (int32_t)ceil(MIN_GAP_MM / d->mm_per_line);
  double centre_column = 0.5 * (d->image_start + d->image_end);

  for(int32_t column = 0; column < d->pixels; column++) {
    d->deskew_offsets[column] = (int32_t)floor((column - centre_column) *
                                                 d->mm_per_pixel *
                                                 d->tilt_tangent /
                                                 d->mm_per_line +
                                               0.5);
  }
  for(int32_t line = d->film_start; line < d->film_end; line++) {
    int32_t base_count = 0;
    int32_t exposed_count = 0;
    for(int32_t column = d->image_start; column < d->image_end; column++) {
      pixel_state state = get_state(d, line, column);
      base_count += state == STATE_BASE;
      exposed_count += state == STATE_EXPOSED;
    }
    if(d->is_no_film[line] != 0) {
      d->gap_scores[line] = 1;
    } else if(base_count + exposed_count >=
              0.5 * (d->image_end - d->image_start)) {
      d->gap_scores[line] = (double)base_count / (base_count + exposed_count);
    } else {
      d->gap_scores[line] = -1;
    }
  }

  d->gap_count = 0;
  int32_t start = -1;
  /* one step past the end, so a gap reaching it is closed too */
  for(int32_t line = d->film_start; line <= d->film_end; line++) {
    bool is_gap_line = line < d->film_end && d->gap_scores[line] >= GAP_SHARE;
    if(is_gap_line && start < 0) start = line;
    if(is_gap_line == false && start >= 0) {
      if(line - start >= min_gap_lines) {
        gap* the_gap = &d->gaps[d->gap_count];
        d->gap_count++;
        memset(the_gap, 0, sizeof *the_gap);
        the_gap->start = start;
        the_gap->end = line;
        for(int32_t inside = start; inside < line && !the_gap->has_no_film;
            inside++) {
          the_gap->has_no_film = d->is_no_film[inside] != 0;
        }
      }
      start = -1;
    }
  }

  int32_t trusted_count = 0;
  for(int32_t index = 0; index < d->gap_count; index++) {
    gap* the_gap = &d->gaps[index];
    if(the_gap->start > d->film_start) {
      the_gap->has_lead_edge =
        fit_edge(d, the_gap, -1, &the_gap->lead_position, &the_gap->lead_slope);
    }
    if(the_gap->end < d->film_end) {
      the_gap->has_trail_edge = fit_edge(
        d, the_gap, +1, &the_gap->trail_position, &the_gap->trail_slope);
    }
    the_gap->is_trusted = the_gap->has_lead_edge || the_gap->has_trail_edge;
    trusted_count += the_gap->is_trusted;
  }
  return trusted_count;
}

/* is this a full or half frame's length? */
static bool is_frame_sized(double length_mm) {
  bool is_half = length_mm >= HALF_MIN_MM && length_mm <= HALF_MAX_MM;
  bool is_full = length_mm >= FULL_MIN_MM && length_mm <= FULL_MAX_MM;
  return is_half || is_full;
}

/* How an exposed run counts, from its length. */
typedef struct run_class {
  ft_frame_type type;
  /* the run is unsure */
  bool is_confidence_low;
  /* the run is a frame at all */
  bool should_keep;
} run_class;

static run_class classify_run(double length_mm, bool is_at_end) {
  run_class result = {
    .type = FT_FRAME_PARTIAL, .is_confidence_low = false, .should_keep = true};
  if(is_at_end) {
    if(length_mm < END_SLIVER_MM) {
      result.should_keep = false;
    } else if(fabs(length_mm - FULL_LENGTH_MM) <= END_LENGTH_TOLERANCE_MM) {
      result.type = FT_FRAME_FULL;
    } else if(fabs(length_mm - HALF_LENGTH_MM) <= END_LENGTH_TOLERANCE_MM) {
      result.type = FT_FRAME_HALF;
    } else {
      result.type = FT_FRAME_PARTIAL;
      /* longer than any frame: unsure */
      result.is_confidence_low = length_mm > FULL_MAX_MM;
    }
    return result;
  }
  if(length_mm >= HALF_MIN_MM && length_mm <= HALF_MAX_MM) {
    result.type = FT_FRAME_HALF;
    return result;
  }
  if(length_mm >= FULL_MIN_MM && length_mm <= FULL_MAX_MM) {
    result.type = FT_FRAME_FULL;
    return result;
  }
  if(length_mm > FULL_MAX_MM) {
    result.type = FT_FRAME_PANORAMA;
    return result;
  }
  if(length_mm < MIN_RUN_MM) {
    result.should_keep = false;
    return result;
  }
  /* between the types: the nearest one, flagged */
  result.is_confidence_low = true;
  result.type = length_mm < HALF_OR_FULL_MM ? FT_FRAME_HALF : FT_FRAME_FULL;
  return result;
}

/* the next frame of the layout, zeroed and numbered; NULL when full */
static ft_detected_frame* add_frame(ft_layout* layout) {
  if(layout->frame_count == FT_MAX_FRAMES) return NULL;
  ft_detected_frame* frame = &layout->frames[layout->frame_count];
  layout->frame_count++;
  memset(frame, 0, sizeof *frame);
  frame->index = layout->frame_count;
  return frame;
}

/* The image's extent across over the lines [start, end): the columns
 * exposed on >= 30 % of their usable pixels. */
static void find_run_extent(const detector* d, int32_t start, int32_t end,
                            int32_t* first_column, int32_t* end_column) {
  const double EXPOSED_SHARE = 0.3;
  *first_column = -1;
  *end_column = -1;
  for(int32_t column = d->image_start; column < d->image_end; column++) {
    int64_t exposed_count = 0;
    int64_t usable_count = 0;
    for(int32_t line = start; line < end; line++) {
      pixel_state state = get_state(d, line, column);
      exposed_count += state == STATE_EXPOSED;
      usable_count += state != STATE_SKIP;
    }
    if(usable_count > 0 && exposed_count >= EXPOSED_SHARE * usable_count) {
      if(*first_column < 0) *first_column = column;
      *end_column = column + 1;
    }
  }
  if(*first_column < 0) {
    *first_column = d->image_start;
    *end_column = d->image_end;
  }
}

/* fill in a frame's position from lines and columns */
static void place_frame(const detector* d, const ft_axes* axes,
                        ft_detected_frame* frame, double first_line,
                        double end_line, int32_t first_column,
                        int32_t end_column) {
  frame->start_mm = axes->first_line_mm + first_line * d->mm_per_line;
  frame->end_mm = axes->first_line_mm + end_line * d->mm_per_line;
  frame->across_start_mm =
    axes->first_pixel_mm + first_column * d->mm_per_pixel;
  frame->across_end_mm = axes->first_pixel_mm + end_column * d->mm_per_pixel;
}

/* A stretch with no trusted edge inside: tile it with the holder pitch
 * from its trusted end, every window flagged. */
static void grid_stretch(const detector* d, ft_layout* layout,
                         const ft_holder* holder, const ft_axes* axes,
                         double first_line, double end_line, bool from_end,
                         int32_t first_column, int32_t end_column) {
  double pitch_lines = holder->grid_pitch_mm / d->mm_per_line;
  int32_t window_count =
    (int32_t)ceil((end_line - first_line) / pitch_lines - 1e-9);
  layout->used_fallback = true;
  for(int32_t index = 0; index < window_count; index++) {
    double window_start = from_end
                            ? end_line - (window_count - index) * pitch_lines
                            : first_line + index * pitch_lines;
    double window_end = window_start + pitch_lines;
    if(window_start < first_line) window_start = first_line;
    if(window_end > end_line) window_end = end_line;
    double length_mm = (window_end - window_start) * d->mm_per_line;
    if(length_mm < MIN_RUN_MM) continue;
    ft_detected_frame* frame = add_frame(layout);
    if(frame == NULL) continue;
    frame->type = length_mm >= FULL_MIN_MM ? FT_FRAME_FULL : FT_FRAME_PARTIAL;
    place_frame(d, axes, frame, window_start, window_end, first_column,
                end_column);
    frame->tilt_degrees = atan(d->tilt_tangent) * 180 / PI;
    frame->is_confidence_low = true;
  }
}

/* An exposed run [start, end) between the gaps before and after (NULL at
 * a strip end): one frame, a grid of them, or nothing. */
static void add_run(const detector* d, ft_layout* layout,
                    const ft_holder* holder, const ft_axes* axes, int32_t start,
                    int32_t end, const gap* before, const gap* after) {
  double mm_per_line = d->mm_per_line;
  bool is_at_end = before == NULL || after == NULL;
  int32_t cut_lines = (int32_t)(END_CUT_MM / d->mm_per_line + 0.5);

  /* at the strip ends: start past the last cut line, end before the
   * first */
  if(before == NULL) {
    int32_t from = start + cut_lines < end ? start + cut_lines : end - 1;
    for(int32_t line = from; line >= start; line--) {
      if(get_band_open_share(d, line) >= CUT_OPEN_SHARE) {
        start = line + 1;
        break;
      }
    }
  }
  if(after == NULL) {
    int32_t from = end - cut_lines > start ? end - cut_lines : start;
    for(int32_t line = from; line < end; line++) {
      if(get_band_open_share(d, line) >= CUT_OPEN_SHARE) {
        end = line;
        break;
      }
    }
  }
  if(end <= start) return;

  /* the run's ends: the trusted edges, else the gap boundaries */
  double first_line = start;
  if(before != NULL) {
    first_line = before->has_trail_edge ? before->trail_position : before->end;
  }
  double end_line = end;
  if(after != NULL) {
    end_line = after->has_lead_edge ? after->lead_position : after->start;
  }
  double length_mm = (end_line - first_line) * mm_per_line;

  int32_t first_column;
  int32_t end_column;
  find_run_extent(d, start, end, &first_column, &end_column);
  if(length_mm > GRID_RUN_MM) {
    grid_stretch(d, layout, holder, axes, first_line, end_line,
                 before == NULL && after != NULL, first_column, end_column);
    return;
  }
  run_class run = classify_run(length_mm, is_at_end);
  if(run.should_keep == false) return;

  /* a panorama is unsure when near-gap lines cross it (maybe a gap we
   * did not trust) or when it is not a real panorama's length */
  double near_gap_mm = 0;
  for(int32_t line = start; line < end; line++) {
    if(d->gap_scores[line] >= NEAR_GAP_SHARE) near_gap_mm += mm_per_line;
  }
  if(run.type == FT_FRAME_PANORAMA && (near_gap_mm >= 0.3 ||
                                       length_mm < PANORAMA_MIN_MM ||
                                       length_mm > PANORAMA_MAX_MM)) {
    run.is_confidence_low = true;
  }

  /* One trusted edge and one untrusted: base-like content at the frame
   * end may have moved the gap boundary into the frame. Extend to the
   * nominal length from the trusted edge, staying inside the gap. */
  bool is_whole_or_half =
    run.type == FT_FRAME_FULL || run.type == FT_FRAME_HALF;
  if(is_at_end == false && run.is_confidence_low == false && is_whole_or_half) {
    double nominal_lines =
      (run.type == FT_FRAME_FULL ? FULL_LENGTH_MM : HALF_LENGTH_MM) /
      mm_per_line;
    if(end_line - first_line < nominal_lines - 0.2 / mm_per_line) {
      if(before->has_trail_edge == false && after->has_lead_edge) {
        first_line = end_line - nominal_lines;
        if(first_line < before->start) first_line = before->start;
      } else if(before->has_trail_edge && after->has_lead_edge == false) {
        end_line = first_line + nominal_lines;
        if(end_line > after->end) end_line = after->end;
      }
    }
  }

  /* the frame's own tilt: the strip's plus the mean edge slope */
  double slope_sum = 0;
  int32_t slope_count = 0;
  if(before != NULL && before->has_trail_edge) {
    slope_sum += before->trail_slope;
    slope_count++;
  }
  if(after != NULL && after->has_lead_edge) {
    slope_sum += after->lead_slope;
    slope_count++;
  }
  ft_detected_frame* frame = add_frame(layout);
  if(frame == NULL) return;
  frame->type = run.type;
  place_frame(d, axes, frame, first_line, end_line, first_column, end_column);
  double mean_slope = slope_count > 0 ? slope_sum / slope_count : 0;
  frame->tilt_degrees = atan(d->tilt_tangent + mean_slope) * 180 / PI;
  frame->is_confidence_low = run.is_confidence_low;
}

/* The blank frames in a long trusted gap, by the local pitch: half
 * frames next to it give 19 mm, else 38 mm. At least 1. */
static uint32_t count_blank_frames(const ft_layout* layout, const ft_axes* axes,
                                   double mm_per_line, const gap* the_gap) {
  double start_mm = axes->first_line_mm + the_gap->start * mm_per_line;
  double end_mm = axes->first_line_mm + the_gap->end * mm_per_line;
  double pitch_mm = FULL_PITCH_MM;
  for(uint32_t index = 0; index < layout->frame_count; index++) {
    const ft_detected_frame* frame = &layout->frames[index];
    if(frame->type == FT_FRAME_HALF &&
       (fabs(frame->end_mm - start_mm) < NEAR_GAP_MM ||
        fabs(frame->start_mm - end_mm) < NEAR_GAP_MM)) {
      pitch_mm = HALF_PITCH_MM;
    }
  }
  int32_t count = (int32_t)((end_mm - start_mm) / pitch_mm + 0.5);
  return count < 1 ? 1 : (uint32_t)count;
}

static void free_detector(detector* d) {
  free(d->states);
  free(d->deskew_offsets);
  free(d->gap_scores);
  free(d->is_no_film);
  free(d->gaps);
}

static bool are_axes_valid(const ft_axes* axes) {
  return axes->mm_per_line > 0 &&
         axes->mm_per_pixel > 0 &&
         isfinite(axes->mm_per_line) &&
         isfinite(axes->mm_per_pixel) &&
         isfinite(axes->first_line_mm) &&
         isfinite(axes->first_pixel_mm);
}

/* Step 1: lines with open holder, holder metal, or film. Film = the
 * first..last film line; dark lines inside (slide Dmax between the
 * holes) are film. Returns false when there is no film. */
static bool find_film_lines(detector* d) {
  int32_t pixels = d->pixels;
  d->film_start = -1;
  for(int32_t line = 0; line < d->lines; line++) {
    int32_t open_count = 0;
    int32_t dark_count = 0;
    for(int32_t column = 0; column < pixels; column++) {
      const float* pixel = get_pixel(d->transmittance, pixels, line, column);
      open_count += is_open(pixel);
      dark_count += is_dark(pixel);
    }
    /* Metal: dark with no hole on the line. Slide Dmax can be as dark
     * as metal, but film lines through the hole rows show open beam
     * (~17 % of the line); lines between holes are interior. */
    bool is_open_line = open_count > LINE_SHARE * pixels;
    bool is_metal_line =
      dark_count > LINE_SHARE * pixels && open_count < HOLE_LINE_SHARE * pixels;
    if(is_open_line == false && is_metal_line == false) {
      if(d->film_start < 0) d->film_start = line;
      d->film_end = line + 1;
    }
  }
  return d->film_start >= 0;
}

/* Step 4: the state of every film pixel. Negative: exposed = darker than
 * the base by BASE_DENSITY (and the noise floor); slide: brighter. */
static void classify_pixels(detector* d, const uint8_t* halo,
                            const float* line_bases, const double noise[3]) {
  bool is_negative = d->film == FT_FILM_NEGATIVE;
  double factor = pow(10, is_negative ? -BASE_DENSITY : BASE_DENSITY);
  double noise_sigmas = is_negative ? -NOISE_SIGMAS : NOISE_SIGMAS;
  for(int32_t line = d->film_start; line < d->film_end; line++) {
    const float* base = line_bases + (line - d->film_start) * 3;
    float thresholds[3];
    for(uint32_t channel = 0; channel < 3; channel++) {
      thresholds[channel] =
        (float)(base[channel] * factor + noise_sigmas * noise[channel]);
    }
    for(int32_t column = 0; column < d->pixels; column++) {
      int64_t at = (int64_t)line * d->pixels + column;
      const float* pixel = get_pixel(d->transmittance, d->pixels, line, column);
      if(halo[at] != 0 || is_open(pixel)) {
        d->states[at] = STATE_SKIP;
        continue;
      }
      bool is_exposed;
      if(is_negative) {
        is_exposed = pixel[0] < thresholds[0] ||
                     pixel[1] < thresholds[1] ||
                     pixel[2] < thresholds[2];
      } else {
        is_exposed = pixel[0] > thresholds[0] ||
                     pixel[1] > thresholds[1] ||
                     pixel[2] > thresholds[2];
      }
      d->states[at] = is_exposed ? STATE_EXPOSED : STATE_BASE;
    }
  }
}

/* Step 7: no trusted edge anywhere on a long strip: the holder grid,
 * every frame flagged. */
static void grid_whole_strip(const detector* d, ft_layout* layout,
                             const ft_holder* holder, const ft_axes* axes) {
  double film_start_mm = layout->film_start_mm;
  double film_end_mm = layout->film_end_mm;
  layout->used_fallback = true;
  for(uint32_t grid_index = 0; grid_index < holder->grid_frame_count;
      grid_index++) {
    double start_mm =
      holder->grid_first_frame_mm + grid_index * holder->grid_pitch_mm;
    double end_mm = start_mm + holder->grid_frame_length_mm;
    if(start_mm < film_start_mm) start_mm = film_start_mm;
    if(end_mm > film_end_mm) end_mm = film_end_mm;
    if(end_mm - start_mm < MIN_RUN_MM) continue;
    ft_detected_frame* frame = add_frame(layout);
    if(frame == NULL) continue;
    frame->type = FT_FRAME_FULL;
    frame->start_mm = start_mm;
    frame->end_mm = end_mm;
    frame->across_start_mm =
      axes->first_pixel_mm + d->image_start * d->mm_per_pixel;
    frame->across_end_mm =
      axes->first_pixel_mm + d->image_end * d->mm_per_pixel;
    frame->tilt_degrees = layout->tilt_degrees;
    frame->is_confidence_low = true;
  }
}

int32_t ft_detect_frames(const float* transmittance, uint32_t lines,
                         uint32_t pixels, const ft_axes* axes, ft_film film,
                         const ft_holder* holder, ft_layout* layout,
                         ft_err* error) {
  const uint32_t MIN_SIDE = 8;
  if(layout == NULL) return ft_fail(error, "detect: no layout");
  memset(layout, 0, sizeof *layout);
  if(transmittance == NULL ||
     axes == NULL ||
     holder == NULL ||
     lines < MIN_SIDE ||
     pixels < MIN_SIDE ||
     lines > INT32_MAX ||
     pixels > INT32_MAX) {
    return ft_fail(error, "detect: bad arguments");
  }
  if(are_axes_valid(axes) == false) return ft_fail(error, "detect: bad axes");

  detector detector_state;
  detector* d = &detector_state;
  memset(d, 0, sizeof *d);
  d->transmittance = transmittance;
  d->lines = (int32_t)lines;
  d->pixels = (int32_t)pixels;
  d->mm_per_line = axes->mm_per_line;
  d->mm_per_pixel = axes->mm_per_pixel;
  d->film = film;
  int32_t result = -1;
  uint8_t* halo = NULL;
  /* a share per column, or per line: big enough for either */
  double* shares = calloc(pixels > lines ? pixels : lines, sizeof *shares);
  float* line_bases = malloc((size_t)lines * 3 * sizeof *line_bases);
  d->states = calloc((size_t)lines * pixels, 1);
  d->deskew_offsets = calloc(pixels, sizeof *d->deskew_offsets);
  d->gap_scores = calloc(lines, sizeof *d->gap_scores);
  d->is_no_film = calloc(lines, 1);
  d->gaps = calloc(lines / 2 + 1, sizeof *d->gaps);
  if(shares == NULL ||
     line_bases == NULL ||
     d->states == NULL ||
     d->deskew_offsets == NULL ||
     d->gap_scores == NULL ||
     d->is_no_film == NULL ||
     d->gaps == NULL) {
    ft_fail(error, "detect: out of memory");
    goto cleanup;
  }

  /* 1. film lines */
  if(find_film_lines(d) == false) {
    ft_fail(error, "detect: no film in the holder");
    goto cleanup;
  }
  layout->film_start_mm = axes->first_line_mm + d->film_start * d->mm_per_line;
  layout->film_end_mm = axes->first_line_mm + d->film_end * d->mm_per_line;

  /* 2. the open share per column over the film lines -> the band and the
   * hole rows */
  for(int32_t column = 0; column < d->pixels; column++) {
    int64_t open_count = 0;
    for(int32_t line = d->film_start; line < d->film_end; line++) {
      open_count += is_open(get_pixel(transmittance, d->pixels, line, column));
    }
    shares[column] = (double)open_count / (d->film_end - d->film_start);
  }
  int32_t band_start;
  int32_t band_end;
  if(find_widest_run(shares, d->pixels, -1, 0.25, &band_start, &band_end) *
       d->mm_per_pixel <
     10) {
    ft_fail(error, "detect: no film band found");
    goto cleanup;
  }
  int32_t holes_left =
    walk_while_in_range(shares, d->pixels, band_start - 1, -1, 0.25, 0.9) + 1;
  int32_t holes_right =
    walk_while_in_range(shares, d->pixels, band_end, +1, 0.25, 0.9);
  layout->has_holes =
    (band_start - holes_left) * d->mm_per_pixel >= HOLE_MIN_MM &&
    (holes_right - band_end) * d->mm_per_pixel >= HOLE_MIN_MM;
  d->band_start = band_start;
  d->band_end = band_end;
  d->holes_start = layout->has_holes ? holes_left : band_start;
  d->holes_end = layout->has_holes ? holes_right : band_end;
  layout->band_start_mm = axes->first_pixel_mm + band_start * d->mm_per_pixel;
  layout->band_end_mm = axes->first_pixel_mm + band_end * d->mm_per_pixel;
  layout->holes_start_mm =
    axes->first_pixel_mm + d->holes_start * d->mm_per_pixel;
  layout->holes_end_mm = axes->first_pixel_mm + d->holes_end * d->mm_per_pixel;

  for(int32_t line = d->film_start; line < d->film_end; line++) {
    d->is_no_film[line] = get_band_open_share(d, line) >= NO_FILM_OPEN_SHARE;
  }

  /* 3. the base level */
  double noise[3];
  halo = mark_open_halo(d);
  if(halo == NULL) {
    ft_fail(error, "detect: out of memory");
    goto cleanup;
  }
  if(measure_base_levels(d, halo, layout->has_holes, line_bases,
                         layout->base_transmittance, noise) != 0) {
    ft_fail(error, "detect: no film base found");
    goto cleanup;
  }

  /* 4. the pixel states */
  classify_pixels(d, halo, line_bases, noise);

  /* 5. the image columns: ever exposed inside the band */
  for(int32_t column = 0; column < d->pixels; column++) {
    shares[column] = 0;
    if(column < band_start || column >= band_end) continue;
    int64_t exposed_count = 0;
    int64_t usable_count = 0;
    for(int32_t line = d->film_start; line < d->film_end; line++) {
      uint8_t state = d->states[(int64_t)line * d->pixels + column];
      exposed_count += state == STATE_EXPOSED;
      usable_count += state != STATE_SKIP;
    }
    if(usable_count > 0) {
      shares[column] = (double)exposed_count / usable_count;
    }
  }
  if(find_widest_run(shares, d->pixels, 0.05, 2.0, &d->image_start,
                     &d->image_end) *
       d->mm_per_pixel <
     5) {
    /* nothing exposed: the whole strip is blank frames */
    double film_mm = (d->film_end - d->film_start) * d->mm_per_line;
    layout->blank_count = (uint32_t)(film_mm / FULL_PITCH_MM + 0.5);
    result = 0;
    goto cleanup;
  }

  /* 6. tilt, gaps, edges. Without a hole-row tilt: a first pass
   * untilted, then again at the median slope of the accepted edges. */
  bool has_holes_tilt = false;
  d->tilt_tangent =
    layout->has_holes ? measure_holes_tilt(d, &has_holes_tilt) : 0;
  int32_t trusted_count = analyse_gaps(d);
  if(has_holes_tilt == false && trusted_count > 0) {
    double slopes[2 * FT_MAX_FRAMES];
    int32_t slope_count = 0;
    for(int32_t index = 0;
        index < d->gap_count && slope_count < 2 * FT_MAX_FRAMES - 1; index++) {
      if(d->gaps[index].has_lead_edge) {
        slopes[slope_count] = d->gaps[index].lead_slope;
        slope_count++;
      }
      if(d->gaps[index].has_trail_edge) {
        slopes[slope_count] = d->gaps[index].trail_slope;
        slope_count++;
      }
    }
    if(slope_count > 0) {
      double median = select_kth_double(slopes, slope_count, slope_count / 2);
      if(fabs(median) > tan(MIN_TILT_DEGREES * PI / 180)) {
        d->tilt_tangent = median;
        trusted_count = analyse_gaps(d);
      }
    }
  }
  layout->tilt_degrees = atan(d->tilt_tangent) * 180 / PI;

  /* 7. no trusted edge anywhere on a long strip: the holder grid */
  if(trusted_count == 0 &&
     (d->film_end - d->film_start) * d->mm_per_line > FALLBACK_MM) {
    grid_whole_strip(d, layout, holder, axes);
    result = 0;
    goto cleanup;
  }

  /* 8. exposed runs between trusted gaps */
  int32_t run_start = d->film_start;
  const gap* before = NULL;
  /* one step past the last gap: the run up to the film's end */
  for(int32_t index = 0; index <= d->gap_count; index++) {
    const gap* the_gap = index < d->gap_count ? &d->gaps[index] : NULL;
    /* Untrusted gaps stay in the run, unless they end a frame-sized
     * piece: gap lines across the band mark a frame end even when dark
     * content hides the edge itself. */
    if(the_gap != NULL &&
       the_gap->is_trusted == false &&
       is_frame_sized((the_gap->start - run_start) * d->mm_per_line) == false) {
      continue;
    }
    int32_t run_end = the_gap != NULL ? the_gap->start : d->film_end;
    if(run_end > run_start) {
      add_run(d, layout, holder, axes, run_start, run_end, before, the_gap);
    }
    if(the_gap != NULL) {
      run_start = the_gap->end;
      before = the_gap;
    }
  }

  /* 9. long trusted gaps are blank frames; at the strip ends they may
   * be leader, so they are counted apart */
  for(int32_t index = 0; index < d->gap_count; index++) {
    const gap* the_gap = &d->gaps[index];
    bool is_blank = the_gap->is_trusted &&
                    the_gap->has_no_film == false &&
                    (the_gap->end - the_gap->start) * d->mm_per_line > BLANK_MM;
    if(is_blank == false) continue;
    uint32_t blanks = count_blank_frames(layout, axes, d->mm_per_line, the_gap);
    if(the_gap->start > d->film_start && the_gap->end < d->film_end) {
      layout->blank_count += blanks;
    } else {
      layout->blank_end_count += blanks;
    }
  }
  result = 0;

cleanup:
  free(shares);
  free(halo);
  free(line_bases);
  free_detector(d);
  return result;
}

static double clamp(double value, double low, double high) {
  if(value < low) return low;
  if(value > high) return high;
  return value;
}

/* Windows are clamped to the holder's crop limits: the preview reaches
 * past them (across 0.92..33.14 mm, along ~232 mm), so sprockets-in
 * windows and frames cut by the strip end would otherwise fail
 * ft_validate_job. */
void ft_convert_layout_to_job(const ft_layout* layout, const ft_holder* holder,
                              ft_film film, double ppi, bool has_sprockets,
                              ft_job* job) {
  memset(job, 0, sizeof *job);
  snprintf(job->holder, sizeof job->holder, "%s", holder->name);
  job->film = film;
  job->focus_mode = FT_FOCUS_SHIFTED;
  /* the render's film base (negative) or black (slide), as density */
  for(uint32_t channel = 0; channel < 3; channel++) {
    double transmittance = layout->base_transmittance[channel];
    if(!(transmittance > FT_TRANSMITTANCE_FLOOR)) {
      transmittance = FT_TRANSMITTANCE_FLOOR;
    }
    /* A channel may exceed T = 1, and -log10(FT_TRANSMITTANCE_FLOOR) is a
     * hair above 4.5; the job reader accepts [0, 4.5] only. */
    job->film_base_density[channel] =
      clamp(-log10(transmittance), 0, MAX_FILM_BASE_DENSITY);
  }
  job->has_film_base = true;
  for(uint32_t index = 0; index < layout->frame_count && index < FT_MAX_FRAMES;
      index++) {
    const ft_detected_frame* detected = &layout->frames[index];
    ft_job_frame* frame = &job->frames[job->frame_count];
    job->frame_count++;
    double across_start =
      has_sprockets ? layout->holes_start_mm : detected->across_start_mm;
    double across_end =
      has_sprockets ? layout->holes_end_mm : detected->across_end_mm;
    double along_start = clamp(detected->start_mm, 0, holder->scan_length_mm);
    double along_end = clamp(detected->end_mm, 0, holder->scan_length_mm);
    across_start = clamp(across_start, 0, holder->scan_width_mm);
    across_end = clamp(across_end, 0, holder->scan_width_mm);
    frame->index = detected->index;
    frame->type = detected->type;
    frame->is_included = true;
    frame->ppi = ppi;
    frame->along_start_mm = along_start;
    frame->length_mm = along_end - along_start;
    frame->across_start_mm = across_start;
    frame->width_mm = across_end - across_start;
    frame->tilt_degrees = detected->tilt_degrees;
    frame->has_sprockets = has_sprockets;
    frame->is_confidence_low = detected->is_confidence_low;
  }
}

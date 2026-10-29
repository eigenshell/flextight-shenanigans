/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_calib.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_geom.h"

/* each kind of calibration needs at least this many lines */
#define MIN_REFERENCE_LINES 10
/* a reference range below 1 DN is treated as 1, to never divide by 0 */
#define MIN_SPAN 1
/* the Faul Pixs dump: CCD pixel 100 with value 0 starts a channel,
 * value +999 marks a faulty pixel */
#define CHANNEL_MARKER_PIXEL 100

/* What a calibration line shows, by its mean level. */
typedef enum line_level { LINE_DARK, LINE_WHITE, LINE_NEITHER } line_level;

/* Classify every line as dark, white or neither: within
 * FT_CALIBRATION_BAND of the range from the darkest or brightest line
 * mean. Returns false when memory runs out. */
static bool classify_lines(const uint16_t* calibration, uint32_t lines,
                           size_t values_per_line, uint8_t* levels) {
  double* line_means = malloc(lines * sizeof *line_means);
  if(line_means == NULL) return false;
  for(size_t line = 0; line < lines; line++) {
    double sum = 0;
    for(size_t index = 0; index < values_per_line; index++) {
      sum += calibration[line * values_per_line + index];
    }
    line_means[line] = sum / values_per_line;
  }

  double darkest = line_means[0];
  double brightest = line_means[0];
  for(size_t line = 1; line < lines; line++) {
    if(line_means[line] < darkest) darkest = line_means[line];
    if(line_means[line] > brightest) brightest = line_means[line];
  }
  double band = FT_CALIBRATION_BAND * (brightest - darkest);
  for(size_t line = 0; line < lines; line++) {
    levels[line] = LINE_NEITHER;
    if(line_means[line] > brightest - band) levels[line] = LINE_WHITE;
    /* checked last: a line that is both (no range at all) counts dark */
    if(line_means[line] < darkest + band) levels[line] = LINE_DARK;
  }
  free(line_means);
  return true;
}

int32_t ft_split_calibration(const uint16_t* calibration, uint32_t lines,
                             uint32_t pixels, float* dark, float* white,
                             ft_err* error) {
  if(lines < 1 || pixels < 1) {
    return ft_fail(error, "calibration: bad dimensions");
  }
  size_t values_per_line = (size_t)pixels * 3;
  uint8_t* levels = malloc(lines);
  double* dark_sums = calloc(values_per_line, sizeof *dark_sums);
  double* white_sums = calloc(values_per_line, sizeof *white_sums);
  int32_t result = 0;

  if(levels == NULL ||
     dark_sums == NULL ||
     white_sums == NULL ||
     classify_lines(calibration, lines, values_per_line, levels) == false) {
    result = ft_fail(error, "calibration: out of memory");
    goto cleanup;
  }

  uint32_t dark_count = 0;
  uint32_t white_count = 0;
  for(size_t line = 0; line < lines; line++) {
    double* sums = NULL;
    switch((line_level)levels[line]) {
      case LINE_DARK:
        sums = dark_sums;
        dark_count++;
        break;
      case LINE_WHITE:
        sums = white_sums;
        white_count++;
        break;
      case LINE_NEITHER:
        break;
    }
    if(sums == NULL) continue;
    for(size_t index = 0; index < values_per_line; index++) {
      sums[index] += calibration[line * values_per_line + index];
    }
  }
  if(dark_count < MIN_REFERENCE_LINES || white_count < MIN_REFERENCE_LINES) {
    result = ft_fail(error, "calibration: %u dark / %u white lines",
                     (unsigned)dark_count, (unsigned)white_count);
    goto cleanup;
  }
  for(size_t index = 0; index < values_per_line; index++) {
    dark[index] = (float)(dark_sums[index] / dark_count);
    white[index] = (float)(white_sums[index] / white_count);
  }

/* one exit, so every buffer is freed on every path */
cleanup:
  free(levels);
  free(dark_sums);
  free(white_sums);
  return result;
}

int32_t ft_measure_white_variance(const uint16_t* calibration, uint32_t lines,
                                  uint32_t pixels, double variance[3],
                                  ft_err* error) {
  size_t values_per_line = (size_t)pixels * 3;
  float* dark = malloc(values_per_line * sizeof *dark);
  float* white = malloc(values_per_line * sizeof *white);
  double* square_sums = calloc(values_per_line, sizeof *square_sums);
  /* malloc(0) may return NULL, so ask for at least 1 byte */
  uint8_t* levels = malloc(lines > 0 ? lines : 1);
  double channel_sums[3] = {0, 0, 0};
  int32_t result = -1;

  if(dark == NULL || white == NULL || square_sums == NULL || levels == NULL) {
    ft_fail(error, "calibration: out of memory");
    goto cleanup;
  }
  if(ft_split_calibration(calibration, lines, pixels, dark, white, error) !=
     0) {
    goto cleanup;
  }
  if(classify_lines(calibration, lines, values_per_line, levels) == false) {
    ft_fail(error, "calibration: out of memory");
    goto cleanup;
  }

  uint32_t white_count = 0;
  for(size_t line = 0; line < lines; line++) {
    if(levels[line] != LINE_WHITE) continue;
    white_count++;
    for(size_t index = 0; index < values_per_line; index++) {
      double difference =
        calibration[line * values_per_line + index] - white[index];
      square_sums[index] += difference * difference;
    }
  }
  for(size_t index = 0; index < values_per_line; index++) {
    double span = white[index] - dark[index];
    if(span < MIN_SPAN) span = MIN_SPAN;
    /* index % 3 is the channel: values go red, green, blue */
    channel_sums[index % 3] += square_sums[index] / white_count / (span * span);
  }
  for(uint32_t channel = 0; channel < 3; channel++) {
    variance[channel] = channel_sums[channel] / pixels;
  }
  result = 0;

cleanup:
  free(dark);
  free(white);
  free(square_sums);
  free(levels);
  return result;
}

void ft_apply_flat_field(const uint16_t* raw, uint32_t lines, uint32_t pixels,
                         const float* dark, const float* white,
                         float* transmittance) {
  size_t values_per_line = (size_t)pixels * 3;
  for(size_t line = 0; line < lines; line++) {
    for(size_t index = 0; index < values_per_line; index++) {
      size_t at = line * values_per_line + index;
      float span = white[index] - dark[index];
      if(span < MIN_SPAN) span = MIN_SPAN;
      float value = (raw[at] - dark[index]) / span;
      if(value < 0) value = 0;
      if(value > 1) value = 1;
      transmittance[at] = value;
    }
  }
}

uint32_t ft_parse_faulty_pixels(const char* text, size_t text_length,
                                int32_t faulty[3][FT_FAULTY_PIXEL_MAX],
                                uint32_t count[3]) {
  /* the channel the entries belong to; -1 before the first marker */
  int32_t channel = -1;
  bool is_header = true;
  size_t position = 0;
  count[0] = 0;
  count[1] = 0;
  count[2] = 0;

  while(position < text_length) {
    /* copy one line; a longer one is cut at 63 characters */
    char line[64];
    size_t line_length = 0;
    while(position < text_length &&
          text[position] != '\n' &&
          text[position] != '\r' &&
          text[position] != '\0') {
      if(line_length < sizeof line - 1) {
        line[line_length] = text[position];
        line_length++;
      }
      position++;
    }
    position++;
    line[line_length] = '\0';
    if(is_header) {
      is_header = false;
      continue;
    }

    long pixel;
    char value[16];
    if(sscanf(line, " %ld %15s", &pixel, value) != 2) continue;
    if(pixel == CHANNEL_MARKER_PIXEL && strcmp(value, "0") == 0) {
      channel++;
      continue;
    }
    bool is_faulty = channel >= 0 && channel < 3 && strcmp(value, "+999") == 0;
    if(is_faulty && count[channel] < FT_FAULTY_PIXEL_MAX) {
      faulty[channel][count[channel]] = (int32_t)pixel;
      count[channel]++;
    }
  }
  return count[0] + count[1] + count[2];
}

/* Integer division that rounds down (towards minus infinity); C's "/"
 * rounds towards zero, which is wrong for negative numbers. */
static int64_t divide_rounding_down(int64_t numerator, int64_t denominator) {
  if(numerator >= 0) return numerator / denominator;
  return -((-numerator + denominator - 1) / denominator);
}

int32_t ft_repair_faulty_pixels(float* transmittance, uint32_t lines,
                                uint32_t pixels,
                                int32_t faulty[3][FT_FAULTY_PIXEL_MAX],
                                const uint32_t count[3], int32_t window_start,
                                uint32_t bin) {
  if(bin < 1 || pixels < 1) return -1;
  /* is_bad[x]: output pixel x of this channel is faulty */
  uint8_t* is_bad = malloc(pixels);
  if(is_bad == NULL) return -1;

  for(uint32_t channel = 0; channel < 3; channel++) {
    memset(is_bad, 0, pixels);
    for(uint32_t index = 0; index < count[channel]; index++) {
      int64_t from_first = faulty[channel][index] - (int64_t)FT_CCD_FIRST_PIXEL;
      int64_t output_pixel =
        divide_rounding_down(from_first, bin) - window_start;
      if(output_pixel >= 0 && output_pixel < pixels) is_bad[output_pixel] = 1;
    }

    for(int64_t pixel = 0; pixel < pixels; pixel++) {
      if(is_bad[pixel] == 0) continue;
      /* the nearest good pixels left and right; -1 or pixels: none */
      int64_t left = pixel - 1;
      int64_t right = pixel + 1;
      while(left >= 0 && is_bad[left] == 1) left--;
      while(right < pixels && is_bad[right] == 1) right++;
      bool has_left = left >= 0;
      bool has_right = right < pixels;
      if(has_left == false && has_right == false) continue;

      for(size_t line = 0; line < lines; line++) {
        float* row = transmittance + line * pixels * 3;
        float value;
        if(has_left == false) {
          value = row[right * 3 + channel];
        } else if(has_right == false) {
          value = row[left * 3 + channel];
        } else {
          float weight = (float)(pixel - left) / (right - left);
          value = (1 - weight) * row[left * 3 + channel] +
                  weight * row[right * 3 + channel];
        }
        row[pixel * 3 + channel] = value;
      }
    }
  }
  free(is_bad);
  return 0;
}

uint32_t ft_align_rgb_rows(float* transmittance, uint32_t lines,
                           uint32_t pixels, uint32_t row_offset) {
  size_t values_per_line = (size_t)pixels * 3;
  if(row_offset == 0 || lines <= 2 * row_offset) return lines;
  uint32_t aligned_lines = lines - 2 * row_offset;
  /* In place, line by line upwards: every line read is at or after the
   * line written, so nothing is overwritten before it is read. Blue
   * (index 2) stays where it is. */
  for(size_t line = 0; line < aligned_lines; line++) {
    float* out = transmittance + line * values_per_line;
    const float* red_source =
      transmittance + (line + 2 * row_offset) * values_per_line;
    const float* green_source =
      transmittance + (line + row_offset) * values_per_line;
    for(size_t pixel = 0; pixel < pixels; pixel++) {
      out[pixel * 3 + 0] = red_source[pixel * 3 + 0];
      out[pixel * 3 + 1] = green_source[pixel * 3 + 1];
    }
  }
  return aligned_lines;
}

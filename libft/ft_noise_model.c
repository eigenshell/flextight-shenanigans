/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_noise_model.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ft_calib.h"

/* the 0.25 DN^2 floor: at least the quantisation of a 1-DN step */
#define OFFSET_FLOOR_DN2 0.25

/* the line classes; NEITHER is the lamp's transition */
enum { CLASS_DARK = 0, CLASS_WHITE = 1, CLASS_NEITHER = -1 };

/* the mean of all values on one calibration line */
static double get_line_mean(const uint16_t* calibration, uint32_t line,
                            size_t values_per_line) {
  double sum = 0;
  for(size_t index = 0; index < values_per_line; index++) {
    sum += calibration[line * values_per_line + index];
  }
  return sum / values_per_line;
}

ft_noise_result ft_measure_noise_profile(const uint16_t* calibration,
                                         uint32_t lines, uint32_t pixels,
                                         double profile[6], ft_err* error) {
  /* each line's class, and its mean less its class's mean per channel */
  int8_t classes[FT_NOISE_MAX_LINES];
  double offsets[FT_NOISE_MAX_LINES][3];
  size_t values_per_line = (size_t)pixels * 3;
  bool was_clamped = false;

  if(lines < 1 || pixels < 1 || lines > FT_NOISE_MAX_LINES) {
    ft_fail(error, "noise profile: %u calibration lines", (unsigned)lines);
    return FT_NOISE_NONE;
  }
  /* the line classes, as ft_split_calibration */
  double darkest = 0;
  double brightest = 0;
  for(uint32_t line = 0; line < lines; line++) {
    double mean = get_line_mean(calibration, line, values_per_line);
    if(line == 0 || mean < darkest) darkest = mean;
    if(line == 0 || mean > brightest) brightest = mean;
  }
  double band = FT_CALIBRATION_BAND * (brightest - darkest);
  uint32_t dark_count = 0;
  uint32_t white_count = 0;
  for(uint32_t line = 0; line < lines; line++) {
    double mean = get_line_mean(calibration, line, values_per_line);
    if(mean < darkest + band) {
      classes[line] = CLASS_DARK;
      dark_count++;
    } else if(mean > brightest - band) {
      classes[line] = CLASS_WHITE;
      white_count++;
    } else {
      classes[line] = CLASS_NEITHER;
    }
  }
  if(dark_count < FT_NOISE_MIN_DARK || white_count < FT_NOISE_MIN_WHITE) {
    ft_fail(error, "noise profile: %u dark / %u white lines (need %d / %d)",
            (unsigned)dark_count, (unsigned)white_count, FT_NOISE_MIN_DARK,
            FT_NOISE_MIN_WHITE);
    return FT_NOISE_NONE;
  }

  /* each line's offset from its class mean, per channel */
  for(uint32_t channel = 0; channel < 3; channel++) {
    double class_means[2] = {0, 0};
    for(uint32_t line = 0; line < lines; line++) {
      offsets[line][channel] = 0;
      if(classes[line] == CLASS_NEITHER) continue;
      for(size_t index = channel; index < values_per_line; index += 3) {
        offsets[line][channel] += calibration[line * values_per_line + index];
      }
      offsets[line][channel] /= pixels;
      class_means[classes[line]] += offsets[line][channel];
    }
    class_means[CLASS_DARK] /= dark_count;
    class_means[CLASS_WHITE] /= white_count;
    for(uint32_t line = 0; line < lines; line++) {
      if(classes[line] != CLASS_NEITHER) {
        offsets[line][channel] -= class_means[classes[line]];
      }
    }
  }

  for(uint32_t channel = 0; channel < 3; channel++) {
    /* sums over the pixels: read noise squared, the white variance above
     * it, the signal span, 1 / span and 1 / span^2 */
    double read_noise2 = 0;
    double shot_variance = 0;
    double signal = 0;
    double inverse_span = 0;
    double inverse_span2 = 0;
    for(uint32_t pixel = 0; pixel < pixels; pixel++) {
      size_t at = (size_t)pixel * 3 + channel;
      double means[2] = {0, 0};
      double variances[2] = {0, 0};
      for(uint32_t line = 0; line < lines; line++) {
        if(classes[line] != CLASS_NEITHER) {
          means[classes[line]] += calibration[line * values_per_line + at];
        }
      }
      means[CLASS_DARK] /= dark_count;
      means[CLASS_WHITE] /= white_count;
      /* about each pixel's own mean (fixed pattern) and each line's own
       * offset (lamp drift): neither is pixel noise */
      for(uint32_t line = 0; line < lines; line++) {
        if(classes[line] == CLASS_NEITHER) continue;
        double deviation = calibration[line * values_per_line + at] -
                           means[classes[line]] -
                           offsets[line][channel];
        variances[classes[line]] += deviation * deviation;
      }
      variances[CLASS_DARK] /= dark_count - 1;
      variances[CLASS_WHITE] /= white_count - 1;
      double span = means[CLASS_WHITE] - means[CLASS_DARK];
      if(!(span >= 1)) {
        ft_fail(error,
                "noise profile: channel %u pixel %u has no white-dark span",
                (unsigned)channel, (unsigned)pixel);
        return FT_NOISE_NONE;
      }
      read_noise2 += variances[CLASS_DARK];
      shot_variance += variances[CLASS_WHITE] - variances[CLASS_DARK];
      signal += span;
      inverse_span += 1 / span;
      inverse_span2 += 1 / (span * span);
    }
    read_noise2 /= pixels;
    inverse_span /= pixels;
    inverse_span2 /= pixels;
    /* variance DN per DN of signal */
    double gain = shot_variance / signal;
    double slope = gain * inverse_span;
    double offset = read_noise2 * (1 + 1.0 / dark_count) * inverse_span2;
    double offset_floor = OFFSET_FLOOR_DN2 * inverse_span2;
    if(isfinite(slope) == false || slope < 0) {
      slope = 0;
      was_clamped = true;
    }
    if(isfinite(offset) == false || offset < offset_floor) {
      offset = offset_floor;
      was_clamped = true;
    }
    profile[2 * channel] = slope;
    profile[2 * channel + 1] = offset;
  }
  if(was_clamped) {
    ft_fail(error,
            "noise profile: clamped (shot or read noise not measurable)");
    return FT_NOISE_CLAMPED;
  }
  return FT_NOISE_OK;
}

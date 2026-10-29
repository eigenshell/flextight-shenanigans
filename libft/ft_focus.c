/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_focus.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* slack when comparing a scale with its limits */
#define SCALE_TOLERANCE 1e-6
/* the lowest ppi crop scans */
#define MIN_PPI 525.0
/* a position past this is nonsense, not a motor step */
#define POSITION_LIMIT_STEPS 2e9
/* the largest correction a focus file may give, tandem steps */
#define CORRECTION_LIMIT_STEPS 50000

/* The bins the scanner reads with, largest first. */
static const int32_t BINS_LARGEST_FIRST[] = {12, 6, 4, 3, 2, 1};
#define BIN_COUNT (sizeof BINS_LARGEST_FIRST / sizeof BINS_LARGEST_FIRST[0])

/* The curve: eight focus brackets 2026-10-12 (3807..6230). The
 * correction: the focus survey's tandem peaks (frame 6 centre) less its
 * 6300 peak, 0 from 3000 up (within the film's height there). */
const ft_lens ft_lens_default = {
  .film_steps = 1468601.0,
  .gap_offset_steps = 268105.0,
  .focal_length_steps = 317836.0,
  .pixel_pitch_inches = 3.4799e-4,
  .correction_count = 5,
  .correction_scale_ppi = {1200, 1500, 2000, 3000, 6300},
  .correction_offset_steps = {-11278, -4778, -1358, 0, 0},
};

static bool is_scale_in_range(double scale_ppi) {
  return scale_ppi >= FT_SCALE_MIN_PPI - SCALE_TOLERANCE &&
         scale_ppi <= FT_SCALE_MAX_PPI + SCALE_TOLERANCE;
}

double ft_get_lens_correction(const ft_lens* lens, double scale_ppi) {
  uint32_t count = lens->correction_count;
  const double* scales = lens->correction_scale_ppi;
  const double* offsets = lens->correction_offset_steps;
  if(count < 1 || count > FT_LENS_CORRECTION_MAX) return 0;
  /* held flat before the first point */
  if(scale_ppi <= scales[0]) return offsets[0];
  for(uint32_t index = 1; index < count; index++) {
    if(scale_ppi <= scales[index]) {
      /* linear between the point below and this one */
      double below = scales[index - 1];
      double above = scales[index];
      return offsets[index - 1] + (scale_ppi - below) /
                                    (above - below) *
                                    (offsets[index] - offsets[index - 1]);
    }
  }
  /* held flat past the last point */
  return offsets[count - 1];
}

double ft_estimate_depth_of_field95(const ft_lens* lens, double scale_ppi,
                                    int32_t bin) {
  double magnification = lens->pixel_pitch_inches * scale_ppi;
  int32_t bin_at_least_1 = bin > 0 ? bin : 1;
  return FT_DEPTH_OF_FIELD95_K *
         (1 + magnification) /
         (magnification * magnification) *
         bin_at_least_1;
}

int32_t ft_get_bin_for_ppi(double ppi) {
  for(size_t index = 0; index < BIN_COUNT; index++) {
    int32_t bin = BINS_LARGEST_FIRST[index];
    if(ppi * bin <= FT_SCALE_MAX_PPI + SCALE_TOLERANCE) return bin;
  }
  return -1;
}

bool ft_is_ppi_supported(double ppi) {
  int32_t bin = ft_get_bin_for_ppi(ppi);
  if(ppi < MIN_PPI || bin < 0) return false;
  return is_scale_in_range(ppi * bin);
}

/* the lens values the curve needs are finite and positive */
static bool is_lens_plausible(const ft_lens* lens) {
  return isfinite(lens->pixel_pitch_inches) &&
         lens->pixel_pitch_inches > 0 &&
         isfinite(lens->focal_length_steps) &&
         lens->focal_length_steps > 0 &&
         isfinite(lens->film_steps) &&
         lens->film_steps > 0 &&
         isfinite(lens->gap_offset_steps);
}

static bool is_position_valid(double position_steps) {
  return isfinite(position_steps) &&
         position_steps >= 0 &&
         position_steps <= POSITION_LIMIT_STEPS;
}

int32_t ft_find_optics_positions(const ft_lens* lens, double scale_ppi,
                                 int64_t* ccd_steps, int64_t* lens_steps) {
  /* written as "not in range" so that NaN is refused too */
  if(is_scale_in_range(scale_ppi) == false) return -1;
  if(is_lens_plausible(lens) == false) return -1;

  double magnification = lens->pixel_pitch_inches * scale_ppi;
  double object_distance = lens->focal_length_steps * (1 + 1 / magnification);
  double image_distance = lens->focal_length_steps * (1 + magnification);
  double lens_position = lens->film_steps -
                         object_distance +
                         ft_get_lens_correction(lens, scale_ppi);
  double ccd_position = lens_position + lens->gap_offset_steps - image_distance;
  if(is_position_valid(lens_position) == false) return -1;
  if(is_position_valid(ccd_position) == false) return -1;

  /* round to the nearest step; both are >= 0 here */
  *lens_steps = (int64_t)(lens_position + 0.5);
  *ccd_steps = (int64_t)(ccd_position + 0.5);
  return 0;
}

int32_t ft_find_focused_optics_positions(const ft_lens* lens, double scale_ppi,
                                         int64_t offset_steps,
                                         int64_t* ccd_steps,
                                         int64_t* lens_steps) {
  int64_t curve_ccd_steps;
  int64_t curve_lens_steps;
  if(ft_find_optics_positions(lens, scale_ppi, &curve_ccd_steps,
                              &curve_lens_steps) != 0) {
    return -1;
  }
  *ccd_steps = curve_ccd_steps + offset_steps;
  *lens_steps = curve_lens_steps + offset_steps;
  return 0;
}

bool ft_are_positions_safe(int64_t ccd_steps, int64_t lens_steps) {
  return ccd_steps >= FT_CCD_MIN_STEPS &&
         ccd_steps <= FT_CCD_MAX_STEPS &&
         lens_steps >= FT_LENS_MIN_STEPS &&
         lens_steps <= FT_LENS_MAX_STEPS &&
         lens_steps - ccd_steps >= FT_GAP_MIN_STEPS;
}

/* Tenengrad on a 3x3-box-denoised image, over its mean: Sobel x and y
 * on the smoothed pixels with full box support, rms magnitude / 8 (a
 * ramp's per-pixel step) / mean. Dividing by the window mean, not by
 * each pixel (log gradient), keeps dense film from inflating its noise:
 * on a dense slide the log gradient was flat across a whole focus sweep
 * (hardware 2026-10-13). */
double ft_measure_sharpness(const float* image, uint32_t width,
                            uint32_t height) {
  if(width < 5 || height < 5) return 0;
  /* the box filter needs a pixel on every side, so it loses the border */
  size_t smooth_width = width - 2;
  size_t smooth_height = height - 2;
  float* smooth = malloc(smooth_width * smooth_height * sizeof *smooth);
  if(smooth == NULL) return NAN;

  /* smooth[y][x] = the 3x3 mean around image[y + 1][x + 1] */
  double sum = 0;
  for(size_t y = 0; y < smooth_height; y++) {
    for(size_t x = 0; x < smooth_width; x++) {
      double box_sum = 0;
      for(size_t row = 0; row < 3; row++) {
        for(size_t column = 0; column < 3; column++) {
          box_sum += image[(y + row) * width + x + column];
        }
      }
      smooth[y * smooth_width + x] = (float)(box_sum / 9);
      sum += box_sum / 9;
    }
  }
  double mean = sum / ((double)smooth_width * smooth_height);

  /* Sobel on the smoothed image, again skipping its border */
  double gradient_sum = 0;
  int64_t gradient_count = 0;
  for(size_t y = 1; y < smooth_height - 1; y++) {
    for(size_t x = 1; x < smooth_width - 1; x++) {
      /* the rows above, at and below (y, x); [-1] and [1] are the
       * pixels left and right of x */
      const float* up = smooth + (y - 1) * smooth_width + x;
      const float* middle = up + smooth_width;
      const float* down = middle + smooth_width;
      double gradient_x = (up[1] + 2.0 * middle[1] + down[1]) -
                          (up[-1] + 2.0 * middle[-1] + down[-1]);
      double gradient_y =
        (down[-1] + 2.0 * down[0] + down[1]) - (up[-1] + 2.0 * up[0] + up[1]);
      gradient_sum += gradient_x * gradient_x + gradient_y * gradient_y;
      gradient_count++;
    }
  }
  free(smooth);
  if(mean > 0) return sqrt(gradient_sum / gradient_count) / (8 * mean);
  return 0;
}

ft_peak_result ft_find_peak(const int64_t* x, const double* y, uint32_t count,
                            double* peak) {
  if(count < 3) return FT_PEAK_NONE;
  for(uint32_t index = 0; index < count; index++) {
    if(isfinite(y[index]) == false) return FT_PEAK_NONE;
  }
  uint32_t top = 0;
  for(uint32_t index = 1; index < count; index++) {
    if(y[index] > y[top]) top = index;
  }
  if(top == 0 || top == count - 1) {
    *peak = (double)x[top];
    return FT_PEAK_AT_END;
  }

  /* the parabola through the top and its two neighbours */
  double x0 = (double)x[top - 1];
  double x1 = (double)x[top];
  double x2 = (double)x[top + 1];
  double y0 = y[top - 1];
  double y1 = y[top];
  double y2 = y[top + 1];
  double numerator =
    (x1 - x0) * (x1 - x0) * (y1 - y2) - (x1 - x2) * (x1 - x2) * (y1 - y0);
  double denominator = (x1 - x0) * (y1 - y2) - (x1 - x2) * (y1 - y0);
  *peak = denominator != 0 ? x1 - 0.5 * numerator / denominator : x1;
  return FT_PEAK_FOUND;
}

ft_focus_mode ft_decide_focus_mode(const int64_t offsets[3], int64_t* shift) {
  double mean = (offsets[0] + offsets[1] + offsets[2]) / 3.0;
  /* round half away from zero */
  *shift = (int64_t)(mean < 0 ? mean - 0.5 : mean + 0.5);
  for(uint32_t index = 0; index < 3; index++) {
    if(fabs(offsets[index] - mean) > FT_FOCUS_MARGIN_STEPS) {
      return FT_FOCUS_PER_FRAME;
    }
  }
  return FT_FOCUS_SHIFTED;
}

/* The fit works on parameters of similar size, so the solver stays
 * accurate: film, gap offset and focal length in millions of steps,
 * pixel pitch x 1e4. */
enum { FIT_FILM, FIT_GAP_OFFSET, FIT_FOCAL_LENGTH, FIT_PIXEL_PITCH };
#define FIT_PARAMETER_COUNT 4
#define STEPS_SCALE 1e6
#define PITCH_SCALE 1e4

/* The curve's CCD and lens positions at scale_ppi for fit parameters. */
static void predict_positions(const double parameters[FIT_PARAMETER_COUNT],
                              double scale_ppi, double* ccd_position,
                              double* lens_position) {
  double magnification = parameters[FIT_PIXEL_PITCH] / PITCH_SCALE * scale_ppi;
  double focal_length = parameters[FIT_FOCAL_LENGTH] * STEPS_SCALE;
  double object_distance = focal_length * (1 + 1 / magnification);
  double image_distance = focal_length * (1 + magnification);
  double lens = parameters[FIT_FILM] * STEPS_SCALE - object_distance;
  *lens_position = lens;
  *ccd_position =
    lens + parameters[FIT_GAP_OFFSET] * STEPS_SCALE - image_distance;
}

/* the sum of squared CCD and lens residuals over all points */
static double compute_fit_cost(const double parameters[FIT_PARAMETER_COUNT],
                               const double* scale_ppi,
                               const int64_t* ccd_steps,
                               const int64_t* lens_steps, uint32_t count) {
  double cost = 0;
  for(uint32_t index = 0; index < count; index++) {
    double ccd_position;
    double lens_position;
    predict_positions(parameters, scale_ppi[index], &ccd_position,
                      &lens_position);
    double ccd_residual = ccd_position - ccd_steps[index];
    double lens_residual = lens_position - lens_steps[index];
    cost += ccd_residual * ccd_residual + lens_residual * lens_residual;
  }
  return cost;
}

/* Solve matrix x = vector for a 4x4 matrix by Gaussian elimination with
 * partial pivoting. matrix and vector are overwritten. Returns false
 * when the matrix is singular. */
static bool solve_4x4(double matrix[4][4], double vector[4],
                      double solution[4]) {
  for(uint32_t column = 0; column < 4; column++) {
    /* the pivot: the row with the largest value in this column */
    uint32_t pivot = column;
    for(uint32_t row = column + 1; row < 4; row++) {
      if(fabs(matrix[row][column]) > fabs(matrix[pivot][column])) pivot = row;
    }
    if(fabs(matrix[pivot][column]) < 1e-300) return false;

    /* swap the pivot row into place */
    for(uint32_t index = 0; index < 4; index++) {
      double swap = matrix[column][index];
      matrix[column][index] = matrix[pivot][index];
      matrix[pivot][index] = swap;
    }
    double swap = vector[column];
    vector[column] = vector[pivot];
    vector[pivot] = swap;

    /* clear this column below the pivot */
    for(uint32_t row = column + 1; row < 4; row++) {
      double factor = matrix[row][column] / matrix[column][column];
      for(uint32_t index = column; index < 4; index++) {
        matrix[row][index] -= factor * matrix[column][index];
      }
      vector[row] -= factor * vector[column];
    }
  }

  /* back substitution, from the last row up */
  for(uint32_t step = 0; step < 4; step++) {
    uint32_t row = 3 - step;
    double sum = vector[row];
    for(uint32_t index = row + 1; index < 4; index++) {
      sum -= matrix[row][index] * solution[index];
    }
    solution[row] = sum / matrix[row][row];
  }
  return true;
}

int32_t ft_fit_lens(const double* scale_ppi, const int64_t* ccd_steps,
                    const int64_t* lens_steps, uint32_t count, ft_lens* lens,
                    double* rms_ccd_steps, double* rms_lens_steps) {
  /* the most Levenberg-Marquardt iterations */
  const uint32_t MAX_ITERATIONS = 200;
  /* the step used for the numeric derivatives */
  const double DERIVATIVE_STEP = 1e-6;

  if(count < 4) return -1;
  for(uint32_t index = 0; index < count; index++) {
    if(isfinite(scale_ppi[index]) == false || !(scale_ppi[index] > 0)) {
      return -1;
    }
  }
  bool is_start_valid = isfinite(lens->film_steps) &&
                        isfinite(lens->gap_offset_steps) &&
                        isfinite(lens->focal_length_steps) &&
                        isfinite(lens->pixel_pitch_inches) &&
                        lens->focal_length_steps > 0 &&
                        lens->pixel_pitch_inches > 0;
  if(is_start_valid == false) return -1;

  double parameters[FIT_PARAMETER_COUNT];
  parameters[FIT_FILM] = lens->film_steps / STEPS_SCALE;
  parameters[FIT_GAP_OFFSET] = lens->gap_offset_steps / STEPS_SCALE;
  parameters[FIT_FOCAL_LENGTH] = lens->focal_length_steps / STEPS_SCALE;
  parameters[FIT_PIXEL_PITCH] = lens->pixel_pitch_inches * PITCH_SCALE;
  double cost =
    compute_fit_cost(parameters, scale_ppi, ccd_steps, lens_steps, count);
  if(isfinite(cost) == false) return -1;

  /* Levenberg-Marquardt: damping blends Gauss-Newton (small) with
   * gradient descent (large); it shrinks after a good step and grows
   * after a bad one */
  double damping = 1e-3;
  bool has_stepped = false;
  for(uint32_t iteration = 0; iteration < MAX_ITERATIONS; iteration++) {
    /* the normal equations J^T J step = J^T r */
    double jacobian_square[4][4] = {{0}};
    double jacobian_residual[4] = {0};
    for(uint32_t index = 0; index < count; index++) {
      double ccd_position;
      double lens_position;
      predict_positions(parameters, scale_ppi[index], &ccd_position,
                        &lens_position);
      double residuals[2] = {ccd_position - ccd_steps[index],
                             lens_position - lens_steps[index]};
      /* jacobian[r][k]: how residual r changes with parameter k */
      double jacobian[2][4];
      for(uint32_t parameter = 0; parameter < 4; parameter++) {
        double moved[FIT_PARAMETER_COUNT];
        memcpy(moved, parameters, sizeof moved);
        moved[parameter] += DERIVATIVE_STEP;
        double moved_ccd;
        double moved_lens;
        predict_positions(moved, scale_ppi[index], &moved_ccd, &moved_lens);
        jacobian[0][parameter] = (moved_ccd - ccd_position) / DERIVATIVE_STEP;
        jacobian[1][parameter] = (moved_lens - lens_position) / DERIVATIVE_STEP;
      }
      for(uint32_t residual = 0; residual < 2; residual++) {
        for(uint32_t row = 0; row < 4; row++) {
          jacobian_residual[row] +=
            jacobian[residual][row] * residuals[residual];
          for(uint32_t column = 0; column < 4; column++) {
            jacobian_square[row][column] +=
              jacobian[residual][row] * jacobian[residual][column];
          }
        }
      }
    }
    for(uint32_t row = 0; row < 4; row++) {
      jacobian_square[row][row] *= 1 + damping;
    }

    double step[4];
    if(solve_4x4(jacobian_square, jacobian_residual, step) == false) {
      if(has_stepped == false) return -1;
      break;
    }
    double candidate[FIT_PARAMETER_COUNT];
    for(uint32_t parameter = 0; parameter < 4; parameter++) {
      candidate[parameter] = parameters[parameter] - step[parameter];
    }
    double candidate_cost =
      compute_fit_cost(candidate, scale_ppi, ccd_steps, lens_steps, count);
    if(candidate_cost < cost) {
      bool is_converged = cost - candidate_cost < 1e-12 * cost;
      memcpy(parameters, candidate, sizeof parameters);
      cost = candidate_cost;
      has_stepped = true;
      damping *= 0.3;
      if(is_converged) break;
    } else {
      damping *= 10;
      if(damping > 1e10) break;
    }
  }

  /* the fit has no correction table: every other field stays zero */
  ft_lens fitted = {0};
  fitted.film_steps = parameters[FIT_FILM] * STEPS_SCALE;
  fitted.gap_offset_steps = parameters[FIT_GAP_OFFSET] * STEPS_SCALE;
  fitted.focal_length_steps = parameters[FIT_FOCAL_LENGTH] * STEPS_SCALE;
  fitted.pixel_pitch_inches = parameters[FIT_PIXEL_PITCH] / PITCH_SCALE;
  bool is_fit_valid = isfinite(cost) &&
                      isfinite(fitted.film_steps) &&
                      isfinite(fitted.gap_offset_steps) &&
                      isfinite(fitted.focal_length_steps) &&
                      isfinite(fitted.pixel_pitch_inches) &&
                      fitted.focal_length_steps > 0 &&
                      fitted.pixel_pitch_inches > 0 &&
                      fitted.film_steps > fitted.gap_offset_steps;
  if(is_fit_valid == false) return -1;

  double ccd_square_sum = 0;
  double lens_square_sum = 0;
  for(uint32_t index = 0; index < count; index++) {
    double ccd_position;
    double lens_position;
    predict_positions(parameters, scale_ppi[index], &ccd_position,
                      &lens_position);
    ccd_square_sum +=
      (ccd_position - ccd_steps[index]) * (ccd_position - ccd_steps[index]);
    lens_square_sum +=
      (lens_position - lens_steps[index]) * (lens_position - lens_steps[index]);
  }
  *lens = fitted;
  *rms_ccd_steps = sqrt(ccd_square_sum / count);
  *rms_lens_steps = sqrt(lens_square_sum / count);
  return 0;
}

/* Parse text a line at a time, as fgets(line, 160) would see it: a line
 * of 159 or more characters (without its '\n') is "too long" unless it
 * is a comment. */
int32_t ft_parse_lens(ft_lens* lens, const char* text, size_t text_length,
                      ft_err* error) {
  /* bits for the keys seen, so a missing one is noticed */
  enum {
    SEEN_FILM = 1,
    SEEN_GAP_OFFSET = 2,
    SEEN_FOCAL_LENGTH = 4,
    SEEN_PIXEL_PITCH = 8,
    SEEN_ALL = 15
  };
  ft_lens parsed = {0};
  uint32_t seen = 0;
  uint32_t line_number = 0;
  size_t position = 0;

  while(position < text_length) {
    char line[160];
    const char* line_start = text + position;
    const char* newline = memchr(line_start, '\n', text_length - position);
    size_t line_length =
      newline != NULL ? (size_t)(newline - line_start) : text_length - position;
    position += line_length + (newline != NULL ? 1 : 0);
    line_number++;

    /* copy what fits, so the checks below see a C string */
    bool is_too_long = line_length >= sizeof line - 1;
    size_t copy_length = is_too_long ? sizeof line - 1 : line_length;
    memcpy(line, line_start, copy_length);
    line[copy_length] = '\0';

    const char* cursor = line;
    while(*cursor == ' ' || *cursor == '\t') cursor++;
    if(*cursor == '#' || *cursor == '\0' || *cursor == '\r') continue;
    if(is_too_long) {
      return ft_fail(error, "focus file line %u: too long",
                     (unsigned)line_number);
    }

    /* %n stores how many characters sscanf used, to check nothing
     * follows the numbers */
    int used = 0;
    bool is_correction = strncmp(cursor, "correct", 7) == 0 &&
                         (cursor[7] == ' ' || cursor[7] == '=');
    if(is_correction) {
      double scale_ppi;
      double offset_steps;
      if(sscanf(cursor, "correct = %lf %lf %n", &scale_ppi, &offset_steps,
                &used) != 2 ||
         cursor[used] != '\0') {
        return ft_fail(error,
                       "focus file line %u: expected correct = scale offset",
                       (unsigned)line_number);
      }
      if(isfinite(scale_ppi) == false ||
         isfinite(offset_steps) == false ||
         !(scale_ppi > 0) ||
         fabs(offset_steps) > CORRECTION_LIMIT_STEPS) {
        return ft_fail(error, "focus file line %u: correction out of range",
                       (unsigned)line_number);
      }
      uint32_t count = parsed.correction_count;
      if(count == FT_LENS_CORRECTION_MAX) {
        return ft_fail(error, "focus file line %u: more than %d corrections",
                       (unsigned)line_number, FT_LENS_CORRECTION_MAX);
      }
      if(count > 0 && !(scale_ppi > parsed.correction_scale_ppi[count - 1])) {
        return ft_fail(error,
                       "focus file line %u: correction scales must ascend",
                       (unsigned)line_number);
      }
      parsed.correction_scale_ppi[count] = scale_ppi;
      parsed.correction_offset_steps[count] = offset_steps;
      parsed.correction_count = count + 1;
      continue;
    }

    char key[16];
    double value;
    if(sscanf(cursor, "%15[a-z] = %lf %n", key, &value, &used) != 2 ||
       cursor[used] != '\0') {
      return ft_fail(error, "focus file line %u: expected key = number",
                     (unsigned)line_number);
    }
    if(isfinite(value) == false) {
      return ft_fail(error, "focus file line %u: value not finite",
                     (unsigned)line_number);
    }
    /* the keys are the file format: keep their short names */
    if(strcmp(key, "film") == 0) {
      parsed.film_steps = value;
      seen |= SEEN_FILM;
    } else if(strcmp(key, "w") == 0) {
      parsed.gap_offset_steps = value;
      seen |= SEEN_GAP_OFFSET;
    } else if(strcmp(key, "fl") == 0) {
      parsed.focal_length_steps = value;
      seen |= SEEN_FOCAL_LENGTH;
    } else if(strcmp(key, "p") == 0) {
      parsed.pixel_pitch_inches = value;
      seen |= SEEN_PIXEL_PITCH;
    } else {
      return ft_fail(error, "focus file line %u: unknown key %s",
                     (unsigned)line_number, key);
    }
  }
  if(seen != SEEN_ALL) {
    return ft_fail(error, "focus file: needs film, w, fl and p");
  }
  if(!(parsed.focal_length_steps > 0) ||
     !(parsed.pixel_pitch_inches > 0) ||
     isfinite(parsed.film_steps) == false) {
    return ft_fail(error, "focus file: fl and p must be positive");
  }
  *lens = parsed;
  return 0;
}

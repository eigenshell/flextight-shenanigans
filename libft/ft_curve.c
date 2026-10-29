/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_curve.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* A named curve. */
typedef struct preset {
  const char* name;
  int32_t point_count;
  float x[5];
  float y[5];
} preset;

static const preset PRESETS[] = {
  {.name = "linear", .point_count = 2, .x = {0, 1}, .y = {0, 1}},
  {.name = "soft",
   .point_count = 4,
   .x = {0, 0.25f, 0.75f, 1},
   .y = {0, 0.28f, 0.72f, 1}},
  {.name = "medium",
   .point_count = 4,
   .x = {0, 0.25f, 0.75f, 1},
   .y = {0, 0.21f, 0.79f, 1}},
  {.name = "strong",
   .point_count = 4,
   .x = {0, 0.25f, 0.75f, 1},
   .y = {0, 0.15f, 0.85f, 1}},
  {.name = "fade",
   .point_count = 3,
   .x = {0, 0.25f, 1},
   .y = {0.06f, 0.27f, 1}},
};
#define PRESET_COUNT (sizeof PRESETS / sizeof PRESETS[0])

void ft_init_identity_curve(ft_curve* curve) {
  memset(curve, 0, sizeof *curve);
  curve->point_count = 2;
  curve->x[1] = 1;
  curve->y[1] = 1;
}

int32_t ft_validate_curve(const ft_curve* curve, ft_err* error) {
  if(curve->point_count < 2 || curve->point_count > FT_CURVE_MAX) {
    return ft_fail(error, "curve: %d points (2..%d)", (int)curve->point_count,
                   FT_CURVE_MAX);
  }
  for(int32_t index = 0; index < curve->point_count; index++) {
    float x = curve->x[index];
    float y = curve->y[index];
    /* written as "not inside" so that NaN is refused too */
    if(!(x >= 0 && x <= 1 && y >= 0 && y <= 1)) {
      return ft_fail(error, "curve: point %d (%g,%g) outside [0,1]",
                     (int)index + 1, x, y);
    }
    if(index > 0 && !(x > curve->x[index - 1])) {
      return ft_fail(error, "curve: x must increase (point %d)",
                     (int)index + 1);
    }
  }
  return 0;
}

/* Fritsch-Carlson tangents: the mean of the neighbouring slopes, 0 at a
 * peak or valley, then limited so no segment overshoots. */
static void compute_tangents(const ft_curve* curve, double* tangents) {
  double slopes[FT_CURVE_MAX] = {0};
  int32_t count = curve->point_count;
  for(int32_t index = 0; index < count - 1; index++) {
    slopes[index] = ((double)curve->y[index + 1] - curve->y[index]) /
                    ((double)curve->x[index + 1] - curve->x[index]);
  }
  tangents[0] = slopes[0];
  tangents[count - 1] = slopes[count - 2];
  for(int32_t index = 1; index < count - 1; index++) {
    if(slopes[index - 1] * slopes[index] <= 0) {
      tangents[index] = 0;
    } else {
      tangents[index] = (slopes[index - 1] + slopes[index]) / 2;
    }
  }
  for(int32_t index = 0; index < count - 1; index++) {
    if(slopes[index] == 0) {
      tangents[index] = 0;
      tangents[index + 1] = 0;
      continue;
    }
    double alpha = tangents[index] / slopes[index];
    double beta = tangents[index + 1] / slopes[index];
    double square_sum = alpha * alpha + beta * beta;
    /* outside the circle of radius 3: scale back onto it */
    if(square_sum > 9) {
      double tau = 3 / sqrt(square_sum);
      tangents[index] = tau * alpha * slopes[index];
      tangents[index + 1] = tau * beta * slopes[index];
    }
  }
}

double ft_evaluate_curve(const ft_curve* curve, double x) {
  int32_t last = curve->point_count - 1;
  if(!(x > curve->x[0])) return curve->y[0];
  if(x >= curve->x[last]) return curve->y[last];
  double tangents[FT_CURVE_MAX];
  compute_tangents(curve, tangents);
  /* the segment x lies in */
  int32_t segment = 0;
  while(x > curve->x[segment + 1]) segment++;
  double width = (double)curve->x[segment + 1] - curve->x[segment];
  double t = (x - curve->x[segment]) / width;
  double t2 = t * t;
  double t3 = t2 * t;
  /* Hermite, written so a flat segment (equal y, zero tangents) is
   * exactly flat */
  double value =
    curve->y[segment] +
    ((double)curve->y[segment + 1] - curve->y[segment]) * (3 * t2 - 2 * t3) +
    width * (tangents[segment] * (t3 - 2 * t2 + t) +
             tangents[segment + 1] * (t3 - t2));
  if(value < 0) return 0;
  if(value > 1) return 1;
  return value;
}

int32_t ft_find_curve_preset(const char* name, ft_curve* curve) {
  for(size_t index = 0; index < PRESET_COUNT; index++) {
    const preset* found = &PRESETS[index];
    if(strcmp(found->name, name) != 0) continue;
    memset(curve, 0, sizeof *curve);
    curve->point_count = found->point_count;
    for(int32_t point = 0; point < curve->point_count; point++) {
      curve->x[point] = found->x[point];
      curve->y[point] = found->y[point];
    }
    return 0;
  }
  return -1;
}

const char* ft_get_curve_preset_name(uint32_t index) {
  return index < PRESET_COUNT ? PRESETS[index].name : NULL;
}

int32_t ft_parse_curve(const char* text, ft_curve* curve, ft_err* error) {
  memset(curve, 0, sizeof *curve);
  const char* cursor = text;
  while(true) {
    while(*cursor == ' ') cursor++;
    if(*cursor == '\0') break;
    int point = (int)curve->point_count + 1;
    if(curve->point_count == FT_CURVE_MAX) {
      return ft_fail(error, "curve: more than %d points", FT_CURVE_MAX);
    }
    char* end;
    double x = strtod(cursor, &end);
    if(end == cursor || *end != ',') {
      return ft_fail(error, "curve: point %d: expected x,y", point);
    }
    cursor = end + 1;
    double y = strtod(cursor, &end);
    if(end == cursor || (*end != '\0' && *end != ' ')) {
      return ft_fail(error, "curve: point %d: expected x,y", point);
    }
    cursor = end;
    if(!(x >= 0 && x <= 1 && y >= 0 && y <= 1)) {
      return ft_fail(error, "curve: point %d (%g,%g) outside [0,1]", point, x,
                     y);
    }
    curve->x[curve->point_count] = (float)x;
    curve->y[curve->point_count] = (float)y;
    curve->point_count++;
  }
  return ft_validate_curve(curve, error);
}

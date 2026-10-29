/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Tone curves: a monotone cubic (Fritsch-Carlson) through 2..16 control
 * points in [0,1] x [0,1]. Monotone points give a monotone curve that
 * never overshoots them. Pure C99 (no I/O, no allocation): it runs in the
 * browser too (ft_render.h). */
#ifndef FT_CURVE_H
#define FT_CURVE_H

#include <stdint.h>

#include "ft.h"

/* the most control points a curve has */
#define FT_CURVE_MAX 16

/* A tone curve: its control points. */
typedef struct ft_curve {
  /* points in x and y, 2..FT_CURVE_MAX */
  int32_t point_count;
  /* x strictly increasing, all in [0,1] */
  float x[FT_CURVE_MAX];
  float y[FT_CURVE_MAX];
} ft_curve;

/* Make the straight line (0,0)-(1,1). */
void ft_init_identity_curve(ft_curve* curve);

/* Check 2..FT_CURVE_MAX points, x strictly increasing, all in [0,1].
 * Returns 0, or -1 with error filled. */
int32_t ft_validate_curve(const ft_curve* curve, ft_err* error);

/* The curve at x (clamped to the end points); the curve must be valid. */
double ft_evaluate_curve(const ft_curve* curve, double x);

/* Load a preset: "linear", "soft", "medium", "strong" or "fade".
 * Returns 0, or -1 when the name is unknown. */
int32_t ft_find_curve_preset(const char* name, ft_curve* curve);

/* The index-th preset's name; NULL past the last. */
const char* ft_get_curve_preset_name(uint32_t index);

/* Read "x,y x,y ..." (spaces between points) and validate it; numbers
 * are read with strtod in the C locale. Returns 0, or -1 with error
 * filled. */
int32_t ft_parse_curve(const char* text, ft_curve* curve, ft_err* error);

#endif

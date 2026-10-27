/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Tone curves: a monotone cubic (Fritsch-Carlson) through 2..16 control
 * points in [0,1] x [0,1]. Monotone points give a monotone curve that
 * never overshoots them. Pure C99 (no I/O, no allocation): it runs in the
 * browser too (ft_render.h). */
#ifndef FT_CURVE_H
#define FT_CURVE_H
#include "ft.h"

#define FT_CURVE_MAX 16

typedef struct {
  int n;
  float x[FT_CURVE_MAX], y[FT_CURVE_MAX];
} ft_curve;

/* the straight line (0,0)-(1,1) */
void ft_curve_identity(ft_curve* c);
/* 2..FT_CURVE_MAX points, x strictly increasing, all in [0,1] */
int ft_curve_validate(const ft_curve* c, ft_err* e);
/* c at x (clamped to the end points); c must be valid */
double ft_curve_eval(const ft_curve* c, double x);
/* "linear", "soft", "medium", "strong", "fade"; -1 if unknown */
int ft_curve_preset(const char* name, ft_curve* c);
/* the i-th preset's name, NULL past the last */
const char* ft_curve_preset_name(int i);
/* "x,y x,y ..." (spaces between points), validated; numbers are read with
 * strtod in the C locale */
int ft_curve_parse(const char* s, ft_curve* c, ft_err* e);

#endif

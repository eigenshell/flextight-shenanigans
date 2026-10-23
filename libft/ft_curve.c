/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_curve.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *name;
  int n;
  float x[5], y[5];
} preset;

static const preset PRESETS[] = {
    {"linear", 2, {0, 1}, {0, 1}},
    {"soft", 4, {0, 0.25f, 0.75f, 1}, {0, 0.28f, 0.72f, 1}},
    {"medium", 4, {0, 0.25f, 0.75f, 1}, {0, 0.21f, 0.79f, 1}},
    {"strong", 4, {0, 0.25f, 0.75f, 1}, {0, 0.15f, 0.85f, 1}},
    {"fade", 3, {0, 0.25f, 1}, {0.06f, 0.27f, 1}},
};
#define N_PRESETS (int)(sizeof PRESETS / sizeof PRESETS[0])

void ft_curve_identity(ft_curve *c) {
  memset(c, 0, sizeof *c);
  c->n = 2;
  c->x[1] = c->y[1] = 1;
}

int ft_curve_validate(const ft_curve *c, ft_err *e) {
  int i;
  if(c->n < 2 || c->n > FT_CURVE_MAX)
    return ft_fail(e, "curve: %d points (2..%d)", c->n, FT_CURVE_MAX);
  for(i = 0; i < c->n; i++) {
    if(!(c->x[i] >= 0 && c->x[i] <= 1 && c->y[i] >= 0 && c->y[i] <= 1))
      return ft_fail(
          e, "curve: point %d (%g,%g) outside [0,1]", i + 1, c->x[i], c->y[i]
      );
    if(i > 0 && !(c->x[i] > c->x[i - 1]))
      return ft_fail(e, "curve: x must increase (point %d)", i + 1);
  }
  return 0;
}

/* Fritsch-Carlson tangents */
static void tangents(const ft_curve *c, double *m) {
  double d[FT_CURVE_MAX] = {0};
  int n = c->n, i;
  for(i = 0; i < n - 1; i++)
    d[i] = ((double)c->y[i + 1] - c->y[i]) / ((double)c->x[i + 1] - c->x[i]);
  m[0] = d[0];
  m[n - 1] = d[n - 2];
  for(i = 1; i < n - 1; i++)
    m[i] = d[i - 1] * d[i] <= 0 ? 0 : (d[i - 1] + d[i]) / 2;
  for(i = 0; i < n - 1; i++) {
    double a, b, s;
    if(d[i] == 0) {
      m[i] = m[i + 1] = 0;
      continue;
    }
    a = m[i] / d[i];
    b = m[i + 1] / d[i];
    s = a * a + b * b;
    if(s > 9) {
      double tau = 3 / sqrt(s);
      m[i] = tau * a * d[i];
      m[i + 1] = tau * b * d[i];
    }
  }
}

double ft_curve_eval(const ft_curve *c, double x) {
  double m[FT_CURVE_MAX], h, t, t2, t3, v;
  int k = 0;
  if(!(x > c->x[0]))
    return c->y[0];
  if(x >= c->x[c->n - 1])
    return c->y[c->n - 1];
  tangents(c, m);
  while(x > c->x[k + 1])
    k++;
  h = (double)c->x[k + 1] - c->x[k];
  t = (x - c->x[k]) / h;
  t2 = t * t;
  t3 = t2 * t;
  /* Hermite, written so a flat segment (equal y, zero tangents) is
   * exactly flat */
  v = c->y[k] + ((double)c->y[k + 1] - c->y[k]) * (3 * t2 - 2 * t3) +
      h * (m[k] * (t3 - 2 * t2 + t) + m[k + 1] * (t3 - t2));
  return v < 0 ? 0 : v > 1 ? 1 : v;
}

int ft_curve_preset(const char *name, ft_curve *c) {
  int i, k;
  for(i = 0; i < N_PRESETS; i++)
    if(strcmp(PRESETS[i].name, name) == 0) {
      memset(c, 0, sizeof *c);
      c->n = PRESETS[i].n;
      for(k = 0; k < c->n; k++) {
        c->x[k] = PRESETS[i].x[k];
        c->y[k] = PRESETS[i].y[k];
      }
      return 0;
    }
  return -1;
}

const char *ft_curve_preset_name(int i) {
  return i >= 0 && i < N_PRESETS ? PRESETS[i].name : NULL;
}

int ft_curve_parse(const char *s, ft_curve *c, ft_err *e) {
  memset(c, 0, sizeof *c);
  for(;;) {
    char *end;
    double x, y;
    while(*s == ' ')
      s++;
    if(!*s)
      break;
    if(c->n == FT_CURVE_MAX)
      return ft_fail(e, "curve: more than %d points", FT_CURVE_MAX);
    x = strtod(s, &end);
    if(end == s || *end != ',')
      return ft_fail(e, "curve: point %d: expected x,y", c->n + 1);
    s = end + 1;
    y = strtod(s, &end);
    if(end == s || (*end && *end != ' '))
      return ft_fail(e, "curve: point %d: expected x,y", c->n + 1);
    s = end;
    if(!(x >= 0 && x <= 1 && y >= 0 && y <= 1))
      return ft_fail(e, "curve: point %d (%g,%g) outside [0,1]", c->n + 1, x, y);
    c->x[c->n] = (float)x;
    c->y[c->n] = (float)y;
    c->n++;
  }
  return ft_curve_validate(c, e);
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_render.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

/* ITU-R BT.2087: linear sRGB (BT.709) -> linear Rec.2020 */
static const float SRGB_TO_2020[9] = {
  0.6274038959f, 0.3292830384f, 0.0433130657f, 0.0690972894f, 0.9195403951f,
  0.0113623156f, 0.0163914389f, 0.0880133079f, 0.8955952532f,
};
/* XYZ (D65) -> linear Rec.2020 */
static const double XYZ_TO_2020[9] = {
  1.7166511880, -0.3556707838, -0.2533662814, -0.6666843518, 1.6164812366,
  0.0157685458, 0.0176398574,  -0.0427706133, 0.9421031212,
};
static const char CH[] = "rgb";

double ft_srgb_encode(double v) {
  if(!(v > 0)) return 0;
  if(v >= 1) return 1;
  return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1 / 2.4) - 0.055;
}

double ft_srgb_decode(double v) {
  if(!(v > 0)) return 0;
  if(v >= 1) return 1;
  return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

void ft_recipe_default(ft_recipe* r, ft_render_mode m) {
  int c;
  memset(r, 0, sizeof *r);
  r->mode = m;
  for(c = 0; c < 3; c++) {
    r->base[c] = m == FT_RENDER_SLIDE ? 3.0f : 0;
    r->range[c] = 1;
    ft_curve_identity(&r->ch[c]);
  }
  r->grade = 1.5f;
  r->paper_black = 2.0f;
  r->paper_white = 0;
  r->bw_weights[0] = 0.2126f;
  r->bw_weights[1] = 0.7152f;
  r->bw_weights[2] = 0.0722f;
  memcpy(r->matrix, SRGB_TO_2020, sizeof r->matrix);
  ft_curve_identity(&r->master);
}

/* lo <= v <= hi, false for NaN */
static int within(double v, double lo, double hi) { return v >= lo && v <= hi; }

static int bad_curve(const ft_curve* c, const char* name, ft_err* e) {
  ft_err inner;
  if(ft_curve_validate(c, &inner) == 0) return 0;
  return ft_fail(e, "recipe: %s %s", name, inner.msg);
}

int ft_recipe_validate(const ft_recipe* r, ft_err* e) {
  static const char* const CURVE[3] = {"red curve", "green curve",
                                       "blue curve"};
  double sum = 0;
  int c;
  if((int)r->mode < FT_RENDER_NEG || (int)r->mode > FT_RENDER_BW)
    return ft_fail(e, "recipe: unknown mode %d", (int)r->mode);
  for(c = 0; c < 3; c++) {
    if(!within(r->base[c], 0, 4.5))
      return ft_fail(e, "recipe: base %c = %g (0..4.5 D)", CH[c], r->base[c]);
    if(r->mode == FT_RENDER_SLIDE && r->base[c] < 0.1)
      return ft_fail(e,
                     "recipe: base %c = %g: a slide's black is "
                     ">= 0.1 D",
                     CH[c], r->base[c]);
    if(!within(r->range[c], 0.05, 4.5))
      return ft_fail(e, "recipe: range %c = %g (0.05..4.5 D)", CH[c],
                     r->range[c]);
    if(!within(r->tint[c], -50, 50))
      return ft_fail(e, "recipe: tint %c = %g (-50..50 CC)", CH[c], r->tint[c]);
    if(r->mode == FT_RENDER_BW && r->tint[c] != 0)
      return ft_fail(e, "recipe: tint must be 0 for b&w");
    if(!within(r->bw_weights[c], 0, 1))
      return ft_fail(e, "recipe: bw weight %c = %g (0..1)", CH[c],
                     r->bw_weights[c]);
    sum += r->bw_weights[c];
  }
  if(!within(sum, 1 - 1e-3, 1 + 1e-3))
    return ft_fail(e, "recipe: bw weights sum to %g, not 1", sum);
  if(!within(r->exposure, -6, 6))
    return ft_fail(e, "recipe: exposure %g (-6..6 stops)", r->exposure);
  if(!within(r->grade, 0.2, 5))
    return ft_fail(e, "recipe: grade %g (0.2..5)", r->grade);
  if(!within(r->paper_white, 0, 4) ||
     !within(r->paper_black, 0, 4) ||
     !(r->paper_black > r->paper_white))
    return ft_fail(e,
                   "recipe: paper white %g, black %g (0 <= white < "
                   "black <= 4 D)",
                   r->paper_white, r->paper_black);
  for(c = 0; c < 9; c++)
    if(!isfinite(r->matrix[c]))
      return ft_fail(e, "recipe: matrix entry %d is not finite", c);
  if(bad_curve(&r->master, "master curve", e)) return -1;
  for(c = 0; c < 3; c++)
    if(bad_curve(&r->ch[c], CURVE[c], e)) return -1;
  return 0;
}

void ft_recipe_set_base(ft_recipe* r, const float d[3]) {
  int c;
  for(c = 0; c < 3; c++)
    if(!isfinite(d[c])) return;
  if(r->mode == FT_RENDER_BW) {
    double t = 0;
    float y;
    for(c = 0; c < 3; c++) t += r->bw_weights[c] * pow(10, -d[c]);
    y = (float)-log10(t > FT_T_FLOOR ? t : FT_T_FLOOR);
    for(c = 0; c < 3; c++) r->base[c] = y;
  } else {
    for(c = 0; c < 3; c++) r->base[c] = d[c];
  }
}

/* Film to linear for channel c (b&w: luminance, the green slots): the
 * negative's print model or the slide's black point and gains. */
static double inner(const ft_recipe* r, int c, double t) {
  double n, lift, u, s, p;
  if(!(t >= FT_T_FLOOR)) t = FT_T_FLOOR;
  if(t > FT_RENDER_T_MAX) t = FT_RENDER_T_MAX;
  if(r->mode == FT_RENDER_SLIDE) {
    double tb = pow(10, -r->base[c]);
    return (t - tb) /
           (1 - tb) *
           pow(2, r->exposure) *
           pow(10, r->tint[c] / 100.0);
  }
  /* 0 = film base, 1 = the brightest highlight */
  n = (-log10(t) - r->base[c]) / r->range[c];
  /* print exposure: + exposure or + tint = less light on the paper */
  lift = r->exposure * log10(2) + r->tint[c] / 100.0;
  u = 0.5 - n - lift / r->range[1];
  s = 1 / (1 + exp(-4 * r->grade * u)); /* 0 = white, 1 = black */
  p = r->paper_white + (r->paper_black - r->paper_white) * s;
  return pow(10, -(p - r->paper_white));
}

/* curves on the sRGB-encoded value; linear in, linear out */
static double tail(const ft_recipe* r, int c, double v) {
  double e = ft_srgb_encode(v);
  e = ft_curve_eval(&r->master, e);
  if(r->mode != FT_RENDER_BW) e = ft_curve_eval(&r->ch[c], e);
  return ft_srgb_decode(e);
}

void ft_render_direct(const ft_recipe* r, const float in3[3], float out[3]) {
  double l[3];
  int c;
  if(r->mode == FT_RENDER_BW) {
    double y = r->bw_weights[0] * (double)in3[0] +
               r->bw_weights[1] * (double)in3[1] +
               r->bw_weights[2] * (double)in3[2];
    /* grey: the matrix cannot change it, so skip it (exactly grey) */
    out[0] = out[1] = out[2] = (float)tail(r, 1, inner(r, 1, y));
    return;
  }
  for(c = 0; c < 3; c++) l[c] = inner(r, c, in3[c]);
  for(c = 0; c < 3; c++)
    out[c] = (float)tail(r, c,
                         r->matrix[3 * c] * l[0] +
                           r->matrix[3 * c + 1] * l[1] +
                           r->matrix[3 * c + 2] * l[2]);
}

int ft_render_compile(const ft_recipe* r, ft_render* c, ft_err* e) {
  int ch, i;
  if(ft_recipe_validate(r, e)) return -1;
  c->mode = r->mode;
  memcpy(c->w, r->bw_weights, sizeof c->w);
  memcpy(c->m, r->matrix, sizeof c->m);
  for(ch = 0; ch < 3; ch++) {
    int src = r->mode == FT_RENDER_BW ? 1 : ch;
    /* b&w: apply reads only channel 1's tables; zero the rest so the
     * struct contents stay deterministic */
    if(r->mode == FT_RENDER_BW && ch != 1) {
      memset(c->in[ch], 0, sizeof c->in[ch]);
      memset(c->out[ch], 0, sizeof c->out[ch]);
      continue;
    }
    for(i = 0; i < FT_RENDER_IN_N; i++) {
      uint32_t b = (FT_RENDER_IN_K0 + (uint32_t)i) << FT_RENDER_IN_SHIFT;
      float t;
      memcpy(&t, &b, sizeof t);
      c->in[ch][i] = (float)inner(r, src, t);
    }
    for(i = 0; i < FT_RENDER_CURVE_N; i++) {
      double s = (double)i / (FT_RENDER_CURVE_N - 1);
      c->out[ch][i] = (float)tail(r, ch, s * s);
    }
  }
  return 0;
}

/* input LUT: node k is the float with bits (K0 + k) << SHIFT, so the low
 * SHIFT mantissa bits are the exact linear position between two nodes */
float ft_render_lut_in(const float* tab, float t) {
  uint32_t b, i;
  float f;
  if(!(t >= (float)FT_T_FLOOR)) t = (float)FT_T_FLOOR; /* also NaN and -inf */
  if(t > FT_RENDER_T_MAX) t = FT_RENDER_T_MAX;
  memcpy(&b, &t, sizeof b);
  i = (b >> FT_RENDER_IN_SHIFT) - FT_RENDER_IN_K0;
  f = (float)(b & ((1u << FT_RENDER_IN_SHIFT) - 1)) *
      (1.0f / (1u << FT_RENDER_IN_SHIFT));
  return tab[i] + (tab[i + 1] - tab[i]) * f;
}

/* curve LUT, indexed by sqrt(v): fine steps in the shadows */
static float lut_out(const float* tab, float v) {
  float s;
  int i;
  if(!(v > 0)) return tab[0];
  if(v >= 1) return tab[FT_RENDER_CURVE_N - 1];
  s = sqrtf(v) * (FT_RENDER_CURVE_N - 1);
  i = (int)s;
  if(i > FT_RENDER_CURVE_N - 2) i = FT_RENDER_CURVE_N - 2;
  return tab[i] + (tab[i + 1] - tab[i]) * (s - (float)i);
}

void ft_render_apply(const ft_render* c, const float* in, float* out,
                     size_t npx) {
  size_t p;
  for(p = 0; p < npx; p++) {
    const float* s = in + 3 * p;
    float *d = out + 3 * p, t[3], l[3];
    int k;
    if(c->mode == FT_RENDER_BW) {
      float y = c->w[0] * s[0] + c->w[1] * s[1] + c->w[2] * s[2];
      d[0] = d[1] = d[2] = lut_out(c->out[1], ft_render_lut_in(c->in[1], y));
      continue;
    }
    t[0] = s[0];
    t[1] = s[1];
    t[2] = s[2];
    for(k = 0; k < 3; k++) l[k] = ft_render_lut_in(c->in[k], t[k]);
    for(k = 0; k < 3; k++)
      d[k] = lut_out(
        c->out[k],
        c->m[3 * k] * l[0] + c->m[3 * k + 1] * l[1] + c->m[3 * k + 2] * l[2]);
  }
}

void ft_render_image(const ft_render* c, ft_image* im) {
  int k;
  ft_render_apply(c, im->rgb, im->rgb, (size_t)im->w * im->h);
  for(k = 0; k < 9; k++) im->xyz_to_rgb[k] = XYZ_TO_2020[k];
}

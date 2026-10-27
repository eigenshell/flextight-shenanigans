/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The lut3d print module: the paper (and any look) as a 3D table, e.g. a
 * preset fitted by measuring another tool's output. The table's input is
 * the print exposure e_c (D: green-referenced film density above base
 * plus the filtration, clicks included; larger = lighter print), mapped
 * from lo..hi to 0..1; its output sRGB-encoded RGB (sRGB primaries), then
 * the recipe's matrix to linear Rec.2020. Colour negatives only. */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "ft_pm.h"

static void lut_recipe(const void* par, ft_recipe* r) {
  (void)par;
  (void)r;
}

static int lut_compile(const void* par, const ft_recipe* r, ft_print_render* pc,
                       ft_err* e) {
  const ft_pm_lut3d_par* p = par;
  ft_print_lut* l = &pc->u.l;
  int ch, i, n;
  if(ft_recipe_validate(r, e)) return -1;
  if(r->mode != FT_RENDER_NEG)
    return ft_fail(e, "lut3d: colour negatives only");
  if(!p->cube || p->cube->n < 2 || !(p->hi > p->lo))
    return ft_fail(e, "lut3d: bad table");
  n = p->cube->n;
  pc->lut = 1;
  l->cube = p->cube;
  memcpy(l->m, r->matrix, sizeof l->m);
  for(ch = 0; ch < 3; ch++) {
    double lift = r->exposure * log10(2) + r->tint[ch] / 100.0;
    for(i = 0; i < FT_RENDER_IN_N; i++) {
      uint32_t b = (FT_RENDER_IN_K0 + (uint32_t)i) << FT_RENDER_IN_SHIFT;
      float tf;
      double t, ev, x;
      memcpy(&tf, &b, sizeof tf);
      t = tf;
      if(t > FT_RENDER_T_MAX) t = FT_RENDER_T_MAX;
      ev = (-log10(t) - r->base[ch]) / r->range[ch] * r->range[1] + lift;
      x = (ev - p->lo) / (p->hi - p->lo);
      x = x < 0 ? 0 : x > 1 ? 1 : x;
      l->in[ch][i] = (float)(x * (n - 1));
    }
  }
  for(i = 0; i <= FT_PRINT_DEC_N; i++)
    l->dec[i] = (float)ft_srgb_decode((double)i / FT_PRINT_DEC_N);
  return 0;
}

const ft_pm_print ft_pm_print_lut3d = {"lut3d", lut_recipe, lut_compile};

/* tetrahedral interpolation at x (0..n-1 per axis) */
static void tetra(const ft_cube* cb, const float x[3], float out[3]) {
  int n = cb->n, i[3], k;
  float f[3];
  const float *c000, *c100, *c010, *c001, *c110, *c101, *c011, *c111;
  for(k = 0; k < 3; k++) {
    i[k] = (int)x[k];
    if(i[k] > n - 2) i[k] = n - 2;
    f[k] = x[k] - (float)i[k];
  }
#define NODE(a, b, c) \
  (cb->rgb + 3 * (((size_t)(i[2] + (c)) * n + i[1] + (b)) * n + i[0] + (a)))
  c000 = NODE(0, 0, 0);
  c100 = NODE(1, 0, 0);
  c010 = NODE(0, 1, 0);
  c001 = NODE(0, 0, 1);
  c110 = NODE(1, 1, 0);
  c101 = NODE(1, 0, 1);
  c011 = NODE(0, 1, 1);
  c111 = NODE(1, 1, 1);
#undef NODE
  for(k = 0; k < 3; k++) {
    float r = f[0], g = f[1], b = f[2];
    if(r >= g) {
      if(g >= b)
        out[k] = c000[k] +
                 r * (c100[k] - c000[k]) +
                 g * (c110[k] - c100[k]) +
                 b * (c111[k] - c110[k]);
      else if(r >= b)
        out[k] = c000[k] +
                 r * (c100[k] - c000[k]) +
                 b * (c101[k] - c100[k]) +
                 g * (c111[k] - c101[k]);
      else
        out[k] = c000[k] +
                 b * (c001[k] - c000[k]) +
                 r * (c101[k] - c001[k]) +
                 g * (c111[k] - c101[k]);
    } else {
      if(b > g)
        out[k] = c000[k] +
                 b * (c001[k] - c000[k]) +
                 g * (c011[k] - c001[k]) +
                 r * (c111[k] - c011[k]);
      else if(b > r)
        out[k] = c000[k] +
                 g * (c010[k] - c000[k]) +
                 b * (c011[k] - c010[k]) +
                 r * (c111[k] - c011[k]);
      else
        out[k] = c000[k] +
                 g * (c010[k] - c000[k]) +
                 r * (c110[k] - c010[k]) +
                 b * (c111[k] - c110[k]);
    }
  }
}

static float decode(const float* dec, float v) {
  float s;
  int i;
  if(!(v > 0)) return 0;
  if(v >= 1) return dec[FT_PRINT_DEC_N];
  s = v * FT_PRINT_DEC_N;
  i = (int)s;
  return dec[i] + (dec[i + 1] - dec[i]) * (s - (float)i);
}

void ft_pm_lut3d_apply(const ft_print_lut* l, const float* in, float* out,
                       size_t npx) {
  size_t p;
  for(p = 0; p < npx; p++) {
    const float* s = in + 3 * p;
    float *d = out + 3 * p, x[3], v[3], lin[3];
    int k;
    for(k = 0; k < 3; k++) x[k] = ft_render_lut_in(l->in[k], s[k]);
    tetra(l->cube, x, v);
    for(k = 0; k < 3; k++) lin[k] = decode(l->dec, v[k]);
    for(k = 0; k < 3; k++) {
      float o = l->m[3 * k] * lin[0] +
                l->m[3 * k + 1] * lin[1] +
                l->m[3 * k + 2] * lin[2];
      d[k] = o > 0 ? (o < 1 ? o : 1) : 0;
    }
  }
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_print.h"

#include <math.h>
#include <string.h>

#include "ft_pm.h"
#include "ft_render_auto.h"
#include "ft_resize.h"

double ft_print_median(double* v, int n) {
  int i, j;
  for (i = 1; i < n; i++) {
    double x = v[i];
    for (j = i; j > 0 && v[j - 1] > x; j--) v[j] = v[j - 1];
    v[j] = x;
  }
  return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

void ft_print_proxy_dims(int w, int h, int* pw, int* ph) {
  int l = w > h ? w : h;
  if (l <= FT_PRINT_STATS_EDGE) {
    *pw = w;
    *ph = h;
    return;
  }
  ft_resize_dims(w, h, (double)FT_PRINT_STATS_EDGE / l, pw, ph);
}

/* the emulsion's recipe before film and balance: mode, base, paper,
 * matrix */
static void skeleton(const ft_emulsion* em, const float base[3], ft_recipe* r) {
  ft_recipe_default(r, em->mode);
  memcpy(r->base, base, sizeof r->base);
  em->print->recipe(em->print_par, r);
  em->output->recipe(em->output_par, r);
}

int ft_print_roll_measure(const ft_emulsion* em, const float base[3],
                          const ft_image* const* im, int n, ft_print_frame* fr,
                          ft_print_roll* roll, ft_err* e) {
  ft_recipe r0, r;
  int i;
  if (n < 1 || n > FT_MAX_FRAMES)
    return ft_fail(e, "print: %d frames (1..%d)", n, FT_MAX_FRAMES);
  memset(roll, 0, sizeof *roll);
  roll->mode = em->mode;
  roll->nframes = n;
  ft_recipe_default(&r0, em->mode);
  ft_recipe_set_base(&r0, base); /* b&w: folded to luminance */
  memcpy(roll->base, r0.base, sizeof roll->base);
  skeleton(em, roll->base, &r0);
  for (i = 0; i < n; i++) { /* pass 1: each frame's range */
    const ft_image* one[1];
    memset(&fr[i], 0, sizeof fr[i]);
    one[0] = im[i];
    r = r0;
    if (ft_render_auto(one, 1, &r, e)) return -1;
    memcpy(fr[i].range, r.range, sizeof fr[i].range);
  }
  if (em->film->roll && em->film->roll(em->film_par, fr, n, roll, e)) return -1;
  if (em->balance->measure) /* pass 2: the balance's means */
    for (i = 0; i < n; i++) {
      r = r0;
      em->film->range(em->film_par, roll, &fr[i], &r);
      if (em->balance->measure(em->balance_par, &r, im[i], &fr[i], e))
        return -1;
    }
  if (em->balance->roll && em->balance->roll(em->balance_par, fr, n, roll, e))
    return -1;
  return 0;
}

int ft_print_clicks_ok(const ft_emulsion* em, ft_cmyd k, ft_err* e) {
  const int v[4] = {k.c, k.m, k.y, k.d};
  static const char N[] = "CMYD";
  int i;
  for (i = 0; i < 4; i++)
    if (v[i] < -FT_PRINT_CLICKS_MAX || v[i] > FT_PRINT_CLICKS_MAX)
      return ft_fail(e, "cmyd: %c %d is out of range (-%d..%d)", N[i], v[i],
                     FT_PRINT_CLICKS_MAX, FT_PRINT_CLICKS_MAX);
  if (em->mode == FT_RENDER_BW && (k.c || k.m || k.y))
    return ft_fail(e,
                   "cmyd: %s is black and white: C, M and Y must "
                   "be 0",
                   em->id);
  return 0;
}

static float clampf(float v, float lim, int* hit) {
  if (v > lim) {
    *hit = 1;
    return lim;
  }
  if (v < -lim) {
    *hit = 1;
    return -lim;
  }
  return v;
}

int ft_print_recipe(const ft_emulsion* em, const ft_print_roll* roll,
                    const ft_print_frame* fr, ft_cmyd k, ft_recipe* r,
                    ft_print_auto* a, int* clamped, ft_err* e) {
  const int cmy[3] = {k.c, k.m, k.y};
  ft_print_auto au;
  int c, hit = 0;
  if (ft_print_clicks_ok(em, k, e)) return -1;
  skeleton(em, roll->base, r);
  em->film->range(em->film_par, roll, fr, r);
  memset(&au, 0, sizeof au);
  em->balance->frame(em->balance_par, roll, fr, r, &au);
  if (a) *a = au;
  /* a click is 0.025 D less print exposure (+D: denser) or less light
   * of the complementary colour (+C: less red, so more cyan) */
  r->exposure =
    clampf(au.exposure - (float)(k.d * FT_PRINT_CLICK / log10(2)), 6, &hit);
  for (c = 0; c < 3; c++)
    r->tint[c] =
      em->mode == FT_RENDER_BW
        ? 0
        : clampf(au.tint[c] - (float)(cmy[c] * 100 * FT_PRINT_CLICK), 50, &hit);
  if (clamped) *clamped = hit;
  return 0;
}

int ft_print_compile(const ft_emulsion* em, const ft_recipe* r,
                     ft_print_render* c, ft_err* e) {
  if (em->print->compile) return em->print->compile(em->print_par, r, c, e);
  c->lut = 0;
  return ft_render_compile(r, &c->u.r, e);
}

void ft_print_apply(const ft_print_render* c, const float* in, float* out,
                    size_t npx) {
  if (c->lut)
    ft_pm_lut3d_apply(&c->u.l, in, out, npx);
  else
    ft_render_apply(&c->u.r, in, out, npx);
}

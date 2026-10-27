/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The frontier balance: how photofinishing printers set density and
 * colour per frame. Each frame's large-area mean film density per channel
 * (strongly coloured areas left out, so a lawn or a red wall barely
 * counts) is compared with the roll: the roll's median is printed at the
 * aim in full, a frame's deviation from it only by the slopes k_d
 * (density) and k_c (colour, < 1, at most clamp D on any channel), so a
 * sunset keeps much of its warmth. Film density d_c is green-referenced D above
 * base (the recipe's n_c x range[green]); the print exposure e_c = d_c +
 * lift_c, lift = exposure log10(2) + tint / 100 (ft_render.c). */
#include <math.h>
#include <string.h>

#include "ft_pm.h"

#define BLOCK 4       /* means on 4x4 blocks: noise / 4 */
#define EDGE 50       /* 2 % in from each edge (the rebate) */
#define CLEAR_T 0.9   /* open beam in all three channels */
#define OPAQUE_T 1e-4 /* 4.0 D in all three: not film */

static int frontier_measure(const void* par, const ft_recipe* r,
                            const ft_image* im, ft_print_frame* fr, ft_err* e) {
  const ft_pm_frontier_par* p = par;
  double sum[3] = {0, 0, 0}, all[3] = {0, 0, 0};
  unsigned long kept = 0, n = 0;
  int x0 = im->w / EDGE, x1 = im->w - im->w / EDGE;
  int y0 = im->h / EDGE, y1 = im->h - im->h / EDGE, x, y, c, i, j;
  int bw = r->mode == FT_RENDER_BW;
  if(r->mode == FT_RENDER_SLIDE)
    return ft_fail(e, "frontier: colour negatives and b&w only");
  for(y = y0; y + BLOCK <= y1; y += BLOCK)
    for(x = x0; x + BLOCK <= x1; x += BLOCK) {
      double t[3] = {0, 0, 0}, d[3], m;
      int bad = 0;
      for(j = 0; j < BLOCK; j++)
        for(i = 0; i < BLOCK; i++) {
          const float* q = im->rgb + 3 * ((size_t)(y + j) * im->w + x + i);
          for(c = 0; c < 3; c++) {
            if(!isfinite(q[c])) bad = 1;
            t[c] += q[c];
          }
        }
      if(bad) continue;
      for(c = 0; c < 3; c++) t[c] /= BLOCK * BLOCK;
      if((t[0] > CLEAR_T && t[1] > CLEAR_T && t[2] > CLEAR_T) ||
         (t[0] < OPAQUE_T && t[1] < OPAQUE_T && t[2] < OPAQUE_T))
        continue;
      if(bw) {
        double l = r->bw_weights[0] * t[0] +
                   r->bw_weights[1] * t[1] +
                   r->bw_weights[2] * t[2];
        d[0] = d[1] = d[2] =
          -log10(l > FT_T_FLOOR ? l : FT_T_FLOOR) - r->base[1];
      } else {
        for(c = 0; c < 3; c++)
          d[c] = (-log10(t[c] > FT_T_FLOOR ? t[c] : FT_T_FLOOR) - r->base[c]) /
                 r->range[c] *
                 r->range[1];
      }
      m = (d[0] + d[1] + d[2]) / 3;
      n++;
      for(c = 0; c < 3; c++) all[c] += d[c];
      if(fabs(d[0] - m) > p->sat ||
         fabs(d[1] - m) > p->sat ||
         fabs(d[2] - m) > p->sat)
        continue;
      kept++;
      for(c = 0; c < 3; c++) sum[c] += d[c];
    }
  if(n == 0) return ft_fail(e, "frontier: no film pixels to measure");
  for(c = 0; c < 3; c++) /* all saturated: use everything */
    fr->mean[c] = kept ? sum[c] / kept : all[c] / n;
  fr->has_mean = 1;
  return 0;
}

static double neutral(const double v[3]) { return (v[0] + v[1] + v[2]) / 3; }

static int frontier_roll(const void* par, const ft_print_frame* fr, int n,
                         ft_print_roll* roll, ft_err* e) {
  const ft_pm_frontier_par* p = par;
  double v[FT_MAX_FRAMES];
  int i, c;
  (void)e;
  for(i = 0; i < n; i++) v[i] = neutral(fr[i].mean);
  roll->ref_d = ft_print_median(v, n);
  for(c = 0; c < 3; c++) {
    if(n < p->min_frames || roll->mode == FT_RENDER_BW) {
      roll->ref_c[c] = roll->mode == FT_RENDER_BW ? 0 : p->aim_c[c];
      continue;
    }
    for(i = 0; i < n; i++) v[i] = fr[i].mean[c] - neutral(fr[i].mean);
    roll->ref_c[c] = ft_print_median(v, n);
  }
  roll->has_ref = 1;
  return 0;
}

static void frontier_frame(const void* par, const ft_print_roll* roll,
                           const ft_print_frame* fr, const ft_recipe* r,
                           ft_print_auto* a) {
  const ft_pm_frontier_par* p = par;
  double m = neutral(fr->mean), lift_d, dev[3], corr[3], big = 0;
  double ref = neutral(roll->ref_c), dm;
  int c;
  (void)r;
  memset(a, 0, sizeof *a);
  /* density: the roll at the aim, the frame's deviation by k_d */
  lift_d = (p->aim - roll->ref_d) + p->k_d * (roll->ref_d - m);
  a->exposure = (float)(lift_d / log10(2));
  if(roll->mode == FT_RENDER_BW) return;
  /* colour (centred: density is above): the roll's cast in full, the
   * frame's own deviation by k_c, scaled down as a whole so that no
   * channel moves more than the clamp (the correction keeps its hue) */
  for(c = 0; c < 3; c++) dev[c] = (fr->mean[c] - m) - (roll->ref_c[c] - ref);
  dm = neutral(dev);
  for(c = 0; c < 3; c++) {
    corr[c] = p->k_c * (dev[c] - dm);
    if(fabs(corr[c]) > big) big = fabs(corr[c]);
  }
  for(c = 0; c < 3; c++) {
    if(big > p->clamp) corr[c] *= p->clamp / big;
    a->tint[c] = (float)(-100 * ((roll->ref_c[c] - ref) + corr[c]));
  }
}

const ft_pm_balance ft_pm_balance_frontier = {"frontier", frontier_measure,
                                              frontier_roll, frontier_frame};

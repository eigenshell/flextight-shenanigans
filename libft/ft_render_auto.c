/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_render_auto.h"

#include <math.h>
#include <string.h>

#define H_D0 -0.2 /* histogram: densities -0.2 .. 4.6 */
#define H_STEP 0.001
#define H_N 4800
#define D_MAX 4.5 /* no estimate beyond the T floor (4.5 D) */
#define MAX_SAMPLES                                             \
  4e6               /* pixels read per call: ~250 k blocks on a \
                     * 44 Mpx frame, still ~1 k highlight       \
                     * blocks for the median */
#define BLOCK 4     /* statistics on 4x4 means: noise / 4 */
#define SEL_MEAN 4  /* channel code: mean of the 3 densities */
#define SEL_LUMA 3  /* channel code: luminance density */
#define SAME_D 0.15 /* highlight neighbour: same area within */
/* Blocks that are not film: holder metal, an unexposed slide border in a
 * negative crop (all three channels at or beyond 4.0 D) must not become
 * the highlight; the open beam through a sprocket hole or a clear gap
 * (T above 0.9 in all three channels) must not become the film base. For
 * the slide black, the densest film is wanted, so only blocks at the T
 * floor (4.4 D and beyond in all channels) are metal. */
#define OPAQUE_T 1e-4             /* 4.0 D */
#define SLIDE_OPAQUE_T 3.98107e-5 /* 4.4 D */
/* Holder metal reads T ~ 0.001 +- 0.0005 per pixel, far above the block-
 * mean limit. A metal block (or a block with a metal column at an edge)
 * almost always has >= 4 of its 16 pixels at T <= 0.002 in all three
 * channels; film up to ~2.6 D almost never does (noise would have to dip
 * 2 sigma in three channels at once, four times). Beyond ~2.8 D film and
 * metal look the same to the scanner: no estimate can tell them apart. */
#define METAL_T 0.002
#define METAL_MIN_PX 4
#define NO_METAL_T -1.0 /* pixel test off */
#define CLEAR_T 0.9
#define NO_LIMIT 1e30

/* Block means of transmittance for the BLOCK x BLOCK block at x, y: dense
 * highlights are a few DN above dark and one pixel's density is mostly
 * noise there. Floored (also NaN) so logs are finite. Returns 1 if the
 * block is not film: it has a non-finite value, >= METAL_MIN_PX pixels at
 * T <= metal in all three channels (holder metal, also the edge of one),
 * or any pixel at T > clear in all three (a hole, the open beam): a block
 * that straddles such an edge is a mix, not the highlight or the base. */
static int block_t(const ft_image* m, int x, int y, double metal, double clear,
                   double t[3]) {
  int i, j, c, bad = 0, dark = 0;
  t[0] = t[1] = t[2] = 0;
  for(j = 0; j < BLOCK; j++)
    for(i = 0; i < BLOCK; i++) {
      const float* p = m->rgb + 3 * ((size_t)(y + j) * m->w + x + i);
      for(c = 0; c < 3; c++) t[c] += p[c];
      if(!isfinite(p[0]) ||
         !isfinite(p[1]) ||
         !isfinite(p[2]) ||
         (p[0] > clear && p[1] > clear && p[2] > clear))
        bad = 1;
      if(p[0] <= metal && p[1] <= metal && p[2] <= metal) dark++;
    }
  for(c = 0; c < 3; c++) {
    t[c] /= BLOCK * BLOCK;
    if(!(t[c] >= FT_T_FLOOR)) t[c] = FT_T_FLOOR;
  }
  return bad || dark >= METAL_MIN_PX;
}

/* the block's mean is not film either (same test on the means) */
static int skipped(const double t[3], double dense, double clear) {
  return (t[0] <= dense && t[1] <= dense && t[2] <= dense) ||
         (t[0] > clear && t[1] > clear && t[2] > clear);
}

/* density of channel ch (0..2, SEL_LUMA by w, SEL_MEAN) from block means */
static double density(const double t[3], int ch, const float w[3]) {
  double l;
  if(ch < 3) return -log10(t[ch]);
  if(ch == SEL_LUMA) {
    l = w[0] * t[0] + w[1] * t[1] + w[2] * t[2];
    return -log10(l > FT_T_FLOOR ? l : FT_T_FLOOR);
  }
  return -log10(t[0] * t[1] * t[2]) / 3; /* product >= 3e-14 */
}

/* The histogram of block densities of channel ch over every frame, 2 % in
 * from each edge, strided so that at most MAX_SAMPLES pixels are read in
 * all. Blocks that are not film (see skipped) are left out. With sel >= 0:
 * only where the block's density of channel sel is >= min, measuring the
 * block 2 * BLOCK to its right: same scene (highlights are areas), but
 * independent noise and grain (dye clouds of 10-30 um span a few pixels at
 * 6300 ppi, so the adjacent block is correlated); selecting a block for
 * its own value would pick its noise. Returns the count. */
static unsigned long histogram(const ft_image* const* im, int n, int ch,
                               const float w[3], int sel, double min,
                               double dense, double pmetal, double clear,
                               unsigned* h) {
  unsigned long total = 0;
  double npx = 0;
  int f, step;
  for(f = 0; f < n; f++)
    if(im[f]->rgb) npx += (double)im[f]->w * im[f]->h;
  step = (int)ceil(BLOCK * sqrt(npx / MAX_SAMPLES));
  if(step < BLOCK) step = BLOCK;
  memset(h, 0, H_N * sizeof *h);
  for(f = 0; f < n; f++) {
    const ft_image* m = im[f];
    int x0 = m->w / 50, x1 = m->w - m->w / 50;
    int y0 = m->h / 50, y1 = m->h - m->h / 50, x, y;
    if(!m->rgb) continue;
    for(y = y0; y + BLOCK <= y1; y += step)
      for(x = x0; x + (sel >= 0 ? 3 : 1) * BLOCK <= x1; x += step) {
        double t[3], d;
        if(sel >= 0) {
          if(block_t(m, x, y, pmetal, clear, t) ||
             skipped(t, dense, clear) ||
             density(t, sel, w) < min)
            continue;
          /* neighbour in other scene content: skip it. The
           * margin is ~5x the block noise, so this hardly
           * selects on noise */
          if(block_t(m, x + 2 * BLOCK, y, pmetal, clear, t) ||
             skipped(t, dense, clear) ||
             density(t, sel, w) < min - SAME_D)
            continue;
        } else {
          if(block_t(m, x, y, pmetal, clear, t) || skipped(t, dense, clear))
            continue;
        }
        d = (density(t, ch, w) - H_D0) / H_STEP;
        if(!(d >= 0)) d = 0; /* also -inf, NaN */
        if(d > H_N - 1) d = H_N - 1;
        h[(int)d]++;
        total++;
      }
  }
  return total;
}

/* density at quantile q (0..1) of the histogram */
static double quantile(const unsigned* h, unsigned long total, double q) {
  double want, cum = 0;
  int b;
  /* defensive: an empty histogram has no quantile (callers check) */
  if(total == 0) return H_D0;
  want = q * (double)(total - 1);
  for(b = 0; b < H_N; b++) {
    cum += h[b];
    if(cum > want) break;
  }
  if(b == H_N) b = H_N - 1;
  return H_D0 + (b + 0.5) * H_STEP;
}

int ft_render_base_from_frames(const ft_image* const* im, int n,
                               ft_render_mode m, float d[3], ft_err* e) {
  unsigned h[H_N];
  int c;
  for(c = 0; c < 3; c++) {
    unsigned long tot;
    double q;
    if(m == FT_RENDER_SLIDE)
      tot = histogram(im, n, c, NULL, -1, 0, SLIDE_OPAQUE_T, NO_METAL_T,
                      NO_LIMIT, h);
    else
      tot = histogram(im, n, c, NULL, -1, 0, 0, NO_METAL_T, CLEAR_T, h);
    if(tot == 0) return ft_fail(e, "auto: no pixels for a film base estimate");
    q = quantile(h, tot, m == FT_RENDER_SLIDE ? 0.995 : 0.005);
    d[c] = (float)(q > D_MAX ? D_MAX : q > 0 ? q : 0);
  }
  return 0;
}

int ft_render_auto(const ft_image* const* im, int n, ft_recipe* r, ft_err* e) {
  unsigned h[H_N];
  unsigned long tot;
  double min;
  int c, sel, bw = r->mode == FT_RENDER_BW;
  for(c = 0; c < 3; c++) r->tint[c] = 0;
  if(r->mode == FT_RENDER_SLIDE) {
    r->exposure = 0;
    return 0;
  }
  /* The brightest 0.5 % of the scene (densest film), picked once on the
   * mean density so every channel sees the same blocks (this favours
   * blocks dense in the high-gamma, blue layer); each channel's range
   * is its median on the neighbours of those blocks. A percentile per
   * channel would pick each channel's own noise tail, the blue most: a
   * colour cast. */
  sel = bw ? SEL_LUMA : SEL_MEAN;
  tot =
    histogram(im, n, sel, r->bw_weights, -1, 0, OPAQUE_T, METAL_T, NO_LIMIT, h);
  if(tot == 0) return ft_fail(e, "auto: no pixels to estimate from");
  min = quantile(h, tot, 0.995) - H_STEP / 2;
  for(c = 0; c < (bw ? 1 : 3); c++) {
    int ch = bw ? SEL_LUMA : c;
    double range;
    tot = histogram(im, n, ch, r->bw_weights, sel, min, OPAQUE_T, METAL_T,
                    NO_LIMIT, h);
    if(tot == 0) {
      /* a thin highlight with no same-area neighbour to measure:
       * use the channel's own 99.5th percentile instead, slightly
       * noise-biased but better than nothing */
      tot = histogram(im, n, ch, r->bw_weights, -1, 0, OPAQUE_T, METAL_T,
                      NO_LIMIT, h);
      range = quantile(h, tot, 0.995) - r->base[c];
    } else
      range = quantile(h, tot, 0.5) - r->base[c];
    if(!(range > 0.05)) range = 0.05; /* also NaN */
    if(range > D_MAX) range = D_MAX;
    r->range[c] = (float)range;
  }
  if(bw) r->range[1] = r->range[2] = r->range[0];
  r->exposure = ft_render_auto_exposure(r);
  return 0;
}

float ft_render_auto_exposure(const ft_recipe* r) {
  double st, ex;
  /* the highlight (n = 1) at paper white + 0.03 D:
   * s = 0.03 / (black - white), z = logit(s) = 4 grade u,
   * u = -0.5 - lift / range[1], lift = exposure log10(2) */
  st = 0.03 / (r->paper_black - r->paper_white);
  if(!(st > 1e-6)) st = 1e-6;
  if(st > 1 - 1e-6) st = 1 - 1e-6;
  ex = -(log(st / (1 - st)) / (4 * r->grade) + 0.5) * r->range[1] / log10(2);
  if(!(r->grade > 0) || !isfinite(ex)) ex = 0;
  if(ex > 6) ex = 6;
  if(ex < -6) ex = -6;
  return (float)ex;
}

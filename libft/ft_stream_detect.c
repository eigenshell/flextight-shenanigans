/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Streaming frame detection (ft_stream_detect.h). */
#include "ft_stream_detect.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define OPEN_T 0.85f     /* all channels above: open beam (ft_detect) */
#define DARK_T 0.005f    /* all channels below: holder metal (ft_detect) */
#define NOFILM_OPEN 0.50 /* band this much open: no film on the line */
#define METAL_FRAC 0.80  /* band this much dark: metal (negatives) */
#define FLAT_D 0.12      /* a flat line: 3rd..97th percentile per channel */
#define SLIDE_D 2.0      /* slide gap: 97 % of the band denser than this */
#define BASE_MM 10.0     /* base samples kept: ~10 mm of gap lines */
#define SLIVER_MM 3.0
#define FULL_LO 33.0
#define FULL_HI 39.5
#define HALF_LO 15.5
#define HALF_HI 21.0
#define END_TOL 1.5 /* at the film's ends: this close to 36 / 18 */
#define GRID_MM 36.0
#define EXPOSED 0.30    /* a column of the image: this share of lines */
#define GROW_GAP_MM 0.4 /* across: unexposed columns crossed at most */
#define BINS 80
#define STEP 0.05 /* density per bin: 0..4 */
#define BLOCK 4   /* px averaged for the line statistics */
#define D_OF(t) (-log10((t) > 1e-4f ? (double)(t) : 1e-4))

enum { L_NOFILM, L_GAP, L_FRAME };

/* k-th smallest of v[0..n) (reorders v) */
static float kth(float* v, int n, int k) {
  int lo = 0, hi = n - 1;
  while(lo < hi) {
    float piv = v[(lo + hi) / 2], tmp;
    int i = lo, j = hi;
    while(i <= j) {
      while(v[i] < piv) i++;
      while(v[j] > piv) j--;
      if(i <= j) {
        tmp = v[i];
        v[i] = v[j];
        v[j] = tmp;
        i++;
        j--;
      }
    }
    if(k <= j)
      hi = j;
    else if(k >= i)
      lo = i;
    else
      break;
  }
  return v[k];
}

static int col(double mm, const ft_axes* ax, int up) {
  double c = (mm - ax->x_mm0) / ax->x_mm_per_px;
  return (int)(up ? ceil(c - 1e-9) : floor(c + 1e-9));
}

int ft_sdet_init(ft_sdet* d, const ft_axes* ax, int px, ft_film film,
                 const ft_holder* h, ft_err* e) {
  double ym = ax ? ax->y_mm_per_line : 0;
  memset(d, 0, sizeof *d);
  if(!h || !ax || px < 1 || !(ym > 0) || !(ax->x_mm_per_px > 0))
    return ft_fail(e, "stream detect: bad geometry");
  d->ax = *ax;
  d->px = px;
  d->film = film;
  d->h = h;
  d->b0 = col(h->band_start_mm, ax, 1);
  d->b1 = col(h->band_end_mm, ax, 0);
  d->s0 = col(h->image_start_mm, ax, 0);
  d->s1 = col(h->image_end_mm, ax, 1);
  if(d->b0 < 0) d->b0 = 0;
  if(d->b1 > px) d->b1 = px;
  if(d->s0 < 0) d->s0 = 0;
  if(d->s1 > px) d->s1 = px;
  if(d->b1 - d->b0 < 8 * BLOCK)
    return ft_fail(e,
                   "stream detect: the holder's image band is not "
                   "on the line");
  if(d->s0 > d->b0) d->s0 = d->b0;
  if(d->s1 < d->b1) d->s1 = d->b1;
  d->gap_lines = (int)ceil(FT_SDET_GAP_MM / ym - 1e-9);
  d->weak_lines = d->gap_lines / 2 < 3 ? 3 : d->gap_lines / 2;
  d->grid_lines = (int)floor(FULL_HI / ym + 1e-9);
  d->cut_lines = (int)floor(GRID_MM / ym + 0.5);
  d->ring_cap = (int)floor(BASE_MM / ym + 0.5);
  if(d->ring_cap < 8) d->ring_cap = 8;
  if(d->ring_cap > FT_SDET_RING) d->ring_cap = FT_SDET_RING;
  d->run_s = -1;
  d->weak_s = d->weak_e = -1;
  d->hist = calloc((size_t)px * 3 * BINS, sizeof *d->hist);
  d->valid = calloc((size_t)px, sizeof *d->valid);
  d->scratch = malloc((size_t)(d->s1 - d->s0) * 3 * sizeof *d->scratch);
  if(!d->hist || !d->valid || !d->scratch) {
    ft_sdet_free(d);
    return ft_fail(e, "stream detect: out of memory");
  }
  return 0;
}

void ft_sdet_free(ft_sdet* d) {
  free(d->hist);
  free(d->valid);
  free(d->scratch);
  d->hist = d->valid = NULL;
  d->scratch = NULL;
}

int ft_sdet_base(const ft_sdet* d, float base[3]) {
  int c;
  if(!d->have_base) return 0;
  for(c = 0; c < 3; c++) base[c] = (float)pow(10, -d->base[c]);
  return 1;
}

static void base_update(ft_sdet* d) {
  float v[FT_SDET_RING];
  int c, i;
  for(c = 0; c < 3; c++) {
    for(i = 0; i < d->nring; i++) v[i] = d->ring[i][c];
    d->base[c] = kth(v, d->nring, d->nring / 2);
  }
  d->have_base = 1;
}

static void base_push(ft_sdet* d, const float* s, int reset) {
  if(reset) {
    d->nring = 0;
    d->ring_at = 0;
  }
  memcpy(d->ring[d->ring_at], s, sizeof d->ring[0]);
  d->ring_at = (d->ring_at + 1) % d->ring_cap;
  if(d->nring < d->ring_cap) d->nring++;
  base_update(d);
}

static void run_reset(ft_sdet* d) {
  memset(d->hist, 0, (size_t)d->px * 3 * BINS * sizeof *d->hist);
  memset(d->valid, 0, (size_t)d->px * sizeof *d->valid);
}

/* column c of the open run: off the base on >= EXPOSED of its lines */
static int exposed(const ft_sdet* d, int c) {
  const unsigned short* hc = d->hist + (size_t)c * 3 * BINS;
  long v = d->valid[c];
  int k, b;
  if(v == 0) return 0;
  for(k = 0; k < 3; k++) {
    long n = 0;
    if(d->film == FT_FILM_SLIDE) {
      for(b = 0; b < (int)(SLIDE_D / STEP + 0.5); b++) n += hc[k * BINS + b];
    } else {
      double base = d->have_base ? d->base[k] : d->have_dmin ? d->dmin[k] : 0;
      int from = (int)ceil((base + FT_SDET_NEAR_D) / STEP - 1e-9);
      for(b = from < 0 ? 0 : from; b < BINS; b++) n += hc[k * BINS + b];
    }
    if(n >= EXPOSED * v) return 1;
  }
  return 0;
}

static double clampd(double v, double lo, double hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

/* seal the run [s, e): ts / te its ends at a gap (or the grid) */
static int seal(ft_sdet* d, long s, long e, int ts, int te, int grid) {
  const ft_axes* ax = &d->ax;
  double ym = ax->y_mm_per_line, len = (e - s) * ym;
  int gap = (int)ceil(GROW_GAP_MM / ax->x_mm_per_px - 1e-9), n, c;
  int c0 = d->b0, c1 = d->b1;
  ft_sdet_frame* f;
  ft_frame_type type;

  /* the film's end just after a short gap: the frame ended there */
  if(!te && !grid && d->weak_s > s && (e - d->weak_e) * ym < SLIVER_MM) {
    e = d->weak_s;
    te = 1;
    len = (e - s) * ym;
  }
  if(e <= s || (!grid && len < SLIVER_MM) || d->nframes == FT_MAX_FRAMES)
    return 0;
  if(grid)
    type = FT_FRAME_FULL;
  else if(ts && te)
    type = len >= FULL_LO && len <= FULL_HI   ? FT_FRAME_FULL
           : len >= HALF_LO && len <= HALF_HI ? FT_FRAME_HALF
                                              : FT_FRAME_PARTIAL;
  else
    type = fabs(len - 36) <= END_TOL   ? FT_FRAME_FULL
           : fabs(len - 18) <= END_TOL ? FT_FRAME_HALF
                                       : FT_FRAME_PARTIAL;
  /* across: the band, grown outward over the run's exposed columns */
  for(n = 0, c = d->b0 - 1; c >= d->s0 && n <= gap; c--) {
    if(exposed(d, c)) {
      c0 = c;
      n = 0;
    } else {
      n++;
    }
  }
  for(n = 0, c = d->b1; c < d->s1 && n <= gap; c++) {
    if(exposed(d, c)) {
      c1 = c + 1;
      n = 0;
    } else {
      n++;
    }
  }
  f = &d->f[d->nframes];
  memset(f, 0, sizeof *f);
  f->index = ++d->nframes;
  f->type = type;
  f->grid = grid;
  f->l0 = s;
  f->l1 = e;
  f->y0 = clampd(ax->y_mm0 + s * ym - FT_SDET_PAD_MM, 0, d->h->scan_length_mm);
  f->y1 = clampd(ax->y_mm0 + e * ym + FT_SDET_PAD_MM, 0, d->h->scan_length_mm);
  f->x0 = clampd(ax->x_mm0 + c0 * ax->x_mm_per_px - FT_SDET_PAD_MM, 0,
                 d->h->scan_width_mm);
  f->x1 = clampd(ax->x_mm0 + c1 * ax->x_mm_per_px + FT_SDET_PAD_MM, 0,
                 d->h->scan_width_mm);
  d->grid |= grid;
  return 1;
}

/* the open run takes line t: its columns' densities */
static void accumulate(ft_sdet* d, const float* t) {
  int c, k;
  for(c = d->s0; c < d->s1; c++) {
    const float* p = t + (size_t)c * 3;
    unsigned short* hc = d->hist + (size_t)c * 3 * BINS;
    if((p[0] > OPEN_T && p[1] > OPEN_T && p[2] > OPEN_T) ||
       (d->film != FT_FILM_SLIDE &&
        p[0] < DARK_T &&
        p[1] < DARK_T &&
        p[2] < DARK_T))
      continue;
    if(d->valid[c] == 0xffff) continue;
    d->valid[c]++;
    for(k = 0; k < 3; k++) {
      int b = (int)(D_OF(p[k]) / STEP);
      hc[k * BINS + (b < 0 ? 0 : b >= BINS ? BINS - 1 : b)]++;
    }
  }
}

/* the line's kind; base and dmin updated */
static int classify(ft_sdet* d, const float* t) {
  int nb = d->b1 - d->b0, nopen = 0, ndark = 0, c, k, flat = 1, gap = 1;
  float *v = d->scratch, p03[3], p50[3], p97[3];
  int lo = nb * 3 / 100, hi = nb - 1 - nb * 3 / 100;
  for(c = d->b0; c < d->b1; c++) {
    const float* p = t + (size_t)c * 3;
    nopen += p[0] > OPEN_T && p[1] > OPEN_T && p[2] > OPEN_T;
    ndark += p[0] < DARK_T && p[1] < DARK_T && p[2] < DARK_T;
  }
  if(nopen >= NOFILM_OPEN * nb ||
     (d->film != FT_FILM_SLIDE && ndark >= METAL_FRAC * nb))
    return L_NOFILM;
  /* the band in blocks of BLOCK px: sensor noise on a dense base (blue
   * T ~0.05) would otherwise spread a gap line past FLAT_D */
  nb /= BLOCK;
  lo = nb * 3 / 100;
  hi = nb - 1 - nb * 3 / 100;
  for(k = 0; k < 3; k++) {
    float* w = v + (size_t)k * nb;
    for(c = 0; c < nb; c++) {
      const float* p = t + (size_t)(d->b0 + c * BLOCK) * 3 + k;
      float s = 0;
      int i;
      for(i = 0; i < BLOCK; i++) s += p[i * 3];
      w[c] = (float)D_OF(s / BLOCK);
    }
    p03[k] = kth(w, nb, lo);
    p97[k] = kth(w, nb, hi);
    p50[k] = kth(w, nb, nb / 2);
    flat &= p97[k] - p03[k] <= FLAT_D;
  }
  if(d->film == FT_FILM_SLIDE) {
    for(k = 0; k < 3; k++) gap &= p03[k] >= SLIDE_D;
    if(gap) base_push(d, p50, 0);
    return gap ? L_GAP : L_FRAME;
  }
  if(nopen == 0)
    for(k = 0; k < 3; k++)
      if(!d->have_dmin || p03[k] < d->dmin[k]) d->dmin[k] = p03[k];
  if(nopen == 0) d->have_dmin = 1;
  if(flat) {
    double m = 0;
    int near = 1;
    for(k = 0; k < 3 && d->have_base; k++) {
      m += (p50[k] - d->base[k]) / 3;
      near &= fabs(p50[k] - d->base[k]) <= FT_SDET_NEAR_D;
    }
    if(!d->have_base || m < -FT_SDET_NEAR_D)
      base_push(d, p50, 1); /* thinner: that is the base */
    else if(near)
      base_push(d, p50, 0);
  }
  for(k = 0; k < 3; k++)
    gap &= d->have_base &&
           p03[k] >= d->base[k] - FT_SDET_NEAR_D &&
           p97[k] <= d->base[k] + FT_SDET_NEAR_D;
  return gap ? L_GAP : L_FRAME;
}

int ft_sdet_line(ft_sdet* d, const float* t) {
  int kind = classify(d, t), n = 0;
  long l = d->line++;

  if(kind == L_NOFILM) {
    if(d->run_s >= 0)
      n += seal(d, d->run_s, d->gap_n ? d->gap_s : l, d->run_trusted, 0, 0);
    d->run_s = -1;
    d->gap_n = 0;
    d->after_gap = 0;
    return n;
  }
  if(kind == L_GAP) {
    if(d->gap_n++ == 0) d->gap_s = l;
    if(d->gap_n == d->gap_lines) {
      if(d->run_s >= 0) n += seal(d, d->run_s, d->gap_s, d->run_trusted, 1, 0);
      d->run_s = -1;
      d->after_gap = 1;
    }
    return n;
  }
  if(d->run_s >= 0 && d->gap_n >= d->weak_lines) {
    if(!d->run_trusted &&
       (d->gap_s - d->run_s) * d->ax.y_mm_per_line < SLIVER_MM) {
      d->run_s = -1; /* the film's start: the frame's edge */
    } else {
      d->weak_s = d->gap_s;
      d->weak_e = l;
    }
  }
  if(d->run_s < 0) {
    d->run_trusted = d->after_gap || d->gap_n >= d->weak_lines;
    d->run_s = l;
    d->weak_s = d->weak_e = -1;
    run_reset(d);
  }
  d->gap_n = 0;
  accumulate(d, t);
  if(l + 1 - d->run_s > d->grid_lines) { /* no gap: the holder grid */
    long cut = d->run_s + d->cut_lines;
    n += seal(d, d->run_s, cut, 1, 1, 1);
    d->run_s = cut;
    d->run_trusted = 1;
    run_reset(d);
  }
  return n;
}

int ft_sdet_feed(ft_sdet* d, const float* lines, int n) {
  int i, k = 0;
  for(i = 0; i < n; i++) k += ft_sdet_line(d, lines + (size_t)i * d->px * 3);
  return k;
}

int ft_sdet_end(ft_sdet* d) {
  int n = 0;
  if(d->run_s >= 0)
    n = seal(d, d->run_s, d->gap_n ? d->gap_s : d->line, d->run_trusted, 0, 0);
  d->run_s = -1;
  d->gap_n = 0;
  return n;
}

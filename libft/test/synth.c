/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "test/synth.h"
#include "ft_geom.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846

void synth_default(synth_spec *s, ft_film film) {
  memset(s, 0, sizeof *s);
  s->px = 666;
  s->lines = 4795;
  s->y_mm_per_line = 25.4 / 525;
  s->x_mm_per_px = 25.4 * 12 / 6300;
  s->film = film;
  s->film_y0 = 3.0;
  s->film_y1 = 231.5;
  s->film_x0 = -1.4;
  s->film_x1 = 33.6;
  s->img_x0 = 4.1;
  s->img_x1 = 28.1;
  s->seed = 1;
  if(film == FT_FILM_SLIDE) {
    s->base[0] = s->base[1] = s->base[2] = 0.003f;
  } else {
    s->base[0] = 0.62f;
    s->base[1] = 0.40f;
    s->base[2] = 0.25f;
  }
}

void synth_add(synth_spec *s, double y0, double len, int blank) {
  s->fr[s->nframes].y0 = y0;
  s->fr[s->nframes].len = len;
  s->fr[s->nframes].blank = blank;
  s->nframes++;
}

static int in_hole(const synth_spec *s, double x, double yf) {
  double ph;
  int across = (x >= s->film_x0 + 2.0 && x < s->film_x0 + 4.8) ||
               (x >= s->film_x1 - 4.8 && x < s->film_x1 - 2.0);
  if(!across)
    return 0;
  ph = fmod(yf - s->film_y0 - 1.0, 4.75);
  if(ph < 0)
    ph += 4.75;
  return ph < 1.98;
}

static double content(double x, double y, int ch, unsigned seed) {
  double ph = seed * 0.37 + ch * 1.1;
  double v = 0.5 + 0.25 * sin(x * 1.3 + ph) * cos(y * 0.7 + 2 * ph) +
             0.25 * sin((x + y) * 2.1 + 3 * ph);
  return v < 0 ? 0 : v > 1 ? 1 : v;
}

static int base_like_area(const synth_spec *s, double x, double yf) {
  int i;
  for(i = 0; i < s->npatch; i++)
    if(yf >= s->patch[i].y0 && yf < s->patch[i].y1 && x >= s->patch[i].x0 &&
       x < s->patch[i].x1)
      return 1;
  return s->blob && (x - s->blob_x) * (x - s->blob_x) +
                            (yf - s->blob_y) * (yf - s->blob_y) <
                        s->blob_r * s->blob_r;
}

static int leak_area(const synth_spec *s, double x, double yf) {
  int i;
  for(i = 0; i < s->nleak && i < 4; i++)
    if(yf >= s->leak[i].y0 && yf < s->leak[i].y1 && x >= s->leak[i].x0 &&
       x < s->leak[i].x1)
      return 1;
  return 0;
}

/* Gaussian deviate from its own LCG (keeps the default noise sequence). */
static double gauss(unsigned *st) {
  double u, v;
  *st = *st * 1103515245u + 12345u;
  u = (((*st >> 8) & 0xffffff) + 0.5) / 16777216.0;
  *st = *st * 1103515245u + 12345u;
  v = ((*st >> 8) & 0xffffff) / 16777216.0;
  return sqrt(-2 * log(u)) * cos(2 * PI * v);
}

/* in-place running box filter of n values, stride st, radius r */
static void box1(float *p, int n, long st, int r, float *tmp) {
  double acc = 0;
  int i, a, b;
  for(i = 0; i < n; i++)
    tmp[i] = p[i * st];
  for(i = 0; i < r && i < n; i++)
    acc += tmp[i];
  for(i = 0; i < n; i++) {
    b = i + r;
    a = i - r - 1;
    if(b < n)
      acc += tmp[b];
    if(a >= 0)
      acc -= tmp[a];
    p[i * st] = (float)(acc / ((b < n ? b : n - 1) - (a >= 0 ? a : -1)));
  }
}

static void blur(const synth_spec *s, float *t) {
  int ry = (int)(s->blur_mm / s->y_mm_per_line + 0.5);
  int rx = (int)(s->blur_mm / s->x_mm_per_px + 0.5);
  int n = s->lines > s->px ? s->lines : s->px, l, c, k;
  float *tmp = malloc((size_t)n * sizeof *tmp);
  if(!tmp)
    return;
  for(c = 0; ry > 0 && c < s->px; c++)
    for(k = 0; k < 3; k++)
      box1(t + (size_t)c * 3 + k, s->lines, (long)s->px * 3, ry, tmp);
  for(l = 0; rx > 0 && l < s->lines; l++)
    for(k = 0; k < 3; k++)
      box1(t + (size_t)l * s->px * 3 + k, s->px, 3, rx, tmp);
  free(tmp);
}

float *synth_preview(const synth_spec *s) {
  const float *base = s->base;
  float *t = malloc((size_t)s->lines * s->px * 3 * sizeof *t);
  double tn = tan(s->tilt_deg * PI / 180), xc = 0.5 * (s->img_x0 + s->img_x1);
  double sy[64], sx[64], xmax = s->x_mm0 + s->px * s->x_mm_per_px;
  double period = s->lines * s->y_mm_per_line;
  unsigned rng = s->seed * 2654435761u + 1, g = s->seed * 40503u + 7;
  int l, c, k, i, nsp = s->nspeck < 64 ? s->nspeck : 64;

  if(!t)
    return NULL;
  for(i = 0; i < nsp; i++) { /* speck positions */
    double x0 = s->film_x0 > s->x_mm0 ? s->film_x0 : s->x_mm0;
    double x1 = s->film_x1 < xmax ? s->film_x1 : xmax;
    g = g * 1103515245u + 12345u;
    sy[i] =
        s->film_y0 + (s->film_y1 - s->film_y0) * ((g >> 8) & 0xffff) / 65536.0;
    g = g * 1103515245u + 12345u;
    sx[i] = x0 + (x1 - x0) * ((g >> 8) & 0xffff) / 65536.0;
  }
  for(l = 0; l < s->lines; l++)
    for(c = 0; c < s->px; c++) {
      double y = l * s->y_mm_per_line, x = s->x_mm0 + c * s->x_mm_per_px;
      double yf = y - (x - xc) * tn; /* tilted strip */
      double gain = 1 + s->drift * sin(2 * PI * y / period);
      float *p = t + ((size_t)l * s->px + c) * 3;
      int fr = -1, speck = 0;
      if(y < s->film_y0 || y >= s->film_y1 || /* holder outside film */
         (s->win_x1 > s->win_x0 && (x < s->win_x0 || x >= s->win_x1))) {
        for(k = 0; k < 3; k++) {
          double v = s->open_outside ? 1.0 : s->metal > 0 ? s->metal : 0.0005;
          if(!s->open_outside && s->metal_noise > 0)
            v += s->metal_noise * gauss(&g);
          p[k] = (float)(v < 0 ? 0 : v);
        }
        continue;
      }
      for(i = 0; i < s->nopen && i < 4; i++)
        if(y >= s->open[i].y0 && y < s->open[i].y1)
          speck = 1; /* no film: open holder */
      if(speck) {
        p[0] = p[1] = p[2] = 1.0f;
        continue;
      }
      if(x < s->film_x0 || x >= s->film_x1 || in_hole(s, x, yf) ||
         y < s->film_y0 +
                 s->cut_mm * (x - s->film_x0) / (s->film_x1 - s->film_x0)) {
        p[0] = p[1] = p[2] = 1.0f; /* open beam */
        continue;
      }
      for(i = 0; i < nsp; i++)
        if((x - sx[i]) * (x - sx[i]) + (y - sy[i]) * (y - sy[i]) <
           s->speck_r * s->speck_r)
          speck = 1;
      if(speck) {
        p[0] = p[1] = p[2] = 1.0f;
        continue;
      }
      for(i = 0; i < s->nframes; i++)
        if(yf >= s->fr[i].y0 && yf < s->fr[i].y0 + s->fr[i].len)
          fr = i;
      for(k = 0; k < 3; k++) {
        double v = base[k];
        if(leak_area(s, x, yf)) /* fogged: dense / clear */
          v = s->film == FT_FILM_NEGATIVE ? base[k] * 0.05 : 0.5;
        else if(
            fr >= 0 && !s->fr[fr].blank && x >= s->img_x0 && x < s->img_x1 &&
            !base_like_area(s, x, yf)
        ) {
          double n = content(x, yf, k, s->seed);
          v = s->film == FT_FILM_NEGATIVE
                  ? base[k] * pow(10, -(0.3 + 0.9 * n) * (1 - 0.1 * k))
                  : 0.03 + 0.6 * n;
        }
        rng = rng * 1103515245u + 12345u; /* +-0.4 % noise */
        v *= 1 + 0.004 * (((rng >> 16) & 1023) / 511.5 - 1);
        if(s->grain > 0)
          v *= 1 + s->grain * gauss(&g);
        v *= gain;
        p[k] = (float)(v < 0 ? 0 : v > 1 ? 1 : v);
      }
    }
  if(s->blur_mm > 0)
    blur(s, t);
  if(s->noise > 0)
    for(i = 0; i < 3; i++)
      for(l = 0; l < s->lines; l++)
        for(c = 0; c < s->px; c++) {
          float *p = t + ((size_t)l * s->px + c) * 3 + i;
          double v = *p + s->noise * gauss(&g);
          *p = (float)(v < 0 ? 0 : v > 1 ? 1 : v);
        }
  return t;
}

/* ------------------------------------------------------- window scans */

void synth_window(const synth_win *w, synth_win_scan *s) {
  synth_spec sp;
  double scale = w->ppi * w->bin, half = (w->bin - 1) / 2.0, xm0, xm1;
  double off;
  int k = 12 / w->bin, tl, l, c, i;
  float *t;

  memset(s, 0, sizeof *s);
  s->y = (long)floor(w->y0_mm / 25.4 * w->ppi + 0.5);
  s->x = (long)floor(
      (ft_mm_to_ccd(w->x0_mm, scale) - FT_CCD_FIRST) / w->bin + 0.5
  );
  xm0 = ft_ccd_to_mm(ft_out_to_ccd(s->x, 0, w->bin) + half, scale);
  xm1 = ft_ccd_to_mm(ft_out_to_ccd(s->x, 1, w->bin) + half, scale);
  s->px = (int)(w->w_mm / (xm1 - xm0));
  s->lines = (int)floor(w->len_mm / 25.4 * w->ppi + 0.5);
  tl = s->lines - 2 * k;

  synth_default(&sp, FT_FILM_NEGATIVE);
  sp.px = s->px;
  sp.lines = tl;
  sp.y_mm_per_line = 25.4 / w->ppi;
  sp.x_mm_per_px = xm1 - xm0;
  sp.x_mm0 = xm0;
  sp.film_y0 = -50;
  sp.film_y1 = 1000;
  sp.blur_mm = 0.05;
  sp.grain = 0.02;
  sp.open_outside = w->open;
  if(w->open)
    sp.film_y0 = sp.film_y1 = 2000; /* no film in the window */
  /* synth row l is holder (y + l + k) / ppi */
  off = (s->y + k) * 25.4 / w->ppi;
  for(i = 0; i < w->nfr; i++)
    synth_add(&sp, w->fy0[i] - off, w->flen[i], 0);
  for(i = 0; i < w->npatch && i < 4; i++) {
    sp.patch[i].y0 = w->patch[i].y0 - off;
    sp.patch[i].y1 = w->patch[i].y1 - off;
    sp.patch[i].x0 = w->patch[i].x0;
    sp.patch[i].x1 = w->patch[i].x1;
  }
  sp.npatch = w->npatch;
  memcpy(s->base, sp.base, sizeof s->base);
  t = synth_preview(&sp);
  s->raw = malloc((size_t)s->lines * s->px * 3 * sizeof *s->raw);
  s->cal_lines = 250;
  s->cal = malloc((size_t)s->cal_lines * s->px * 3 * sizeof *s->cal);
  if(!t || !s->raw || !s->cal) {
    free(t);
    free(s->raw);
    free(s->cal);
    s->raw = s->cal = NULL;
    return;
  }
  for(l = 0; l < s->lines; l++)
    for(c = 0; c < s->px; c++)
      for(i = 0; i < 3; i++) {
        int src = l - (i == 0 ? 2 * k : i == 1 ? k : 0);
        float v;
        if(src < 0)
          src = 0;
        if(src >= tl)
          src = tl - 1;
        v = t[((size_t)src * s->px + c) * 3 + i];
        if(v > 1.2f)
          v = 1.2f;
        s->raw[((size_t)l * s->px + c) * 3 + i] =
            (unsigned short)(300 + v * 4700 + 0.5f);
      }
  for(l = 0; l < s->cal_lines; l++)
    for(c = 0; c < s->px * 3; c++)
      s->cal[(size_t)l * s->px * 3 + c] = l < 200 ? 300 : 5000;
  free(t);
}

void synth_window_free(synth_win_scan *s) {
  free(s->raw);
  free(s->cal);
  s->raw = s->cal = NULL;
}

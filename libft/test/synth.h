/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Synthetic flat-fielded previews with known frames (detection tests). */
#ifndef SYNTH_H
#define SYNTH_H
#include "ft.h"

#define SYNTH_MAX 32

typedef struct {
  int px, lines;
  double y_mm_per_line, x_mm_per_px;
  double x_mm0; /* mm at pixel 0 (default 0) */
  ft_film film;
  double film_y0, film_y1; /* film along, mm (holder metal outside) */
  double film_x0, film_x1; /* film edges across, mm */
  double img_x0, img_x1;   /* image area across, mm */
  double tilt_deg;
  int nframes;
  struct {
    double y0, len;
    int blank;
  } fr[SYNTH_MAX];
  int npatch; /* base-like rectangles inside frames */
  struct {
    double y0, y1, x0, x1;
  } patch[4];
  int blob; /* base-like circle */
  double blob_y, blob_x, blob_r;
  unsigned seed;
  /* realistic degradations (all off after synth_default) */
  float base[3];             /* film base transmittance */
  double blur_mm;            /* separable box blur radius */
  double grain;              /* multiplicative Gaussian, fraction */
  double noise;              /* additive Gaussian sigma */
  double drift;              /* base gain 1 + drift*sin(2 pi y / len) */
  int nspeck;                /* open-beam specks on the film */
  double speck_r;            /* their radius, mm */
  int open_outside;          /* holder outside the film is open */
  double metal, metal_noise; /* holder metal level (0: 0.0005), sigma */
  double win_x0, win_x1;     /* holder window across, mm (metal
                                outside; both 0: no window) */
  double cut_mm;             /* strip start cut diagonally: open
                                beam over this much along */
  int nopen;                 /* open holder between loose pieces */
  struct {
    double y0, y1;
  } open[4];
  int nleak; /* light leaks: dense rectangles */
  struct {
    double y0, y1, x0, x1;
  } leak[4];
} synth_spec;

void synth_default(synth_spec *s, ft_film film);
void synth_add(synth_spec *s, double y0, double len, int blank);
float *synth_preview(const synth_spec *s); /* caller frees */

/* A window scan as ftpoc writes it for a share job: raw lines with the
 * tri-linear rows offset (aligned row l = R of raw l + 2k, G of l + k, B
 * of l; k = 12 / bin), dark 300 and white 5000 everywhere, 200 + 50
 * calibration lines. Film fills the window; frames and base-like patches
 * at their true places, holder mm. */
typedef struct {
  double ppi;
  int bin;
  double y0_mm, len_mm; /* window along */
  double x0_mm, w_mm;   /* window across (approximately) */
  int nfr;
  double fy0[8], flen[8];
  int npatch;
  struct {
    double y0, y1, x0, x1;
  } patch[4];
  int open; /* no film: open beam everywhere */
} synth_win;

typedef struct {
  unsigned short *raw, *cal; /* NULL: out of memory */
  int lines, px, cal_lines;
  long x, y;     /* first output px, first line (.txt) */
  float base[3]; /* the film base transmittance */
} synth_win_scan;

void synth_window(const synth_win *w, synth_win_scan *s);
void synth_window_free(synth_win_scan *s);
#endif

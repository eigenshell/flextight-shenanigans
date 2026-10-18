/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Streaming frame detection: a flat-fielded preview fed line by line (as
 * it arrives from the scanner), each frame sealed as soon as the gap
 * after it shows. A cheap per-line state machine, separate from
 * ft_detect (the whole-strip pass the box runs): no look back, a small
 * fixed state, the same frames whatever the chunking.
 *
 *   lines    classified on the holder's image band (ft_holder band_*):
 *            no film (open beam or, for negatives, holder metal), gap
 *            (negative: within FT_SDET_NEAR_D of the film base, low
 *            spread; slide: dark) or frame
 *   base     negative: the median of the last ~10 mm of gap lines; the
 *            first flat film line seeds it, a thinner flat line resets it
 *   runs     frame lines between gaps (FT_SDET_GAP_MM of gap lines in a
 *            row end a run). Between two gaps a run of 33..39.5 mm is
 *            full, 15.5..21 mm half, anything else partial; at the film's
 *            start or end (or the strip's) within 1.5 mm of 36 / 18 full
 *            / half, else partial. Shorter than 3 mm: dropped. A run
 *            past 39.5 mm with no gap is cut at 36 mm (the holder grid)
 *   across   each frame's extent from its own lines: the image band grown
 *            outward over exposed columns (>= 30 % of the run's lines
 *            off the base), within the holder's img_* limits
 *   box      the run padded by FT_SDET_PAD_MM along and across; no tilt
 *            correction (the user can crop)
 *
 * Sprocket holes are not used (120 has none). Browser tier: heap, no
 * files. */
#ifndef FT_STREAM_DETECT_H
#define FT_STREAM_DETECT_H
#include "ft.h"
#include "ft_detect.h"
#include "ft_holder.h"

#define FT_SDET_PAD_MM  0.3     /* the box around a run, each side */
#define FT_SDET_NEAR_D  0.10    /* density within the base: a gap pixel */
#define FT_SDET_GAP_MM  0.4     /* gap lines in a row that end a run */
#define FT_SDET_RING    512     /* base samples kept, at most */

typedef struct {
    int index;                  /* 1.., in the order sealed */
    ft_frame_type type;         /* FULL, HALF or PARTIAL */
    int grid;                   /* cut by the holder grid (no gap seen) */
    long l0, l1;                /* the run: lines [l0, l1) */
    double y0, y1;              /* the box along, holder mm */
    double x0, x1;              /* the box across, holder mm */
} ft_sdet_frame;

typedef struct {
    /* set up (ft_sdet_init) */
    ft_axes ax;
    int px;
    ft_film film;
    const ft_holder *h;
    int b0, b1;                 /* band columns [b0, b1) */
    int s0, s1;                 /* columns the image may lie in */
    int gap_lines, grid_lines, cut_lines, ring_cap;
    /* state */
    long line;                  /* lines fed */
    int have_base;
    float base[3];              /* density */
    float ring[FT_SDET_RING][3];
    int nring, ring_at;
    int have_dmin;
    float dmin[3];              /* thinnest film density seen (fallback) */
    int after_gap;              /* a gap ended the last run */
    long run_s;                 /* the open run's first line, -1 none */
    int run_trusted;            /* it started at a gap (or grid cut) */
    long gap_s;                 /* the current gap streak */
    int gap_n;
    unsigned short *hist;       /* the open run: px x 3 density bins */
    unsigned short *valid;      /* per column: lines on film */
    float *scratch;             /* the band's densities, 3 x band */
    /* out */
    int nframes;
    ft_sdet_frame f[FT_MAX_FRAMES];
    int grid;                   /* some frame came from the holder grid */
} ft_sdet;

/* px pixels per line (RGB transmittance), ax the preview's axes, h the
 * holder. -1 with e filled when the band misses the line or no memory. */
int ft_sdet_init(ft_sdet *d, const ft_axes *ax, int px, ft_film film,
                 const ft_holder *h, ft_err *e);
void ft_sdet_free(ft_sdet *d);
/* one line (px x 3 floats); the frames it sealed (appended to d->f) */
int ft_sdet_line(ft_sdet *d, const float *line);
/* n lines in a row; the frames they sealed */
int ft_sdet_feed(ft_sdet *d, const float *lines, int n);
/* the strip ended: the open run sealed (0 or 1) */
int ft_sdet_end(ft_sdet *d);
/* the film base (negative) or black (slide) as transmittance; 0 if none
 * seen yet (base untouched) */
int ft_sdet_base(const ft_sdet *d, float base[3]);

#endif

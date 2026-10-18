/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Focus: thin lens on one rail, motor steps. Film at `film`, lens at L,
 * CCD at C: u = film - L, v = L - C + w, 1/u + 1/v = 1/fl, and
 * v/u = p * scale (scale = ppi x bin = CCD px per inch of film; p = CCD
 * pixel pitch in inches). ft_lens_default is this scanner's fit to eight
 * focus brackets (2026-10-12, rms 223 CCD / 114 lens steps).
 */
#ifndef FT_FOCUS_H
#define FT_FOCUS_H
#include <stdio.h>
#include "ft.h"

/* Below ~3000 the thin-lens curve drifts from the true focus on this
 * unit (focus survey 2026-10-15/16, notes/hw/focusmap/RESULTS.md; the
 * same trend on three films): a correction per scale, tandem steps added
 * to both carriers (a film-side focus plane shift), linear between the
 * points and held past the ends. Ascending scales; ncorr 0: none. */
#define FT_LENS_CORR_MAX 16
typedef struct {
    double film, w, fl, p;
    int ncorr;
    double corr_scale[FT_LENS_CORR_MAX];
    double corr_off[FT_LENS_CORR_MAX];
} ft_lens;
extern const ft_lens ft_lens_default;
/* l's correction at scale (0 without a table) */
double ft_lens_correction(const ft_lens *l, double scale);

/* The fit spans 3807..6230; the scanner's own focus table (FP<serial>.txt)
 * sits within ~2400 tandem steps of the curve down to scale ~1475
 * (m 0.51), so the curve is trusted to 1500 (2026-10-13). */
#define FT_SCALE_MIN     1500.0
#define FT_SCALE_MAX     6300.0
/* Focus offsets move the CCD and the lens together (tandem, as the manual
 * says): the gap and so the magnification stay, only the film-side focus
 * plane moves. Moving the CCD alone also changes the magnification (~1 %
 * for -9000 steps at 6300) and is ~m^2 (~5x) less sensitive. Hardware
 * 2026-10-12, 6300 ppi: the tandem curve is 4-5x narrower than CCD-only. */
#define FT_FOCUS_MARGIN  300L       /* tandem steps: ~5 % sharpness loss */
#define FT_BRACKET_MAX   4000L      /* tandem: never further from the curve */
/* calibrate-focus brackets the CCD alone (lens on the curve) */
#define FT_CAL_MARGIN    1200L      /* CCD steps: ~5 % sharpness loss */
#define FT_CAL_BRACKET_MAX 12000L
/* every value FlexColor sent, except: the lens may go up to 530000, the
 * scanner's own focus-table minimum for film (FP<serial>.txt entries 6/7,
 * m 0.51; the gap rule below keeps the carriers apart), and the CCD may
 * go to 240000, ~14k steps under FlexColor's 254123: at 3150 ppi negative
 * focus was still rising at 254270 (hardware 2026-10-12). Both counters grow towards the film below; a
 * lower CCD value moves the CCD up, away from the lens. Stepped to 241813
 * with steady motor times (user approved). */
#define FT_CCD_MIN    240000L
#define FT_CCD_MAX    1020000L
#define FT_OPTIC_MIN  530000L
#define FT_OPTIC_MAX  1021000L
/* Carrier gap (lens - CCD, steps). Collisions depend on the gap alone:
 * both counters share one scale. FlexColor's maintenance "bottom" parks
 * them at 1020000 / 1021000 (gap 1000, the smallest proven safe; it sets
 * FT_CCD_MAX/FT_OPTIC_MAX); a crossed pair (1020000 / 997296, gap -22704)
 * lost the white batch and the carriage never moved. The scanner's own
 * focus table (FP<serial>.txt) never goes below a 160231 gap, so scans
 * keep FT_GAP_MIN (user, 2026-10-13). */
#define FT_GAP_BOTTOM 1000L
#define FT_GAP_MIN    150000L

/* The 95 % depth of field (tandem steps across which sharpness stays
 * >= 95 % of its peak) at an optical scale (ppi x bin) and bin: ~K
 * (1+m)/m^2 x bin, m = p x scale (bin < 1 counts as 1). A fit to the
 * focus survey 2026-10-15/16 (notes/hw/focusmap/RESULTS.md 4: measured
 * 636 at 6300, 3651 at 2000, 5653 at 1500, bin 1; within ~10 %). */
#define FT_DOF95_K 980.0
double ft_lens_dof95(const ft_lens *l, double scale, int bin);

int ft_bin_for_ppi(double ppi);          /* -1 if ppi > 6300 */
int ft_ppi_supported(double ppi);        /* 1 = crop can scan it in focus */
/* 0 ok; -1 (outputs not written) for scale outside range / NaN or an
 * implausible lens or positions that overflow */
int ft_optics_for(const ft_lens *l, double scale, long *ccd, long *optic);
int ft_positions_ok(long ccd, long optic);
/* the curve's pair for scale with focus offset off on both (tandem);
 * same returns as ft_optics_for */
int ft_optics_focus(const ft_lens *l, double scale, long off, long *ccd,
                    long *optic);

/* sharpness of a w x h single-channel linear image: rms Sobel gradient
 * of the 3x3-box-denoised image over its mean (~relative step per pixel;
 * gain-free). 0 when w or h < 5 or the mean is 0; NaN out of memory. */
double ft_sharpness(const float *g, int w, int h);
/* x must be ascending; -1 also if any y is not finite.
 * parabola peak through the maximum and its neighbours: 0 ok, 1 maximum
 * at an end (*peak = that x), -1 fewer than 3 points */
int ft_peak(const long *x, const double *y, int n, double *peak);
/* offsets are averaged in double; per-frame if any is > margin from mean */
ft_focus_mode ft_focus_decide(const long off[3], long *shift);

/* focus.txt ("key = number" lines: film, w, fl, p; '#' comments; any
 * number of "correct = <scale> <offset>" lines, scales ascending) from
 * text[0..n) or a stream; -1 with e filled and *l untouched on error */
int ft_lens_parse(ft_lens *l, const char *text, size_t n, ft_err *e);
int ft_lens_read(ft_lens *l, FILE *f, ft_err *e);
int ft_lens_write(const ft_lens *l, FILE *f);
/* least-squares fit of l (start value in, result out) to n >= 4 in-focus
 * points (scale M[i], CCD C[i], lens L[i]); rms residuals out. The fit
 * has no correction table: it is fitted to the points as they are. */
int ft_lens_fit(const double *M, const long *C, const long *L, int n,
                ft_lens *l, double *rms_ccd, double *rms_lens);

#endif

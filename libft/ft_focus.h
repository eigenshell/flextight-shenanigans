/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Focus: a thin lens on one rail, positions in motor steps.
 *
 * The film is at film_steps, the lens at L and the CCD at C:
 *   u = film_steps - L          (object distance)
 *   v = L - C + gap_offset_steps (image distance)
 *   1/u + 1/v = 1/focal_length_steps
 *   v/u = m = pixel_pitch_inches x scale
 * where scale = ppi x bin = CCD pixels per inch of film.
 *
 * ft_lens_default is this scanner's fit to eight focus brackets
 * (2026-10-12, rms 223 CCD / 114 lens steps). */
#ifndef FT_FOCUS_H
#define FT_FOCUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ft.h"

/* the most points a lens correction table holds */
#define FT_LENS_CORRECTION_MAX 16

/* The focus curve of one scanner. */
typedef struct ft_lens {
  /* the film's position on the rail, in motor steps */
  double film_steps;
  /* added to lens - CCD to give the image distance, in motor steps */
  double gap_offset_steps;
  /* the lens's focal length, in motor steps */
  double focal_length_steps;
  /* the size of one CCD pixel, in inches */
  double pixel_pitch_inches;
  /* points in the correction table; 0 = no correction.
   *
   * Below ~3000 the thin-lens curve drifts from the true focus on this
   * unit (focus survey 2026-10-15/16, notes/hw/focusmap/RESULTS.md; the
   * same trend on three films). The correction adds tandem steps to
   * both carriers (a film-side focus plane shift) per scale, linear
   * between the points and held flat past the ends. */
  uint32_t correction_count;
  /* the scales (ppi x bin) of the points, ascending */
  double correction_scale_ppi[FT_LENS_CORRECTION_MAX];
  /* the tandem steps added at each point */
  double correction_offset_steps[FT_LENS_CORRECTION_MAX];
} ft_lens;

/* this scanner's focus curve */
extern const ft_lens ft_lens_default;

/* The lens's correction at scale_ppi, in tandem steps; 0 without a
 * table. */
double ft_get_lens_correction(const ft_lens* lens, double scale_ppi);

/* The fit spans 3807..6230; the scanner's own focus table (FP<serial>.txt)
 * sits within ~2400 tandem steps of the curve down to scale ~1475
 * (m 0.51), so the curve is trusted to 1500 (2026-10-13). */
#define FT_SCALE_MIN_PPI 1500.0
#define FT_SCALE_MAX_PPI 6300.0
/* The lowest optics the scanner reaches at all: its focus table's A4
 * reflective pair (FP entries 4/18, m 0.342, scale ~984). Only explicit
 * positions (crop -p) go below FT_SCALE_MIN_PPI; the curve is not trusted
 * there (user, 2026-10-15: every zoom down to A4 reflective). */
#define FT_SCALE_REACH_MIN_PPI 980.0

/* Focus offsets move the CCD and the lens together (tandem, as the manual
 * says): the gap and so the magnification stay, only the film-side focus
 * plane moves. Moving the CCD alone also changes the magnification (~1 %
 * for -9000 steps at 6300) and is ~m^2 (~5x) less sensitive. Hardware
 * 2026-10-12, 6300 ppi: the tandem curve is 4-5x narrower than CCD-only. */

/* tandem steps that cost ~5 % sharpness */
#define FT_FOCUS_MARGIN_STEPS 300L
/* a tandem bracket never goes further from the curve */
#define FT_BRACKET_MAX_STEPS 4000L
/* calibrate-focus brackets the CCD alone (lens on the curve): CCD steps
 * that cost ~5 % sharpness, and the furthest it goes */
#define FT_CALIBRATION_MARGIN_STEPS 1200L
#define FT_CALIBRATION_BRACKET_MAX_STEPS 12000L

/* Scan positions stay inside the scanner's own focus table
 * (FP<serial>.txt), scanning entries only (user, 2026-10-15): CCD 56674
 * (entry 4, A4 reflective) .. 466453 (entry 16, 1:1), lens 217000
 * (entry 4) .. 1010000 (user: past the table's 1000000 and FlexColor's
 * own 1004246 for 6300, under the ~1020000 park). Both counters grow
 * towards the film below; a lower CCD value moves the CCD up, away from
 * the lens. Before: CCD 240000..1020000, lens 530000..1021000. */
#define FT_CCD_MIN_STEPS 56674L
#define FT_CCD_MAX_STEPS 466453L
#define FT_LENS_MIN_STEPS 217000L
#define FT_LENS_MAX_STEPS 1010000L

/* Carrier gap (lens - CCD, steps). Collisions depend on the gap alone:
 * both counters share one scale. FlexColor's maintenance "bottom" parks
 * them at 1020000 / 1021000 (gap 1000, the smallest proven safe; parking,
 * not a scan position); a crossed pair (1020000 / 997296, gap -22704)
 * lost the white batch and the carriage never moved. The scanner's own
 * focus table (FP<serial>.txt) never goes below a 160231 gap, so scans
 * keep FT_GAP_MIN_STEPS (user, 2026-10-13). */
#define FT_GAP_BOTTOM_STEPS 1000L
#define FT_GAP_MIN_STEPS 150000L

/* The 95 % depth of field: the tandem steps across which sharpness stays
 * >= 95 % of its peak. ~K (1+m)/m^2 x bin, with m = pixel pitch x scale.
 * A fit to the focus survey 2026-10-15/16 (notes/hw/focusmap/RESULTS.md
 * 4: measured 636 at 6300, 3651 at 2000, 5653 at 1500, bin 1; within
 * ~10 %). */
#define FT_DEPTH_OF_FIELD95_K 980.0

/* The 95 % depth of field in tandem steps at scale_ppi (ppi x bin) and
 * bin. A bin below 1 counts as 1. */
double ft_estimate_depth_of_field95(const ft_lens* lens, double scale_ppi,
                                    int32_t bin);

/* The largest bin whose scale ppi x bin stays at or under 6300.
 * Returns the bin, or -1 when ppi is over 6300. */
int32_t ft_get_bin_for_ppi(double ppi);

/* Can crop scan ppi in focus? */
bool ft_is_ppi_supported(double ppi);

/* The CCD and lens positions that focus the curve at scale_ppi.
 *
 * Returns 0 and fills both, or -1 (both untouched) when scale_ppi is out
 * of range or NaN, the lens is implausible, or a position overflows. */
int32_t ft_find_optics_positions(const ft_lens* lens, double scale_ppi,
                                 int64_t* ccd_steps, int64_t* lens_steps);

/* The same as ft_find_optics_positions, then offset_steps added to both
 * (tandem). The same results. */
int32_t ft_find_focused_optics_positions(const ft_lens* lens, double scale_ppi,
                                         int64_t offset_steps,
                                         int64_t* ccd_steps,
                                         int64_t* lens_steps);

/* Are these positions inside the scanner's safe scan range (both
 * limits and the minimum gap)? */
bool ft_are_positions_safe(int64_t ccd_steps, int64_t lens_steps);

/* The sharpness of a single-channel linear image: the rms Sobel gradient
 * of the 3x3-box-denoised image over its mean (~relative step per pixel;
 * independent of gain).
 *
 * image   height rows of width values
 *
 * Returns the sharpness; 0 when width or height is under 5 or the mean
 * is 0; NaN when memory runs out. */
double ft_measure_sharpness(const float* image, uint32_t width,
                            uint32_t height);

/* What ft_find_peak found. The values match its old int results. */
typedef enum ft_peak_result {
  /* the maximum is at an end: *peak is that x */
  FT_PEAK_AT_END = 1,
  /* a parabola through the maximum and its neighbours: *peak is its top */
  FT_PEAK_FOUND = 0,
  /* fewer than 3 points, or a y that is not finite: *peak untouched */
  FT_PEAK_NONE = -1
} ft_peak_result;

/* Find the peak of y over x.
 *
 * x      ascending positions
 * y      the values at them
 * count  points in x and y */
ft_peak_result ft_find_peak(const int64_t* x, const double* y, uint32_t count,
                            double* peak);

/* Decide between one shifted focus and focus per frame from three
 * measured offsets.
 *
 * offsets  the focus offsets of three frames, tandem steps
 * shift    receives their mean, rounded
 *
 * Returns FT_FOCUS_PER_FRAME when any offset is more than
 * FT_FOCUS_MARGIN_STEPS from the mean, else FT_FOCUS_SHIFTED. */
ft_focus_mode ft_decide_focus_mode(const int64_t offsets[3], int64_t* shift);

/* Read focus.txt: "key = number" lines for film, w, fl and p; '#'
 * comments; any number of "correct = <scale> <offset>" lines, scales
 * ascending.
 *
 * text         the file's contents
 * text_length  its length in bytes
 *
 * Returns 0 and fills *lens, or -1 with error filled and *lens
 * untouched. */
int32_t ft_parse_lens(ft_lens* lens, const char* text, size_t text_length,
                      ft_err* error);

/* The same as ft_parse_lens, reading the whole stream. */
int32_t ft_read_lens(ft_lens* lens, FILE* file, ft_err* error);

/* Write lens to a stream in the focus.txt format. Returns 0, or -1 on a
 * write error. */
int32_t ft_write_lens(const ft_lens* lens, FILE* file);

/* Least-squares fit of a lens to in-focus points. The fit has no
 * correction table: it is fitted to the points as they are.
 *
 * scale_ppi      the scale of each point
 * ccd_steps      the CCD position of each point
 * lens_steps     the lens position of each point
 * count          points, at least 4
 * lens           the start value in, the fitted curve out
 * rms_ccd_steps  receives the rms CCD residual
 * rms_lens_steps receives the rms lens residual
 *
 * Returns 0, or -1 (all outputs untouched) when the fit fails. */
int32_t ft_fit_lens(const double* scale_ppi, const int64_t* ccd_steps,
                    const int64_t* lens_steps, uint32_t count, ft_lens* lens,
                    double* rms_ccd_steps, double* rms_lens_steps);

#endif

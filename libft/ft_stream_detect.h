/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Streaming frame detection: a flat-fielded preview fed line by line (as
 * it arrives from the scanner), each frame sealed as soon as the gap
 * after it shows. A cheap per-line state machine, separate from
 * ft_detect_frames (the whole-strip pass Sancho runs): no look back, a small
 * fixed state, the same frames whatever the chunking.
 *
 *   lines    classified on the holder's image band (ft_holder band_*_mm):
 *            no film (open beam or, for negatives, holder metal), gap
 *            (negative: within FT_STREAM_DETECT_NEAR_DENSITY of the film base,
 * low spread; slide: dark) or frame base     negative: the median of the last
 * ~10 mm of gap lines; the first flat film line seeds it, a thinner flat line
 * resets it runs     frame lines between gaps (FT_STREAM_DETECT_GAP_MM of gap
 * lines in a row end a run). Between two gaps a run of 33..39.5 mm is
 *            full, 15.5..21 mm half, anything else partial; at the film's
 *            start or end (or the strip's) within 1.5 mm of 36 / 18 full
 *            / half, else partial. Shorter than 3 mm: dropped. A run
 *            past 39.5 mm with no gap is cut at 36 mm (the holder grid).
 *            A short gap (half that many lines) within 3 mm of the
 *            film's start or end is the frame's edge: the film's cut
 *            edge or a thin leader beyond it is not frame
 *   across   each frame's extent from its own lines: the image band grown
 *            outward over exposed columns (>= 30 % of the run's lines
 *            off the base), within the holder's image_* limits
 *   box      the run padded by FT_STREAM_DETECT_PAD_MM along and across; no
 * tilt correction (the user can crop)
 *
 * Sprocket holes are not used (120 has none). Heap only, no
 * files, so it runs in the browser. */
#ifndef FT_STREAM_DETECT_H
#define FT_STREAM_DETECT_H

#include <stdbool.h>
#include <stdint.h>

#include "ft.h"
#include "ft_detect.h"
#include "ft_holder.h"

/* the box around a run, each side */
#define FT_STREAM_DETECT_PAD_MM 0.3
/* density within the base: a gap pixel */
#define FT_STREAM_DETECT_NEAR_DENSITY 0.10
/* gap lines in a row that end a run */
#define FT_STREAM_DETECT_GAP_MM 0.4
/* base samples kept, at most */
#define FT_STREAM_DETECT_RING_SIZE 512

/* One frame the streaming detection sealed. */
typedef struct ft_stream_frame {
  /* 1.., in the order sealed */
  uint32_t index;
  /* FULL, HALF or PARTIAL */
  ft_frame_type type;
  /* cut by the holder grid (no gap seen) */
  bool is_from_grid;
  /* the run: lines [first_line, end_line) */
  int64_t first_line;
  int64_t end_line;
  /* the box along the strip, holder mm */
  double start_mm;
  double end_mm;
  /* the box across the strip, holder mm */
  double across_start_mm;
  double across_end_mm;
} ft_stream_frame;

/* The detector's set-up, state and results. Set up with
 * ft_init_stream_detector; read frames[] and used_grid. */
typedef struct ft_stream_detector {
  /* --- set up --- */
  ft_axes axes;
  /* pixels per line */
  int32_t pixels;
  ft_film film;
  const ft_holder* holder;
  /* the holder's image band: columns [band_start, band_end) */
  int32_t band_start;
  int32_t band_end;
  /* the columns the image may lie in: [search_start, search_end) */
  int32_t search_start;
  int32_t search_end;
  /* gap lines in a row that end a run */
  int32_t gap_lines;
  /* gap lines in a row that make a short gap */
  int32_t weak_lines;
  /* a run longer than this has no gap: the holder grid cuts it */
  int32_t grid_lines;
  /* where the grid cuts, lines from the run's start */
  int32_t cut_lines;
  /* base samples kept */
  int32_t ring_capacity;

  /* --- state --- */
  /* lines fed so far */
  int64_t lines_fed;
  /* base_density is known */
  bool has_base;
  /* the film base's density per channel */
  float base_density[3];
  /* the last base samples, a ring of ring_capacity */
  float ring[FT_STREAM_DETECT_RING_SIZE][3];
  int32_t ring_count;
  int32_t ring_position;
  /* min_density is known */
  bool has_min_density;
  /* the thinnest film density seen (the fallback base) */
  float min_density[3];
  /* a gap ended the last run */
  bool is_after_gap;
  /* the open run's first line; -1 = none */
  int64_t run_start;
  /* the open run started at a gap (or a grid cut) */
  bool is_run_trusted;
  /* the current gap streak: its first line and its length */
  int64_t gap_start;
  int32_t gap_length;
  /* the open run's last short gap; -1 = none */
  int64_t weak_start;
  int64_t weak_end;
  /* the open run: pixels x 3 density histograms */
  uint16_t* histogram;
  /* per column: the open run's lines on film */
  uint16_t* valid_lines;
  /* the band's densities, 3 x band */
  float* scratch;

  /* --- results --- */
  uint32_t frame_count;
  ft_stream_frame frames[FT_MAX_FRAMES];
  /* some frame came from the holder grid */
  bool used_grid;
} ft_stream_detector;

/* Set up a detector.
 *
 * axes    the preview's axes
 * pixels  pixels per line (RGB transmittance)
 * holder  the holder, for its band and limits
 *
 * Returns 0, or -1 with error filled when the band misses the line or
 * memory runs out. */
int32_t ft_init_stream_detector(ft_stream_detector* detector,
                                const ft_axes* axes, uint32_t pixels,
                                ft_film film, const ft_holder* holder,
                                ft_err* error);

/* Free the detector's buffers. */
void ft_free_stream_detector(ft_stream_detector* detector);

/* Feed one line (pixels x 3 floats).
 * Returns how many frames it sealed (appended to frames[]). */
uint32_t ft_detect_stream_line(ft_stream_detector* detector, const float* line);

/* Feed line_count lines in a row. Returns how many frames they sealed. */
uint32_t ft_detect_stream_lines(ft_stream_detector* detector,
                                const float* lines, uint32_t line_count);

/* The strip ended: seal the open run. Returns 0 or 1. */
uint32_t ft_end_stream_detection(ft_stream_detector* detector);

/* The film base (negative) or black (slide) as transmittance.
 * Returns false (base untouched) when none was seen yet. */
bool ft_get_stream_base(const ft_stream_detector* detector, float base[3]);

#endif

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Frame detection on a flat-fielded preview (lines along the strip,
 * pixels across, RGB transmittance 0..1). */
#ifndef FT_DETECT_H
#define FT_DETECT_H

#include <stdbool.h>
#include <stdint.h>

#include "ft.h"
#include "ft_holder.h"
#include "ft_job.h"

/* Where the lines and pixels of an image sit on the holder:
 *   along:  mm = first_line_mm + line * mm_per_line
 *   across: mm = first_pixel_mm + pixel * mm_per_pixel */
typedef struct ft_axes {
  /* holder mm along the strip at the centre of line 0 */
  double first_line_mm;
  /* mm along the strip from one line to the next */
  double mm_per_line;
  /* holder mm across the strip at the centre of pixel 0 */
  double first_pixel_mm;
  /* mm across the strip from one pixel to the next */
  double mm_per_pixel;
} ft_axes;

/* One frame the detection found. */
typedef struct ft_detected_frame {
  /* its number along the strip, from 1 */
  uint32_t index;
  /* whole, half, panorama or partial */
  ft_frame_type type;
  /* along the strip, holder mm */
  double start_mm;
  double end_mm;
  /* across the strip, holder mm: the image's extent */
  double across_start_mm;
  double across_end_mm;
  /* how far the frame is turned, degrees */
  double tilt_degrees;
  /* the detection is unsure about this frame */
  bool is_confidence_low;
} ft_detected_frame;

/* What the detection found on a strip. */
typedef struct ft_layout {
  /* where the film is along the strip, holder mm */
  double film_start_mm;
  double film_end_mm;
  /* the image band across: between the hole rows, holder mm */
  double band_start_mm;
  double band_end_mm;
  /* the outer edges of the hole rows (the scanned part), holder mm */
  double holes_start_mm;
  double holes_end_mm;
  /* the strip has sprocket-hole rows in view */
  bool has_holes;
  /* the film base's transmittance per channel (median along the strip) */
  float base_transmittance[3];
  /* the strip's tilt (from the hole rows, else the frame edges), degrees */
  double tilt_degrees;
  /* frames in frames[] */
  uint32_t frame_count;
  ft_detected_frame frames[FT_MAX_FRAMES];
  /* blank frames dropped between frames */
  uint32_t blank_count;
  /* frame-sized unexposed film at the strip ends: blank frames or
   * leader */
  uint32_t blank_end_count;
  /* the holder grid was used on some stretch */
  bool used_fallback;
} ft_layout;

/* Find the frames on a flat-fielded preview.
 *
 * transmittance  lines x pixels x 3 values, 0..1
 * axes           where the lines and pixels sit on the holder
 * film           negative or slide
 * holder         the holder, for its frame grid
 * layout         receives what was found
 *
 * Returns 0, or -1 with error filled on bad arguments, no film, no film
 * base or no memory. */
int32_t ft_detect_frames(const float* transmittance, uint32_t lines,
                         uint32_t pixels, const ft_axes* axes, ft_film film,
                         const ft_holder* holder, ft_layout* layout,
                         ft_err* error);

/* Turn a layout into a job: one frame per detected frame at ppi, the
 * windows clamped into the holder; with has_sprockets the windows span
 * the hole rows. */
void ft_convert_layout_to_job(const ft_layout* layout, const ft_holder* holder,
                              ft_film film, double ppi, bool has_sprockets,
                              ft_job* job);

#endif

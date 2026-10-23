/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Frame detection on a flat-fielded preview (lines along the strip,
 * px across, RGB transmittance 0..1). */
#ifndef FT_DETECT_H
#define FT_DETECT_H
#include "ft.h"
#include "ft_holder.h"
#include "ft_job.h"

typedef struct {
  double y_mm0, y_mm_per_line; /* along: mm = y_mm0 + line * y_mm_per_line */
  double x_mm0, x_mm_per_px;   /* across: mm = x_mm0 + col * x_mm_per_px */
} ft_axes;

typedef struct {
  int index;
  ft_frame_type type;
  double y0, y1; /* along, holder mm */
  double x0, x1; /* across, image extent */
  double tilt_deg;
  int confidence_low;
} ft_det_frame;

typedef struct {
  double film_y0, film_y1;   /* film along */
  double band_x0, band_x1;   /* between the hole rows */
  double holes_x0, holes_x1; /* outer extent of the hole rows (scanned part) */
  int has_holes;
  float base[3];   /* film base transmittance (median along) */
  double tilt_deg; /* strip tilt (hole rows, else edges) */
  int nframes;
  ft_det_frame f[FT_MAX_FRAMES];
  int nblank;     /* blank frames dropped (between frames) */
  int nblank_end; /* frame-sized unexposed film at the strip
                     ends: blank frames or leader */
  int fallback;   /* holder grid used on some stretch */
} ft_layout;

int ft_detect(
    const float *t, int lines, int px, const ft_axes *ax, ft_film film,
    const ft_holder *h, ft_layout *L, ft_err *e
);
void ft_layout_to_job(
    const ft_layout *L, const ft_holder *h, ft_film film, double ppi,
    int sprockets, ft_job *j
);

#endif

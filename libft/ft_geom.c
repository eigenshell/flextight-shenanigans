/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_geom.h"

#include <stddef.h>
#include <stdint.h>

#define MM_PER_INCH 25.4
/* the scale at which millimetres are defined: 6000 CCD pixels per inch */
#define REFERENCE_SCALE_PPI 6000.0

/* One measured point of the optics correction. */
typedef struct optics_correction {
  /* the nominal scale (ppi x bin) this point was measured at */
  double scale_ppi;
  /* the real scale divided by the nominal one */
  double scale_ratio;
  /* the CCD pixel that really sits on the optical axis */
  double axis_pixel;
} optics_correction;

/* Below the measured range the focus curve's optics land a little off the
 * nominal scale and axis. Hardware 2026-10-13: each scale scanned back to
 * back with the standard preview (the film moves between longer runs),
 * five edges at the strip start, residuals <= 6 um. At 3150 and above the
 * 2026-10-12 model holds as measured. The correction is linear in scale
 * between the points and held flat below the first one. Sorted by scale. */
static const optics_correction CORRECTIONS[] = {
  {.scale_ppi = 1500.0, .scale_ratio = 1491.5 / 1500.0, .axis_pixel = 4132.9},
  {.scale_ppi = 2000.0, .scale_ratio = 1992.4 / 2000.0, .axis_pixel = 4140.5},
  {.scale_ppi = 3150.0, .scale_ratio = 1.0, .axis_pixel = FT_CCD_AXIS_PIXEL},
};
#define CORRECTION_COUNT (sizeof CORRECTIONS / sizeof CORRECTIONS[0])

/* The scale and axis the optics really have at a nominal scale. */
typedef struct effective_optics {
  /* the real scale, ppi x bin */
  double scale_ppi;
  /* the CCD pixel on the real optical axis */
  double axis_pixel;
} effective_optics;

static effective_optics get_effective_optics(double scale_ppi) {
  const optics_correction* first = &CORRECTIONS[0];
  const optics_correction* last = &CORRECTIONS[CORRECTION_COUNT - 1];
  effective_optics optics;

  /* at and above the last point the nominal model holds */
  if(scale_ppi >= last->scale_ppi) {
    optics.scale_ppi = scale_ppi;
    optics.axis_pixel = FT_CCD_AXIS_PIXEL;
    return optics;
  }
  /* at and below the first point the correction is held flat */
  if(scale_ppi <= first->scale_ppi) {
    optics.scale_ppi = scale_ppi * first->scale_ratio;
    optics.axis_pixel = first->axis_pixel;
    return optics;
  }

  /* Find the two points around scale_ppi: below is the last point under
   * it, above the first point at or over it. The checks above make sure
   * both exist. */
  size_t above_index = 1;
  while(scale_ppi > CORRECTIONS[above_index].scale_ppi) above_index++;
  const optics_correction* below = &CORRECTIONS[above_index - 1];
  const optics_correction* above = &CORRECTIONS[above_index];

  /* how far scale_ppi lies from below (0) to above (1) */
  double weight =
    (scale_ppi - below->scale_ppi) / (above->scale_ppi - below->scale_ppi);
  double scale_ratio =
    below->scale_ratio + weight * (above->scale_ratio - below->scale_ratio);
  optics.scale_ppi = scale_ppi * scale_ratio;
  optics.axis_pixel =
    below->axis_pixel + weight * (above->axis_pixel - below->axis_pixel);
  return optics;
}

double ft_get_ccd_pixel_from_mm(double position_mm, double scale_ppi) {
  effective_optics optics = get_effective_optics(scale_ppi);
  /* the CCD pixel at the reference scale ... */
  double reference_pixel =
    position_mm / MM_PER_INCH * REFERENCE_SCALE_PPI + FT_CCD_FIRST_PIXEL;
  /* ... then magnified about the optical axis */
  double distance_from_axis = reference_pixel - optics.axis_pixel;
  return optics.axis_pixel +
         distance_from_axis * optics.scale_ppi / REFERENCE_SCALE_PPI;
}

double ft_get_mm_from_ccd_pixel(double ccd_pixel, double scale_ppi) {
  effective_optics optics = get_effective_optics(scale_ppi);
  /* undo the magnification about the optical axis ... */
  double distance_from_axis = ccd_pixel - optics.axis_pixel;
  double reference_pixel = optics.axis_pixel + distance_from_axis *
                                                 REFERENCE_SCALE_PPI /
                                                 optics.scale_ppi;
  /* ... then the reference scale back to millimetres */
  return (reference_pixel - FT_CCD_FIRST_PIXEL) *
         MM_PER_INCH /
         REFERENCE_SCALE_PPI;
}

double ft_get_ccd_pixel_from_output(double line_start_pixel,
                                    double output_pixel, uint32_t bin) {
  return (line_start_pixel + output_pixel) * bin + FT_CCD_FIRST_PIXEL;
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Holder millimetres <-> CCD pixels.
 *
 * Across the film, millimetres are defined at the reference scale of 6000
 * CCD pixels per inch. Any other scale magnifies about the optical axis
 * (measured 2026-10-12 at 3150..6300). Below 3150 a measured correction
 * for the focus curve's optics applies; see ft_geom.c.
 *
 * "scale" is always ppi x bin: CCD pixels per inch of film (nominal). */
#ifndef FT_GEOM_H
#define FT_GEOM_H

#include <stdint.h>

/* the CCD pixel on the optical axis; every scale magnifies about it */
#define FT_CCD_AXIS_PIXEL 4118.0
/* the CCD pixel under output pixel 0 when the line starts at x = 0 */
#define FT_CCD_FIRST_PIXEL 144.0
/* CCD pixels the scanner reads: the preview's 666 px x bin 12 */
#define FT_CCD_PIXEL_COUNT 7992

/* Convert a position across the film to a CCD pixel.
 *
 * position_mm  millimetres across the holder
 * scale_ppi    the optical scale, ppi x bin
 *
 * Returns the CCD pixel (fractional) that sees position_mm. */
double ft_get_ccd_pixel_from_mm(double position_mm, double scale_ppi);

/* Convert a CCD pixel to a position across the film; the inverse of
 * ft_get_ccd_pixel_from_mm.
 *
 * ccd_pixel  the CCD pixel (fractional)
 * scale_ppi  the optical scale, ppi x bin
 *
 * Returns the position in millimetres across the holder. */
double ft_get_mm_from_ccd_pixel(double ccd_pixel, double scale_ppi);

/* Find the first CCD pixel behind one output pixel.
 *
 * A scan line starts at output pixel line_start_pixel, and each output
 * pixel sums bin CCD pixels.
 *
 * line_start_pixel  the output pixel the line starts at (the window's x)
 * output_pixel      the output pixel within the line, from 0
 * bin               CCD pixels per output pixel
 *
 * Returns the first CCD pixel of that output pixel. */
double ft_get_ccd_pixel_from_output(double line_start_pixel,
                                    double output_pixel, uint32_t bin);

#endif

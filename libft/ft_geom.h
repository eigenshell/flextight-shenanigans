/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Holder millimetres <-> CCD pixels. Across the film, mm are defined at
 * the 6000 px/inch optical scale; other scales magnify about the optical
 * axis (measured 2026-10-12 at 3150..6300; below 3150 a measured
 * correction for the curve's optics applies, see ft_geom.c). scale = ppi x
 * bin = CCD px per inch (nominal). */
#ifndef FT_GEOM_H
#define FT_GEOM_H

#define FT_CCD_AXIS 4118.0 /* CCD pixel on the optical axis */
#define FT_CCD_FIRST 144.0 /* CCD pixel under output pixel 0 at x = 0 */
#define FT_CCD_PIXELS 7992 /* preview: 666 px x bin 12 */

double ft_mm_to_ccd(double mm, double scale);
double ft_ccd_to_mm(double ccd, double scale);
/* first CCD pixel of output pixel c in a line starting at output pixel x */
double ft_out_to_ccd(double x, double c, int bin);

#endif

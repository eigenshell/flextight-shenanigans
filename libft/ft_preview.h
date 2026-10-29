/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Whole-holder previews: fast, for framing and detection. "standard" is
 * FlexColor's captured 525 ppi preview (6300 optics, bin 12) with its
 * CCD/lens on the focus curve (ft_build_standard_preview_descriptor). "o3150"
 * is 525 ppi at the 3150 optics (bin 6): a detailed scan's preview is taken at
 * the scan's own optics, so its light level and Dmax match the scan (6300 scans
 * preview with "standard"). "1000" and "1500" are synthesized at the 6000
 * optics FlexColor's crops use (bin 6 and 4), the fastest way to those
 * resolutions: the stock firmware costs ~84k output px/s for the full line
 * (7992 / bin px) whatever the window, so a larger bin is faster.
 * Lower-magnification (more light, slower) optics are a per-frame choice for
 * the real scans (ft_plan.h). All cover the standard preview's window. Frame
 * detection runs on a copy box-averaged by detection_factor to ~500 ppi, near
 * the 525 ppi it is tuned on. */
#ifndef FT_PREVIEW_H
#define FT_PREVIEW_H

#include <stdint.h>

#include "ft_detect.h"
#include "ft_focus.h"

/* the holder's length in lines at 525 ppi (~232 mm) */
#define FT_PREVIEW_LINES_AT_525 4795L
/* the standard preview's first output pixel across */
#define FT_PREVIEW_FIRST_PIXEL 2
/* the standard preview's output pixels per line */
#define FT_PREVIEW_PIXEL_COUNT 666

/* One preview mode. */
typedef struct ft_preview {
  /* the name the user picks: "standard", "o3150", "1000", "1500" */
  const char* name;
  /* output pixels per inch, along and across */
  double ppi;
  /* CCD pixels per output pixel across (the firmware keeps one) */
  int32_t bin;
  /* the box factor down to the detection grid */
  int32_t detection_factor;
} ft_preview;

/* Find a preview mode: "standard" (or "525"), "o3150", "1000" or "1500".
 * Returns NULL when name is unknown or NULL. */
const ft_preview* ft_find_preview(const char* name);

/* The window every preview scans, in holder mm: across from
 * across_start_mm, width_mm wide (the standard preview's span), and along
 * from 0, length_mm long. */
void ft_get_preview_window(double* across_start_mm, double* width_mm,
                           double* length_mm);

/* The lines a preview reads over the whole holder. */
int64_t ft_get_preview_line_count(const ft_preview* preview);

/* The RGB row offset in lines at the preview's bin: 12 / bin. */
int32_t ft_get_preview_row_offset(const ft_preview* preview);

/* The axes of the detection image of a scan whose first output pixel is
 * first_pixel: box-averaged by detection_factor, then RGB-aligned (row
 * offset / detection_factor lines). */
void ft_get_preview_axes(const ft_preview* preview, int64_t first_pixel,
                         ft_axes* axes);

/* The SET WINDOW descriptor: its length and the byte offsets of the CCD
 * and lens positions (big-endian 32-bit) */
#define FT_DESCRIPTOR_BYTES 84
#define FT_DESCRIPTOR_CCD_OFFSET 56
#define FT_DESCRIPTOR_LENS_OFFSET 60

/* Build the standard preview's SET WINDOW descriptor.
 *
 * FlexColor's own CCD/lens pair sits ~10000 CCD steps short of the focus
 * peak, so this copies template (FlexColor's captured 525 ppi window,
 * FT_DESCRIPTOR_BYTES) to descriptor and moves only the CCD and lens
 * positions to the focus curve at the 6300 optics, offset 0
 * (ft_find_optics_positions, as crops and the synthesized previews).
 *
 * ccd_steps, lens_steps  receive the positions written
 *
 * Returns 0, or -1 (descriptor untouched) when there is no focus at
 * 6300. The caller checks the positions (ft_are_positions_safe). */
int32_t ft_build_standard_preview_descriptor(const uint8_t* template,
                                             const ft_lens* lens,
                                             uint8_t* descriptor,
                                             int64_t* ccd_steps,
                                             int64_t* lens_steps);

/* Box-average an RGB image factor x factor.
 *
 * image    lines x pixels x 3 values
 * average  receives (lines / factor) x (pixels / factor) x 3 values;
 *          leftover lines and pixels are dropped */
void ft_box_average(const float* image, uint32_t lines, uint32_t pixels,
                    uint32_t factor, float* average);

#endif

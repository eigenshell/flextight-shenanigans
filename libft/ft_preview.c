/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_preview.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ft_endian.h"
#include "ft_geom.h"

#define MM_PER_INCH 25.4
/* the standard preview reads at 525 ppi, bin 12: the 6300 optics */
#define STANDARD_PPI 525.0
#define STANDARD_BIN 12
#define STANDARD_SCALE_PPI 6300

static const ft_preview MODES[] = {
  {.name = "standard", .ppi = 525.0, .bin = 12, .detection_factor = 1},
  {.name = "o3150", .ppi = 525.0, .bin = 6, .detection_factor = 1},
  {.name = "1000", .ppi = 1000.0, .bin = 6, .detection_factor = 2},
  {.name = "1500", .ppi = 1500.0, .bin = 4, .detection_factor = 3},
};
#define MODE_COUNT (sizeof MODES / sizeof MODES[0])

const ft_preview* ft_find_preview(const char* name) {
  if(name == NULL) return NULL;
  /* "525" is another name for the standard preview */
  if(strcmp(name, "525") == 0) return &MODES[0];
  for(size_t index = 0; index < MODE_COUNT; index++) {
    if(strcmp(MODES[index].name, name) == 0) return &MODES[index];
  }
  return NULL;
}

void ft_get_preview_window(double* across_start_mm, double* width_mm,
                           double* length_mm) {
  double first_ccd_pixel =
    ft_get_ccd_pixel_from_output(FT_PREVIEW_FIRST_PIXEL, 0, STANDARD_BIN);
  double end_ccd_pixel = ft_get_ccd_pixel_from_output(
    FT_PREVIEW_FIRST_PIXEL, FT_PREVIEW_PIXEL_COUNT, STANDARD_BIN);
  double start_mm =
    ft_get_mm_from_ccd_pixel(first_ccd_pixel, STANDARD_SCALE_PPI);
  double end_mm = ft_get_mm_from_ccd_pixel(end_ccd_pixel, STANDARD_SCALE_PPI);
  *across_start_mm = start_mm;
  *width_mm = end_mm - start_mm;
  *length_mm = FT_PREVIEW_LINES_AT_525 * MM_PER_INCH / STANDARD_PPI;
}

int64_t ft_get_preview_line_count(const ft_preview* preview) {
  /* the same rounding as Sancho's holder-end check */
  return (
    int64_t)((double)FT_PREVIEW_LINES_AT_525 * preview->ppi / STANDARD_PPI);
}

int32_t ft_get_preview_row_offset(const ft_preview* preview) {
  return FT_CCD_ROW_SPACING_LINES / preview->bin;
}

void ft_get_preview_axes(const ft_preview* preview, int64_t first_pixel,
                         ft_axes* axes) {
  int32_t factor = preview->detection_factor;
  double scale_ppi = preview->ppi * preview->bin;
  /* the centre of a detection pixel: factor output pixels of bin CCD
   * pixels */
  double box_centre = (factor * preview->bin - 1) / 2.0;
  axes->mm_per_line = MM_PER_INCH / preview->ppi * factor;
  /* integer division: whole detection lines lost to the alignment */
  axes->first_line_mm =
    (ft_get_preview_row_offset(preview) / factor) * axes->mm_per_line;
  double first_ccd_pixel =
    ft_get_ccd_pixel_from_output(first_pixel, 0, preview->bin) + box_centre;
  double next_ccd_pixel =
    ft_get_ccd_pixel_from_output(first_pixel, factor, preview->bin) +
    box_centre;
  axes->first_pixel_mm = ft_get_mm_from_ccd_pixel(first_ccd_pixel, scale_ppi);
  axes->mm_per_pixel =
    ft_get_mm_from_ccd_pixel(next_ccd_pixel, scale_ppi) - axes->first_pixel_mm;
}

int32_t ft_build_standard_preview_descriptor(const uint8_t* template,
                                             const ft_lens* lens,
                                             uint8_t* descriptor,
                                             int64_t* ccd_steps,
                                             int64_t* lens_steps) {
  const ft_preview* standard = &MODES[0];
  int64_t ccd;
  int64_t lens_position;
  if(ft_find_optics_positions(lens, standard->ppi * standard->bin, &ccd,
                              &lens_position) != 0) {
    return -1;
  }
  memcpy(descriptor, template, FT_DESCRIPTOR_BYTES);
  /* safe: the focus curve keeps both positions far below 2^32 */
  ft_write_big_endian32(descriptor, FT_DESCRIPTOR_CCD_OFFSET, (uint32_t)ccd);
  ft_write_big_endian32(descriptor, FT_DESCRIPTOR_LENS_OFFSET,
                        (uint32_t)lens_position);
  *ccd_steps = ccd;
  *lens_steps = lens_position;
  return 0;
}

void ft_box_average(const float* image, uint32_t lines, uint32_t pixels,
                    uint32_t factor, float* average) {
  size_t average_lines = lines / factor;
  size_t average_pixels = pixels / factor;
  float scale = 1.0f / (factor * factor);
  for(size_t line = 0; line < average_lines; line++) {
    for(size_t pixel = 0; pixel < average_pixels; pixel++) {
      for(size_t channel = 0; channel < 3; channel++) {
        float sum = 0;
        for(size_t row = 0; row < factor; row++) {
          for(size_t column = 0; column < factor; column++) {
            size_t source_line = line * factor + row;
            size_t source_pixel = pixel * factor + column;
            sum += image[(source_line * pixels + source_pixel) * 3 + channel];
          }
        }
        average[(line * average_pixels + pixel) * 3 + channel] = sum * scale;
      }
    }
  }
}

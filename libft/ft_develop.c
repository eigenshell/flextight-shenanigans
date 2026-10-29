/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_develop.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ft_calib.h"
#include "ft_geom.h"

/* the highest CCD pixel number a defect map may name */
#define MAX_CCD_PIXEL 65535
/* the largest bin */
#define MAX_BIN 12

int32_t ft_parse_defects(const char* text, size_t text_length,
                         ft_defects* defects, ft_err* error) {
  size_t position = 0;
  uint32_t line_number = 0;
  defects->count = 0;
  while(position < text_length) {
    /* copy one line; a longer one is cut at 127 characters */
    char line[128];
    size_t line_length = 0;
    while(position < text_length && text[position] != '\n') {
      if(line_length < sizeof line - 1) {
        line[line_length] = text[position];
        line_length++;
      }
      position++;
    }
    position++;
    line_number++;
    line[line_length] = '\0';

    char* hash = strchr(line, '#');
    if(hash != NULL) *hash = '\0';
    char* cursor = line;
    while(isspace((unsigned char)*cursor)) cursor++;
    if(*cursor == '\0') continue;

    char* end;
    long ccd_pixel = strtol(cursor, &end, 10);
    if(end == cursor || ccd_pixel < 0 || ccd_pixel > MAX_CCD_PIXEL) {
      return ft_fail(error, "defects line %u: expected a CCD pixel",
                     (unsigned)line_number);
    }
    uint8_t channels = 0;
    for(cursor = end; *cursor != '\0'; cursor++) {
      int letter = tolower((unsigned char)*cursor);
      if(letter == 'r') {
        channels |= FT_DEFECT_RED;
      } else if(letter == 'g') {
        channels |= FT_DEFECT_GREEN;
      } else if(letter == 'b') {
        channels |= FT_DEFECT_BLUE;
      } else if(isspace(letter) == 0 && letter != ',') {
        return ft_fail(error, "defects line %u: channel '%c' is not r, g or b",
                       (unsigned)line_number, *cursor);
      }
    }
    if(defects->count == FT_DEFECT_MAX) {
      return ft_fail(error, "defects: more than %d entries", FT_DEFECT_MAX);
    }
    defects->ccd_pixel[defects->count] = (int32_t)ccd_pixel;
    defects->channels[defects->count] =
      channels != 0 ? channels : FT_DEFECT_ALL;
    defects->count++;
  }
  return 0;
}

int32_t ft_repair_defects(float* transmittance, uint32_t lines, uint32_t pixels,
                          const char* faulty_dump, size_t faulty_dump_length,
                          const ft_defects* defects, int32_t first_pixel,
                          int32_t bin, ft_err* error) {
  int32_t faulty[3][FT_FAULTY_PIXEL_MAX];
  uint32_t count[3] = {0, 0, 0};
  if(faulty_dump != NULL) {
    ft_parse_faulty_pixels(faulty_dump, faulty_dump_length, faulty, count);
  }
  /* add the user's defects to the scanner's own list */
  for(uint32_t index = 0; defects != NULL && index < defects->count; index++) {
    for(uint32_t channel = 0; channel < 3; channel++) {
      if((defects->channels[index] & (1u << channel)) == 0) continue;
      if(count[channel] == FT_FAULTY_PIXEL_MAX) {
        return ft_fail(error,
                       "defects: more than %d in channel %u with Faul Pixs",
                       FT_FAULTY_PIXEL_MAX, (unsigned)channel);
      }
      faulty[channel][count[channel]] = defects->ccd_pixel[index];
      count[channel]++;
    }
  }
  /* a bin below 1 is refused here: as uint32_t it would become huge */
  if(bin < 1 ||
     ft_repair_faulty_pixels(transmittance, lines, pixels, faulty, count,
                             first_pixel, (uint32_t)bin) != 0) {
    return ft_fail(error, "defects: bad bin %d or out of memory", (int)bin);
  }
  return 0;
}

int32_t ft_crop_and_orient(const float* transmittance, uint32_t lines,
                           uint32_t pixels, const ft_develop_options* options,
                           ft_image* image, ft_err* error) {
  /* the whole scan, unless the options give a crop */
  int64_t crop_x = 0;
  int64_t crop_y = 0;
  int64_t crop_width = pixels;
  int64_t crop_height = lines;
  if(options->crop_width > 0) {
    crop_x = options->crop_x;
    crop_y = options->crop_y;
    crop_width = options->crop_width;
    crop_height = options->crop_height;
  }
  bool fits = crop_x >= 0 &&
              crop_y >= 0 &&
              crop_width >= 1 &&
              crop_height >= 1 &&
              crop_x + crop_width <= pixels &&
              crop_y + crop_height <= lines;
  if(fits == false) {
    return ft_fail(error, "crop %d,%d %dx%d outside the %ux%u scan",
                   (int)crop_x, (int)crop_y, (int)crop_width, (int)crop_height,
                   (unsigned)pixels, (unsigned)lines);
  }
  /* turned: the crop's height (scan lines) becomes the image's width */
  if(ft_allocate_image(image, (uint32_t)crop_height, (uint32_t)crop_width,
                       error) != 0) {
    return -1;
  }
  for(size_t row = 0; row < (size_t)crop_width; row++) {
    float* target = image->pixels + row * crop_height * 3;
    for(size_t column = 0; column < (size_t)crop_height; column++) {
      size_t scan_line = crop_y + column;
      size_t scan_pixel = crop_x + row;
      const float* source =
        transmittance + (scan_line * pixels + scan_pixel) * 3;
      target[column * 3 + 0] = source[0];
      target[column * 3 + 1] = source[1];
      target[column * 3 + 2] = source[2];
    }
  }
  return 0;
}

float* ft_develop_transmittance(const uint16_t* raw, uint32_t lines,
                                uint32_t pixels, const uint16_t* calibration,
                                uint32_t calibration_lines,
                                const char* faulty_dump,
                                size_t faulty_dump_length,
                                const ft_develop_options* options,
                                uint32_t* aligned_lines, ft_err* error) {
  if(lines < 1 || pixels < 1 || options->bin < 1 || options->bin > MAX_BIN) {
    ft_fail(error, "develop: bad geometry (%u lines, %u px, bin %d)",
            (unsigned)lines, (unsigned)pixels, (int)options->bin);
    return NULL;
  }
  bool is_done = false;
  float* transmittance = NULL;
  float* dark = malloc((size_t)pixels * 3 * sizeof *dark);
  float* white = malloc((size_t)pixels * 3 * sizeof *white);
  if(dark != NULL && white != NULL) {
    transmittance = malloc((size_t)lines * pixels * 3 * sizeof *transmittance);
  }
  if(transmittance == NULL) {
    ft_fail(error, "develop: out of memory");
    goto cleanup;
  }
  if(ft_split_calibration(calibration, calibration_lines, pixels, dark, white,
                          error) != 0) {
    goto cleanup;
  }
  ft_apply_flat_field(raw, lines, pixels, dark, white, transmittance);
  if(ft_repair_defects(transmittance, lines, pixels, faulty_dump,
                       faulty_dump_length, options->defects,
                       options->first_pixel, options->bin, error) != 0) {
    goto cleanup;
  }
  *aligned_lines =
    ft_align_rgb_rows(transmittance, lines, pixels,
                      FT_CCD_ROW_SPACING_LINES / (uint32_t)options->bin);
  is_done = true;

cleanup:
  free(dark);
  free(white);
  if(is_done == false) {
    free(transmittance);
    transmittance = NULL;
  }
  return transmittance;
}

int32_t ft_develop(const uint16_t* raw, uint32_t lines, uint32_t pixels,
                   const uint16_t* calibration, uint32_t calibration_lines,
                   const char* faulty_dump, size_t faulty_dump_length,
                   const ft_develop_options* options, ft_image* image,
                   ft_err* error) {
  uint32_t aligned_lines;
  float* transmittance = ft_develop_transmittance(
    raw, lines, pixels, calibration, calibration_lines, faulty_dump,
    faulty_dump_length, options, &aligned_lines, error);
  if(transmittance == NULL) return -1;
  int32_t result = ft_crop_and_orient(transmittance, aligned_lines, pixels,
                                      options, image, error);
  free(transmittance);
  return result;
}

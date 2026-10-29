/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_out.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ft_dng.h"
#include "ft_output.h"
#include "ft_png.h"

/* the quick-look PNG's gamma */
#define PREVIEW_GAMMA 2.2

static const ft_writer WRITERS[] = {
  {.name = "dng",
   .extension = ".dng",
   .description = "linear 16-bit DNG (darktable, RawTherapee, Lightroom)",
   .write = ft_write_dng},
  {.name = "png",
   .extension = ".png",
   .description = "8-bit preview, gamma 2.2, not inverted",
   .write = ft_write_preview_png},
  {.name = "positive",
   .extension = "_positive.png",
   .description =
     "rendered positive, 16-bit sRGB PNG, soft gamut (needs --film)",
   .write = ft_write_positive_png},
  /* the end of the table */
  {.name = NULL, .extension = NULL, .description = NULL, .write = NULL},
};

const ft_writer* ft_get_writers(void) { return WRITERS; }

const ft_writer* ft_find_writer(const char* name) {
  for(const ft_writer* writer = WRITERS; writer->name != NULL; writer++) {
    if(strcmp(writer->name, name) == 0) return writer;
  }
  return NULL;
}

int32_t ft_write_preview_png(const char* path, const ft_image* image,
                             ft_err* error) {
  if(image->pixels == NULL || image->width < 1 || image->height < 1) {
    return ft_fail(error, "png: empty image");
  }
  size_t value_count = (size_t)image->width * image->height * 3;
  uint8_t* bytes = malloc(value_count);
  if(bytes == NULL) return ft_fail(error, "png: out of memory");
  for(size_t index = 0; index < value_count; index++) {
    float value = image->pixels[index];
    if(value <= 0) {
      value = 0;
    } else if(value >= 1) {
      value = 1;
    } else {
      value = (float)pow(value, 1 / PREVIEW_GAMMA);
    }
    /* + 0.5 rounds to the nearest of 0..255 */
    bytes[index] = (uint8_t)(value * 255 + 0.5f);
  }
  int32_t result =
    ft_write_png_rgb8(path, bytes, image->width, image->height, error);
  free(bytes);
  return result;
}

int32_t ft_write_positive_png(const char* path, const ft_image* image,
                              ft_err* error) {
  if(image->pixels == NULL || image->width < 1 || image->height < 1) {
    return ft_fail(error, "positive: empty image");
  }
  size_t pixel_count = (size_t)image->width * image->height;
  size_t row_pixels = image->width;
  int32_t result = -1;
  /* ~330 KB of tables: too big for the stack */
  ft_output* output = malloc(sizeof *output);
  uint16_t* quantised = malloc(pixel_count * 3 * sizeof *quantised);
  float* encoded = malloc(row_pixels * 3 * sizeof *encoded);
  if(output == NULL || quantised == NULL || encoded == NULL) {
    ft_fail(error, "positive: out of memory");
    goto cleanup;
  }
  if(ft_compile_output(FT_SPACE_SRGB, FT_GAMUT_SOFT, output, error) != 0) {
    goto cleanup;
  }
  /* one row at a time through the output stage */
  for(size_t start = 0; start < pixel_count; start += row_pixels) {
    ft_apply_output(output, image->pixels + 3 * start, encoded, row_pixels);
    ft_quantise_output16(encoded, quantised + 3 * start, row_pixels);
  }
  result =
    ft_write_png_rgb16(path, quantised, image->width, image->height, error);

cleanup:
  free(encoded);
  free(quantised);
  free(output);
  return result;
}

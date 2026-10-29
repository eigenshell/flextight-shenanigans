/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_image.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* IEC 61966-2-1 (sRGB), XYZ D65 -> linear sRGB */
static const double SRGB_FROM_XYZ[9] = {
  3.2404542, -1.5371385, -0.4985314, -0.9692660, 1.8760108,
  0.0415560, 0.0556434,  -0.2040259, 1.0572252,
};

void ft_init_image(ft_image* image) {
  memset(image, 0, sizeof *image);
  image->film = -1;
  memcpy(image->xyz_to_rgb, SRGB_FROM_XYZ, sizeof image->xyz_to_rgb);
  strcpy(image->make, "Imacon");
  strcpy(image->model, "Flextight Precision II");
}

int32_t ft_allocate_image(ft_image* image, uint32_t width, uint32_t height,
                          ft_err* error) {
  if(width < 1 || height < 1) {
    return ft_fail(error, "image: bad size %ux%u", (unsigned)width,
                   (unsigned)height);
  }
  /* in double, so the check itself cannot overflow */
  double value_count = (double)width * height * 3;
  if(value_count > (double)(SIZE_MAX / sizeof(float))) {
    return ft_fail(error, "image: %ux%u is too large", (unsigned)width,
                   (unsigned)height);
  }
  image->pixels = calloc((size_t)width * height * 3, sizeof *image->pixels);
  if(image->pixels == NULL) {
    return ft_fail(error, "image: out of memory for %ux%u", (unsigned)width,
                   (unsigned)height);
  }
  image->width = width;
  image->height = height;
  return 0;
}

void ft_free_image(ft_image* image) {
  free(image->pixels);
  image->pixels = NULL;
  image->width = 0;
  image->height = 0;
}

int32_t ft_crop_image(const ft_image* image, const int32_t box[4],
                      ft_image* cut, ft_err* error) {
  int32_t box_x = box[0];
  int32_t box_y = box[1];
  int32_t box_width = box[2];
  int32_t box_height = box[3];
  /* written so that nothing can overflow: compare against the image's
   * size minus the box's, never add */
  int64_t image_width = image->width;
  int64_t image_height = image->height;
  bool fits = box_x >= 0 &&
              box_y >= 0 &&
              box_width >= 1 &&
              box_height >= 1 &&
              box_width <= image_width &&
              box_height <= image_height &&
              box_x <= image_width - box_width &&
              box_y <= image_height - box_height;
  if(fits == false) {
    return ft_fail(error, "box %d,%d %dx%d outside the %ux%u image", (int)box_x,
                   (int)box_y, (int)box_width, (int)box_height,
                   (unsigned)image->width, (unsigned)image->height);
  }
  if(ft_allocate_image(cut, (uint32_t)box_width, (uint32_t)box_height, error) !=
     0) {
    return -1;
  }
  size_t row_bytes = (size_t)box_width * 3 * sizeof *cut->pixels;
  for(size_t row = 0; row < (size_t)box_height; row++) {
    const float* source =
      image->pixels + (((size_t)box_y + row) * image->width + box_x) * 3;
    memcpy(cut->pixels + row * box_width * 3, source, row_bytes);
  }
  cut->ppi = image->ppi;
  return 0;
}

int32_t ft_rotate_image(const ft_image* image, int32_t degrees,
                        ft_image* rotated, ft_err* error) {
  if(degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270) {
    return ft_fail(error, "rotate: %d is not 0, 90, 180 or 270", (int)degrees);
  }
  /* a quarter turn swaps width and height */
  bool is_quarter_turn = degrees == 90 || degrees == 270;
  uint32_t width = is_quarter_turn ? image->height : image->width;
  uint32_t height = is_quarter_turn ? image->width : image->height;
  *rotated = *image;
  rotated->pixels = NULL;
  if(ft_allocate_image(rotated, width, height, error) != 0) return -1;

  /* for every pixel (x, y) of the result, the source pixel it comes
   * from */
  for(uint32_t y = 0; y < height; y++) {
    for(uint32_t x = 0; x < width; x++) {
      uint32_t source_x;
      uint32_t source_y;
      switch(degrees) {
        case 90:
          source_x = y;
          source_y = image->height - 1 - x;
          break;
        case 180:
          source_x = image->width - 1 - x;
          source_y = image->height - 1 - y;
          break;
        case 270:
          source_x = image->width - 1 - y;
          source_y = x;
          break;
        default:
          source_x = x;
          source_y = y;
          break;
      }
      const float* source =
        image->pixels + ((size_t)source_y * image->width + source_x) * 3;
      float* target = rotated->pixels + ((size_t)y * width + x) * 3;
      target[0] = source[0];
      target[1] = source[1];
      target[2] = source[2];
    }
  }
  return 0;
}

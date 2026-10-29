/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A developed image: what every pipeline stage after the scan works on and
 * what every output writer takes. Pixels are linear transmittance: 0 = no
 * light through the film, 1 = the open-beam white of the scan's own
 * calibration. RGB interleaved, rows top to bottom. */
#ifndef FT_IMAGE_H
#define FT_IMAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "ft.h"

/* An image and what is known about it. */
typedef struct ft_image {
  /* pixels per row */
  uint32_t width;
  /* rows */
  uint32_t height;
  /* height rows of width RGB values, 0..1. After ft_render_image they
   * are a positive in linear Rec.2020, no longer transmittance (write
   * the transmittance formats before rendering). */
  float* pixels;
  /* output resolution; 0 = unknown */
  double ppi;
  /* an ft_film, or -1 = unknown */
  int32_t film;
  /* the job's frame number; 0 = none */
  uint32_t frame_number;
  /* XYZ (D65) -> scanner RGB, row-major (the DNG ColorMatrix1). The
   * default treats scanner RGB as linear sRGB until the scanner is
   * profiled from an IT8 target. */
  double xyz_to_rgb[9];
  /* noise_profile is filled in */
  bool has_noise_profile;
  /* the NoiseProfile (ft_noise_model.h): variance_c(T) = S_c T + O_c in
   * these pixels' transmittance units, as S_r O_r S_g O_g S_b O_b */
  double noise_profile[6];
  /* the camera's maker and model, as raw converters show them */
  char make[32];
  char model[32];
  /* free text: scan settings, job, frame */
  char description[256];
} ft_image;

/* Set the metadata defaults, without pixels (pixels NULL, width = height
 * = 0). */
void ft_init_image(ft_image* image);

/* Allocate width x height pixels, all 0.
 * Returns 0, or -1 with error filled when the size is 0, too large, or
 * memory runs out. */
int32_t ft_allocate_image(ft_image* image, uint32_t width, uint32_t height,
                          ft_err* error);

/* Free the pixels. Safe to call on an image without pixels. */
void ft_free_image(ft_image* image);

/* Copy a box of image into a new image.
 *
 * box  x, y, width, height of the box, in pixels
 * cut  receives the box (pixels allocated; ppi kept)
 *
 * Returns 0, or -1 with error filled when the box does not fit. */
int32_t ft_crop_image(const ft_image* image, const int32_t box[4],
                      ft_image* cut, ft_err* error);

/* Turn image clockwise by degrees (0, 90, 180 or 270) into a new image.
 *
 * rotated  receives the result (pixels allocated; metadata copied; width
 *          and height swapped at 90 and 270)
 *
 * Returns 0, or -1 with error filled on another angle or out of
 * memory. */
int32_t ft_rotate_image(const ft_image* image, int32_t degrees,
                        ft_image* rotated, ft_err* error);

#endif

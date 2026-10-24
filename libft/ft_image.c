/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_image.h"

#include <stdlib.h>
#include <string.h>

/* IEC 61966-2-1 (sRGB), XYZ D65 -> linear sRGB */
static const double SRGB_FROM_XYZ[9] = {
  3.2404542, -1.5371385, -0.4985314, -0.9692660, 1.8760108,
  0.0415560, 0.0556434,  -0.2040259, 1.0572252,
};

void ft_image_init(ft_image* im) {
  memset(im, 0, sizeof *im);
  im->film = -1;
  memcpy(im->xyz_to_rgb, SRGB_FROM_XYZ, sizeof im->xyz_to_rgb);
  strcpy(im->make, "Imacon");
  strcpy(im->model, "Flextight Precision II");
}

int ft_image_alloc(ft_image* im, int w, int h, ft_err* e) {
  if (w < 1 || h < 1) return ft_fail(e, "image: bad size %dx%d", w, h);
  if ((double)w * h * 3 > (double)((size_t)-1 / sizeof(float)))
    return ft_fail(e, "image: %dx%d is too large", w, h);
  im->rgb = calloc((size_t)w * h * 3, sizeof *im->rgb);
  if (!im->rgb) return ft_fail(e, "image: out of memory for %dx%d", w, h);
  im->w = w;
  im->h = h;
  return 0;
}

void ft_image_free(ft_image* im) {
  free(im->rgb);
  im->rgb = NULL;
  im->w = im->h = 0;
}

int ft_image_crop(const ft_image* im, const int box[4], ft_image* cut,
                  ft_err* e) {
  int y;
  if (box[0] < 0 ||
      box[1] < 0 ||
      box[2] < 1 ||
      box[3] < 1 ||
      box[2] > im->w ||
      box[3] > im->h ||
      box[0] > im->w - box[2] ||
      box[1] > im->h - box[3])
    return ft_fail(e, "box %d,%d %dx%d outside the %dx%d image", box[0], box[1],
                   box[2], box[3], im->w, im->h);
  if (ft_image_alloc(cut, box[2], box[3], e)) return -1;
  for (y = 0; y < box[3]; y++)
    memcpy(cut->rgb + (size_t)y * box[2] * 3,
           im->rgb + ((size_t)(box[1] + y) * im->w + box[0]) * 3,
           (size_t)box[2] * 3 * sizeof *cut->rgb);
  cut->ppi = im->ppi;
  return 0;
}

int ft_image_rotate(const ft_image* im, int deg, ft_image* out, ft_err* e) {
  int x, y, ow, oh;
  if (deg != 0 && deg != 90 && deg != 180 && deg != 270)
    return ft_fail(e, "rotate: %d is not 0, 90, 180 or 270", deg);
  ow = deg % 180 ? im->h : im->w;
  oh = deg % 180 ? im->w : im->h;
  *out = *im;
  out->rgb = NULL;
  if (ft_image_alloc(out, ow, oh, e)) return -1;
  /* out (x, y) from in (sx, sy) */
  for (y = 0; y < oh; y++)
    for (x = 0; x < ow; x++) {
      int sx = deg == 0     ? x
               : deg == 90  ? y
               : deg == 180 ? im->w - 1 - x
                            : im->w - 1 - y;
      int sy = deg == 0     ? y
               : deg == 90  ? im->h - 1 - x
               : deg == 180 ? im->h - 1 - y
                            : x;
      const float* s = im->rgb + ((size_t)sy * im->w + sx) * 3;
      float* d = out->rgb + ((size_t)y * ow + x) * 3;
      d[0] = s[0];
      d[1] = s[1];
      d[2] = s[2];
    }
  return 0;
}

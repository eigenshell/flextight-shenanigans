/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_out.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ft_dng.h"
#include "ft_output.h"
#include "ft_png.h"

static const ft_writer WRITERS[] = {
  {"dng", ".dng", "linear 16-bit DNG (darktable, RawTherapee, Lightroom)",
   ft_dng_write},
  {"png", ".png", "8-bit preview, gamma 2.2, not inverted",
   ft_png_write_preview},
  {"positive", "_positive.png",
   "rendered positive, 16-bit sRGB PNG, soft gamut (needs --film)",
   ft_png_write_positive},
  {NULL, NULL, NULL, NULL},
};

const ft_writer* ft_writers(void) { return WRITERS; }

const ft_writer* ft_writer_find(const char* name) {
  const ft_writer* w;
  for(w = WRITERS; w->name; w++)
    if(strcmp(w->name, name) == 0) return w;
  return NULL;
}

int ft_png_write_preview(const char* path, const ft_image* im, ft_err* e) {
  size_t n, i;
  unsigned char* rgb;
  int rc;
  if(!im->rgb || im->w < 1 || im->h < 1) return ft_fail(e, "png: empty image");
  n = (size_t)im->w * im->h * 3;
  rgb = malloc(n);
  if(!rgb) return ft_fail(e, "png: out of memory");
  for(i = 0; i < n; i++) {
    float v = im->rgb[i];
    v = v <= 0 ? 0 : v >= 1 ? 1 : (float)pow(v, 1 / 2.2);
    rgb[i] = (unsigned char)(v * 255 + 0.5f);
  }
  rc = ft_png_write_rgb8(path, rgb, im->w, im->h, e);
  free(rgb);
  return rc;
}

int ft_png_write_positive(const char* path, const ft_image* im, ft_err* e) {
  size_t n, i, row;
  unsigned short* o = NULL;
  float* enc = NULL;
  ft_output* out;
  int rc = -1;
  if(!im->rgb || im->w < 1 || im->h < 1)
    return ft_fail(e, "positive: empty image");
  n = (size_t)im->w * im->h;
  row = (size_t)im->w;
  out = malloc(sizeof *out); /* ~330 KB of tables */
  if(out) o = malloc(n * 3 * sizeof *o);
  if(o) enc = malloc(row * 3 * sizeof *enc);
  if(!enc) {
    ft_fail(e, "positive: out of memory");
    goto done;
  }
  if(ft_output_compile(FT_SPACE_SRGB, FT_GAMUT_SOFT, out, e)) goto done;
  for(i = 0; i < n; i += row) {
    ft_output_apply(out, im->rgb + 3 * i, enc, row);
    ft_output_quantise16(enc, o + 3 * i, row);
  }
  rc = ft_png_write_rgb16(path, o, im->w, im->h, e);
done:
  free(enc);
  free(o);
  free(out);
  return rc;
}

#include "ft_draw_frames.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "ft.h"

static void put_px(uint8_t* img, int32_t w, int32_t h, int32_t x, int32_t y,
                   uint8_t r, uint8_t g, uint8_t b) {
  size_t o;
  if(x < 0 || y < 0 || x >= w || y >= h) return;
  o = ((size_t)y * w + (size_t)x) * 3;
  img[o] = r;
  img[o + 1] = g;
  img[o + 2] = b;
}

static long mm_to_idx(double mm, double mm0, double per, int n) {
  double v = floor((mm - mm0) / per + 0.5);
  if(!(v >= 0)) return -1;
  if(v > n) return (long)n + 1;
  return (long)v;
}

int32_t allocate_image()

  int32_t apply_gamma2_2(uint8_t* path, int32_t lines, int32_t width) {
  uint8_t* image;
  int64_t length;
  if(lines < 1 || width < 1) return -1;
  image = malloc((size_t)lines * width * 3);
  if(!image) {
    fprintf(stderr, "%s: out of memory\n", path);
    return -1;
  }
  for(l = 0; l < lines; l++)
    for(c = 0; c < px; c++)
      for(k = 0; k < 3; k++) {
        double v = t[((size_t)l * px + c) * 3 + k];
        v = v > 0 ? pow(v < 1 ? v : 1, 1 / 2.2) : 0;
        img[((size_t)c * lines + l) * 3 + k] = (unsigned char)(v * 255 + 0.5);
      }
}

int32_t write_strip_png(const char* path, const float* t, int lines, int px,
                        const ft_axes* ax, const ft_job* j) {
  unsigned char* img;
  long l, c;
  int i, k, rc;
  ft_err e;
  if(lines < 1 || px < 1) return -1;
  img = malloc((size_t)lines * px * 3);
  if(!img) {
    fprintf(stderr, "%s: out of memory\n", path);
    return -1;
  }
  for(l = 0; l < lines; l++)
    for(c = 0; c < px; c++)
      for(k = 0; k < 3; k++) {
        double v = t[((size_t)l * px + c) * 3 + k];
        v = v > 0 ? pow(v < 1 ? v : 1, 1 / 2.2) : 0;
        img[((size_t)c * lines + l) * 3 + k] = (unsigned char)(v * 255 + 0.5);
      }
  for(i = 0; i < j->nframes; i++) {
    const ft_job_frame* f = &j->f[i];
    long x0 = mm_to_idx(f->y0, ax->y_mm0, ax->y_mm_per_line, lines);
    long x1 = mm_to_idx(f->y0 + f->len, ax->y_mm0, ax->y_mm_per_line, lines);
    long y0 = mm_to_idx(f->x0, ax->x_mm0, ax->x_mm_per_px, px);
    long y1 = mm_to_idx(f->x0 + f->width, ax->x_mm0, ax->x_mm_per_px, px);
    unsigned char r = f->include ? 255 : 128, g = f->confidence_low ? 64 : 255;
    for(l = x0; l <= x1; l++) {
      put_px(img, lines, px, l, y0, r, g, 0);
      put_px(img, lines, px, l, y1, r, g, 0);
    }
    for(c = y0; c <= y1; c++) {
      put_px(img, lines, px, x0, c, r, g, 0);
      put_px(img, lines, px, x1, c, r, g, 0);
    }
    draw_number(img, lines, px, x0 + 6, y0 + 6, f->index);
  }
  rc = ft_png_write_rgb8(path, img, lines, px, &e);
  if(rc) fprintf(stderr, "%s\n", e.msg);
  free(img);
  return rc;
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_preview.h"

#include <string.h>

#include "ft_geom.h"

static const ft_preview MODES[] = {
  {"standard", 525.0,  12, 1},
  {"o3150",    525.0,  6,  1},
  {"1000",     1000.0, 6,  2},
  {"1500",     1500.0, 4,  3},
};

const ft_preview* ft_preview_find(const char* name) {
  size_t i;
  if (!name) return NULL;
  if (strcmp(name, "525") == 0) return &MODES[0];
  for (i = 0; i < sizeof MODES / sizeof MODES[0]; i++)
    if (strcmp(MODES[i].name, name) == 0) return &MODES[i];
  return NULL;
}

void ft_preview_window(double* x0, double* w, double* len) {
  double a = ft_ccd_to_mm(ft_out_to_ccd(FT_PREVIEW_X_OUT, 0, 12), 6300);
  double b =
    ft_ccd_to_mm(ft_out_to_ccd(FT_PREVIEW_X_OUT, FT_PREVIEW_PX, 12), 6300);
  *x0 = a;
  *w = b - a;
  *len = FT_PREVIEW_LINES_525 * 25.4 / 525;
}

long ft_preview_lines(const ft_preview* m) {
  /* the same rounding as ftpoc's holder-end check */
  return (long)((double)FT_PREVIEW_LINES_525 * m->ppi / 525.0);
}

int ft_preview_rgb_k(const ft_preview* m) { return 12 / m->bin; }

void ft_preview_axes(const ft_preview* m, long x_out, ft_axes* ax) {
  int f = m->down;
  double scale = m->ppi * m->bin;
  /* centre of a detection pixel: f output pixels of bin CCD pixels */
  double half = (f * m->bin - 1) / 2.0;
  ax->y_mm_per_line = 25.4 / m->ppi * f;
  ax->y_mm0 = (ft_preview_rgb_k(m) / f) * ax->y_mm_per_line;
  ax->x_mm0 = ft_ccd_to_mm(ft_out_to_ccd(x_out, 0, m->bin) + half, scale);
  ax->x_mm_per_px =
    ft_ccd_to_mm(ft_out_to_ccd(x_out, f, m->bin) + half, scale) - ax->x_mm0;
}

static void put_be32(unsigned char* d, int off, long v) {
  d[off] = (unsigned char)(v >> 24);
  d[off + 1] = (unsigned char)(v >> 16);
  d[off + 2] = (unsigned char)(v >> 8);
  d[off + 3] = (unsigned char)v;
}

int ft_preview_std_desc(const unsigned char* tmpl, const ft_lens* l,
                        unsigned char* out, long* ccd, long* optic) {
  const ft_preview* m = &MODES[0];
  long c, o;
  if (ft_optics_for(l, m->ppi * m->bin, &c, &o)) return -1;
  memcpy(out, tmpl, FT_DESC_LEN);
  put_be32(out, FT_DESC_CCD, c);
  put_be32(out, FT_DESC_OPTIC, o);
  *ccd = c;
  *optic = o;
  return 0;
}

void ft_box_down(const float* in, int lines, int px, int f, float* out) {
  int lo = lines / f, po = px / f, l, c, i, j, k;
  float norm = 1.0f / (f * f);
  for (l = 0; l < lo; l++)
    for (c = 0; c < po; c++)
      for (k = 0; k < 3; k++) {
        float s = 0;
        for (i = 0; i < f; i++)
          for (j = 0; j < f; j++)
            s += in[((size_t)(l * f + i) * px + c * f + j) * 3 + k];
        out[((size_t)l * po + c) * 3 + k] = s * norm;
      }
}

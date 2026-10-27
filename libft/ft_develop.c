/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_develop.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "ft_calib.h"

int ft_defects_parse(const char* text, size_t n, ft_defects* d, ft_err* e) {
  size_t i = 0;
  int ln = 0;
  d->n = 0;
  while(i < n) {
    char line[128], *p, *end;
    size_t k = 0;
    long pix;
    unsigned char ch = 0;
    while(i < n && text[i] != '\n') {
      if(k < sizeof line - 1) line[k++] = text[i];
      i++;
    }
    i++;
    ln++;
    line[k] = 0;
    if((p = strchr(line, '#')) != NULL) *p = 0;
    p = line;
    while(isspace((unsigned char)*p)) p++;
    if(!*p) continue;
    pix = strtol(p, &end, 10);
    if(end == p || pix < 0 || pix > 65535)
      return ft_fail(e, "defects line %d: expected a CCD pixel", ln);
    for(p = end; *p; p++) {
      int c = tolower((unsigned char)*p);
      if(c == 'r')
        ch |= 1;
      else if(c == 'g')
        ch |= 2;
      else if(c == 'b')
        ch |= 4;
      else if(!isspace(c) && c != ',')
        return ft_fail(e,
                       "defects line %d: channel '%c' is not "
                       "r, g or b",
                       ln, *p);
    }
    if(d->n == FT_DEFECT_MAX)
      return ft_fail(e, "defects: more than %d entries", FT_DEFECT_MAX);
    d->ccd[d->n] = (int)pix;
    d->ch[d->n] = ch ? ch : 7;
    d->n++;
  }
  return 0;
}

int ft_fix_defects(float* t, int lines, int px, const char* faul, size_t n,
                   const ft_defects* d, int x, int bin, ft_err* e) {
  int pix[3][FT_FAUL_MAX], count[3] = {0, 0, 0}, i, c;
  if(faul) ft_faul_parse(faul, n, pix, count);
  for(i = 0; d && i < d->n; i++)
    for(c = 0; c < 3; c++) {
      if(!(d->ch[i] & (1 << c))) continue;
      if(count[c] == FT_FAUL_MAX)
        return ft_fail(e,
                       "defects: more than %d in channel %d with "
                       "Faul Pixs",
                       FT_FAUL_MAX, c);
      pix[c][count[c]++] = d->ccd[i];
    }
  if(ft_faul_fix(t, lines, px, pix, count, x, bin))
    return ft_fail(e, "defects: bad bin %d or out of memory", bin);
  return 0;
}

int ft_crop_orient(const float* t, int lines, int px, const ft_develop_opts* o,
                   ft_image* im, ft_err* e) {
  int cx = 0, cy = 0, cw = px, ch = lines, r, c;
  if(o->crop_w > 0) {
    cx = o->crop_x;
    cy = o->crop_y;
    cw = o->crop_w;
    ch = o->crop_h;
  }
  if(cx < 0 || cy < 0 || cw < 1 || ch < 1 || cx + cw > px || cy + ch > lines)
    return ft_fail(e, "crop %d,%d %dx%d outside the %dx%d scan", cx, cy, cw, ch,
                   px, lines);
  if(ft_image_alloc(im, ch, cw, e)) return -1;
  for(r = 0; r < cw; r++) {
    float* dst = im->rgb + (size_t)r * ch * 3;
    for(c = 0; c < ch; c++) {
      const float* src = t + ((size_t)(cy + c) * px + cx + r) * 3;
      dst[c * 3 + 0] = src[0];
      dst[c * 3 + 1] = src[1];
      dst[c * 3 + 2] = src[2];
    }
  }
  return 0;
}

float* ft_develop_t(const unsigned short* raw, int lines, int px,
                    const unsigned short* cal, int cal_lines, const char* faul,
                    size_t faul_n, const ft_develop_opts* o, int* lines_out,
                    ft_err* e) {
  float *dark, *white, *t = NULL;
  int ok = 0;

  if(lines < 1 || px < 1 || o->bin < 1 || o->bin > 12) {
    ft_fail(e, "develop: bad geometry (%d lines, %d px, bin %d)", lines, px,
            o->bin);
    return NULL;
  }
  dark = malloc((size_t)px * 3 * sizeof *dark);
  white = malloc((size_t)px * 3 * sizeof *white);
  if(dark && white) t = malloc((size_t)lines * px * 3 * sizeof *t);
  if(!t) {
    ft_fail(e, "develop: out of memory");
    goto out;
  }
  if(ft_split_cal(cal, cal_lines, px, dark, white, e)) goto out;
  ft_flatfield(raw, lines, px, dark, white, t);
  if(ft_fix_defects(t, lines, px, faul, faul_n, o->defects, o->x, o->bin, e))
    goto out;
  *lines_out = ft_align_rgb(t, lines, px, 12 / o->bin);
  ok = 1;
out:
  free(dark);
  free(white);
  if(!ok) {
    free(t);
    t = NULL;
  }
  return t;
}

int ft_develop(const unsigned short* raw, int lines, int px,
               const unsigned short* cal, int cal_lines, const char* faul,
               size_t faul_n, const ft_develop_opts* o, ft_image* im,
               ft_err* e) {
  int n, rc;
  float* t =
    ft_develop_t(raw, lines, px, cal, cal_lines, faul, faul_n, o, &n, e);
  if(!t) return -1;
  rc = ft_crop_orient(t, n, px, o, im, e);
  free(t);
  return rc;
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_stream.h"

#include <stdlib.h>
#include <string.h>

#include "ft_calib.h"

struct ft_stream {
  int px, k, nring;
  long n; /* raw lines pushed */
  float *dark, *white;
  float* ring; /* nring lines of px x 3 */
  char* faul;  /* copy of the dump (may be NULL) */
  size_t faul_n;
  ft_defects defects;
  int has_defects, x, bin;
};

void ft_stream_close(ft_stream* s) {
  if(!s) return;
  free(s->dark);
  free(s->white);
  free(s->ring);
  free(s->faul);
  free(s);
}

ft_stream* ft_stream_open(int px, const unsigned short* cal, int cal_lines,
                          const char* faul, size_t faul_n,
                          const ft_develop_opts* o, ft_err* e) {
  ft_stream* s;
  size_t line = (size_t)px * 3;

  if(px < 1 || o->bin < 1 || o->bin > 12) {
    ft_fail(e, "stream: bad geometry (%d px, bin %d)", px, o->bin);
    return NULL;
  }
  s = calloc(1, sizeof *s);
  if(!s) {
    ft_fail(e, "stream: out of memory");
    return NULL;
  }
  s->px = px;
  s->k = 12 / o->bin;
  s->nring = 2 * s->k + 1;
  s->x = o->x;
  s->bin = o->bin;
  if(o->defects) {
    s->defects = *o->defects;
    s->has_defects = 1;
  }
  s->dark = malloc(line * sizeof *s->dark);
  s->white = malloc(line * sizeof *s->white);
  s->ring = malloc(line * s->nring * sizeof *s->ring);
  if(faul && faul_n) {
    s->faul = malloc(faul_n);
    if(s->faul) {
      memcpy(s->faul, faul, faul_n);
      s->faul_n = faul_n;
    }
  }
  if(!s->dark || !s->white || !s->ring || (faul && faul_n && !s->faul)) {
    ft_fail(e, "stream: out of memory");
    goto fail;
  }
  if(ft_split_cal(cal, cal_lines, px, s->dark, s->white, e)) goto fail;
  /* the defect lists are checked once here, on a scratch line */
  if(ft_fix_defects(s->ring, 1, px, s->faul, s->faul_n,
                    s->has_defects ? &s->defects : NULL, s->x, s->bin, e))
    goto fail;
  return s;
fail:
  ft_stream_close(s);
  return NULL;
}

int ft_stream_k(const ft_stream* s) { return s->k; }

int ft_stream_push(ft_stream* s, const unsigned short* raw, float* out,
                   ft_err* e) {
  size_t line = (size_t)s->px * 3;
  float *r, *g, *b;
  long n = s->n;
  int c;

  r = s->ring + (size_t)(n % s->nring) * line;
  ft_flatfield(raw, 1, s->px, s->dark, s->white, r);
  if(ft_fix_defects(r, 1, s->px, s->faul, s->faul_n,
                    s->has_defects ? &s->defects : NULL, s->x, s->bin, e))
    return -1;
  s->n++;
  if(n < 2 * s->k) return 0;
  /* ft_align_rgb: line l = R of l + 2k, G of l + k, B of l */
  g = s->ring + (size_t)((n - s->k) % s->nring) * line;
  b = s->ring + (size_t)((n - 2 * s->k) % s->nring) * line;
  for(c = 0; c < s->px; c++) {
    out[c * 3 + 0] = r[c * 3 + 0];
    out[c * 3 + 1] = g[c * 3 + 1];
    out[c * 3 + 2] = b[c * 3 + 2];
  }
  return 1;
}

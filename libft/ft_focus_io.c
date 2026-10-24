/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The focus curve file (focus.txt) through a stream: ft_focus.c (and
 * its ft_lens_parse) also runs in the browser, these two are for the
 * CLIs. */
#include <stdlib.h>
#include <string.h>

#include "ft_focus.h"

int ft_lens_write(const ft_lens* l, FILE* f) {
  int i;
  fprintf(f,
          "# ftpoc focus curve (thin lens, motor steps)\n"
          "film = %.1f\nw = %.1f\nfl = %.1f\np = %.6e\n",
          l->film, l->w, l->fl, l->p);
  for (i = 0; i < l->ncorr && i < FT_LENS_CORR_MAX; i++)
    fprintf(f, "correct = %.0f %.0f\n", l->corr_scale[i], l->corr_off[i]);
  return ferror(f) ? -1 : 0;
}

int ft_lens_read(ft_lens* l, FILE* f, ft_err* e) {
  char *b = NULL, *nb;
  size_t n = 0, cap = 0, got;
  int rc;
  do {
    if (n + 4096 > cap) {
      cap = cap ? cap * 2 : 8192;
      nb = realloc(b, cap);
      if (!nb) {
        free(b);
        return ft_fail(e, "focus file: out of memory");
      }
      b = nb;
    }
    got = fread(b + n, 1, cap - n, f);
    n += got;
  } while (got > 0);
  rc = ft_lens_parse(l, b ? b : "", n, e);
  free(b);
  return rc;
}

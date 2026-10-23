/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ft_jpeg_write: the encoder's buffer into a file (box tier: stdio). */
#include "ft_jpeg.h"
#include <stdio.h>
#include <stdlib.h>

int ft_jpeg_write(
    const char *path, const unsigned char *rgb, int w, int h, int quality,
    double ppi, const unsigned char *icc, size_t icc_n, const char *comment,
    ft_err *e
) {
  unsigned char *b;
  size_t n;
  FILE *f;
  int bad;

  if(ft_jpeg_encode(rgb, w, h, quality, ppi, icc, icc_n, comment, &b, &n, e))
    return -1;
  f = fopen(path, "wb");
  if(!f) {
    free(b);
    return ft_fail(e, "jpeg: cannot write %s", path);
  }
  bad = fwrite(b, 1, n, f) != n;
  if(fclose(f))
    bad = 1;
  free(b);
  if(bad) {
    remove(path);
    return ft_fail(e, "jpeg: write error on %s", path);
  }
  return 0;
}

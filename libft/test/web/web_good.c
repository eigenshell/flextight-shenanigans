/* SPDX-License-Identifier: GPL-2.0-or-later */
/* purity_web.sh's positive sample: heap, formatting into a buffer and
 * libm are fine in the browser */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int web_good(char *buf, size_t n, double v) {
  double *p = malloc(sizeof *p);
  int k;
  if(!p)
    return -1;
  *p = sqrt(v);
  k = snprintf(buf, n, "%g", *p);
  free(p);
  return k;
}

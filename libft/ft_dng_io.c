/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The DNG's file pair (box tier: stdio); the layout is ft_dng.c's. */
#include "ft_dng.h"
#include "ft_tiff_ifd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STRIP_BYTES 262144L

int ft_dng_write(const char *path, const ft_image *im, ft_err *e) {
  ft_ifd d;
  ft_ifd_entry *so;
  unsigned char *p, *buf = NULL;
  unsigned long rps, nstrips, row_bytes, data0, s;
  FILE *f = NULL;
  int rc = -1;
  long r;

  if(!im->rgb || im->w < 1 || im->h < 1)
    return ft_fail(e, "dng: empty image");
  row_bytes = (unsigned long)im->w * 6;
  if((double)row_bytes * im->h > 4.0e9)
    return ft_fail(e, "dng: %dx%d is over 4 GB", im->w, im->h);
  rps = (unsigned long)(STRIP_BYTES / (long)row_bytes);
  if(rps < 1)
    rps = 1;
  if(rps > (unsigned long)im->h)
    rps = (unsigned long)im->h;
  nstrips = ((unsigned long)im->h + rps - 1) / rps;

  memset(&d, 0, sizeof d);
  ft_ifd_common(&d, im, im->w, im->h, 3, 1);
  ft_ifd_add(&d, 273, FT_T_LONG, nstrips); /* StripOffsets: below */
  ft_ifd_long(&d, 278, rps);
  if((p = ft_ifd_add(&d, 279, FT_T_LONG, nstrips)) != NULL)
    for(s = 0; s < nstrips; s++) {
      unsigned long rows =
          s + 1 < nstrips ? rps : (unsigned long)im->h - s * rps;
      ft_le32(p + 4 * s, rows * row_bytes);
    }
  if(im->has_noise && ft_dng_noise_ok(im->noise))
    ft_ifd_doubles(&d, 51041, im->noise, 6); /* NoiseProfile */
  if(d.oom) {
    ft_fail(e, "dng: out of memory");
    goto out;
  }
  data0 = ft_ifd_layout(&d);
  so = ft_ifd_find(&d, 273);
  for(s = 0; s < nstrips; s++)
    ft_le32(so->data + 4 * s, data0 + s * rps * row_bytes);

  f = fopen(path, "wb");
  if(!f) {
    ft_fail(e, "dng: cannot write %s", path);
    goto out;
  }
  buf = malloc(row_bytes);
  if(!buf) {
    ft_fail(e, "dng: out of memory");
    goto out;
  }
  if(ft_ifd_write(&d, f)) {
    ft_fail(e, "dng: write error on %s", path);
    goto out;
  }
  for(r = 0; r < im->h; r++) {
    const float *src = im->rgb + (size_t)r * im->w * 3;
    unsigned long k;
    for(k = 0; k < (unsigned long)im->w * 3; k++) {
      float v = src[k];
      ft_le16(buf + 2 * k, ft_dng_q16(v));
    }
    if(fwrite(buf, 1, row_bytes, f) != row_bytes)
      break;
  }
  rc = r == im->h ? 0 : ft_fail(e, "dng: write error on %s", path);
out:
  if(f && fclose(f) != 0 && rc == 0)
    rc = ft_fail(e, "dng: write error on %s", path);
  free(buf);
  ft_ifd_free(&d);
  return rc;
}

int ft_dng_read(const char *path, ft_image *im, ft_err *e) {
  FILE *f = fopen(path, "rb");
  unsigned char *b = NULL;
  long n;
  int rc = -1;
  if(!f)
    return ft_fail(e, "dng: cannot read %s", path);
  if(fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 &&
     fseek(f, 0, SEEK_SET) == 0 && (b = malloc(n ? (size_t)n : 1)) != NULL &&
     fread(b, 1, (size_t)n, f) == (size_t)n) {
    rc = ft_dng_parse(b, (size_t)n, im, e);
    if(rc && strncmp(e->msg, "dng: ", 5) == 0) {
      char m[sizeof e->msg];
      snprintf(m, sizeof m, "%s", e->msg + 5);
      ft_fail(e, "dng: %s: %s", path, m);
    }
  } else {
    ft_fail(e, "dng: cannot read %s", path);
  }
  free(b);
  fclose(f);
  return rc;
}

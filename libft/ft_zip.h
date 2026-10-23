/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A store-only zip archive in memory (the page's "All": every frame's
 * JPEG and its .txt). Pure (browser tier: heap, no files). No
 * compression (JPEGs do not shrink), no zip64 (an archive stays under
 * 4 GB), every entry dated 1980-01-01 00:00 so the bytes depend only on
 * the contents. */
#ifndef FT_ZIP_H
#define FT_ZIP_H
#include "ft.h"
#include <stddef.h>

typedef struct {
  unsigned char *b, *cd; /* local entries, central directory */
  size_t n, cap, ncd, capcd;
  unsigned count;
} ft_zip;

/* CRC-32 (ISO 3309, as zip and PNG use) of p[0..n) continuing crc (0 to
 * start) */
unsigned long ft_crc32(unsigned long crc, const void *p, size_t n);

void ft_zip_init(ft_zip *z);
/* one file: name (relative, no "..", at most 65535 bytes), n bytes of
 * data. -1 with e filled (the archive unchanged) on a bad name, a full
 * archive or no memory. */
int ft_zip_add(
    ft_zip *z, const char *name, const void *data, size_t n, ft_err *e
);
/* the archive in a malloc'd buffer (*out, *n; the caller frees it); z is
 * freed either way */
int ft_zip_finish(ft_zip *z, unsigned char **out, size_t *n, ft_err *e);
/* drop an unfinished archive */
void ft_zip_free(ft_zip *z);

#endif

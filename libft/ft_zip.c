/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Store-only zip (PKWARE APPNOTE 4.3: local headers, central directory,
 * end record; version 1.0, method 0). */
#include "ft_zip.h"
#include <stdlib.h>
#include <string.h>

#define DOS_DATE 0x21 /* 1980-01-01 */
#define MAX32 0xFFFFFFFFUL

unsigned long ft_crc32(unsigned long crc, const void *p, size_t n) {
  const unsigned char *b = p;
  unsigned long c = crc ^ MAX32;
  int k;
  while(n--) {
    c ^= *b++;
    for(k = 0; k < 8; k++)
      c = c & 1 ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
  }
  return (c ^ MAX32) & MAX32;
}

void ft_zip_init(ft_zip *z) {
  memset(z, 0, sizeof *z);
}

void ft_zip_free(ft_zip *z) {
  free(z->b);
  free(z->cd);
  ft_zip_init(z);
}

/* room for more bytes in *b; -1 if no memory */
static int grow(unsigned char **b, size_t *cap, size_t need) {
  size_t c = *cap ? *cap : 4096;
  unsigned char *nb;
  if(need <= *cap)
    return 0;
  while(c < need)
    c *= 2;
  nb = realloc(*b, c);
  if(!nb)
    return -1;
  *b = nb;
  *cap = c;
  return 0;
}

static unsigned char *le16(unsigned char *p, unsigned long v) {
  p[0] = (unsigned char)v;
  p[1] = (unsigned char)(v >> 8);
  return p + 2;
}

static unsigned char *le32(unsigned char *p, unsigned long v) {
  return le16(le16(p, v & 0xFFFF), (v >> 16) & 0xFFFF);
}

/* the fields local and central headers share, from "version needed" */
static unsigned char *common(
    unsigned char *p, unsigned long crc, size_t n, size_t nl
) {
  p = le16(p, 10); /* version needed: 1.0 */
  p = le16(p, 0);  /* flags */
  p = le16(p, 0);  /* stored */
  p = le16(p, 0);  /* 00:00 */
  p = le16(p, DOS_DATE);
  p = le32(p, crc);
  p = le32(p, (unsigned long)n); /* compressed = uncompressed */
  p = le32(p, (unsigned long)n);
  p = le16(p, (unsigned long)nl);
  return le16(p, 0); /* no extra field */
}

int ft_zip_add(
    ft_zip *z, const char *name, const void *data, size_t n, ft_err *e
) {
  size_t nl = strlen(name);
  unsigned long crc, off = (unsigned long)z->n;
  unsigned char *p;
  if(nl > 65535)
    return ft_fail(e, "zip: name too long");
  if(nl == 0)
    return ft_fail(e, "zip: bad name");
  if(name[0] == '/' || strstr(name, "..") || strchr(name, '\\'))
    return ft_fail(e, "zip: bad name %.60s", name);
  if(z->count == 65535 || n > MAX32 || z->n + 30 + nl + n > MAX32)
    return ft_fail(e, "zip: archive full");
  if(grow(&z->b, &z->cap, z->n + 30 + nl + n) ||
     grow(&z->cd, &z->capcd, z->ncd + 46 + nl))
    return ft_fail(e, "zip: out of memory");
  crc = ft_crc32(0, data, n);
  p = le32(z->b + z->n, 0x04034b50UL);
  p = common(p, crc, n, nl);
  memcpy(p, name, nl);
  if(n)
    memcpy(p + nl, data, n);
  z->n += 30 + nl + n;
  p = le32(z->cd + z->ncd, 0x02014b50UL);
  p = le16(p, 10); /* made by: MS-DOS, 1.0 */
  p = common(p, crc, n, nl);
  p = le16(p, 0); /* comment */
  p = le16(p, 0); /* disk */
  p = le16(p, 0); /* internal attributes */
  p = le32(p, 0); /* external attributes */
  p = le32(p, off);
  memcpy(p, name, nl);
  z->ncd += 46 + nl;
  z->count++;
  return 0;
}

int ft_zip_finish(ft_zip *z, unsigned char **out, size_t *n, ft_err *e) {
  unsigned char *p;
  *out = NULL;
  *n = 0;
  if(z->n + z->ncd + 22 > MAX32) {
    ft_zip_free(z);
    return ft_fail(e, "zip: archive full");
  }
  if(grow(&z->b, &z->cap, z->n + z->ncd + 22)) {
    ft_zip_free(z);
    return ft_fail(e, "zip: out of memory");
  }
  if(z->ncd)
    memcpy(z->b + z->n, z->cd, z->ncd);
  p = le32(z->b + z->n + z->ncd, 0x06054b50UL);
  p = le16(p, 0); /* this disk */
  p = le16(p, 0); /* the directory's disk */
  p = le16(p, z->count);
  p = le16(p, z->count);
  p = le32(p, (unsigned long)z->ncd);
  p = le32(p, (unsigned long)z->n);
  le16(p, 0); /* no comment */
  *out = z->b;
  *n = z->n + z->ncd + 22;
  free(z->cd);
  ft_zip_init(z);
  return 0;
}

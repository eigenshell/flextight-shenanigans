/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_upload.h"
#include <string.h>

#define HDR 10

static void write_buffer_cdb(unsigned char cdb[10], size_t len) {
  memset(cdb, 0, 10);
  cdb[0] = 0x3b;
  cdb[1] = 0x01;
  cdb[6] = (unsigned char)(len >> 16);
  cdb[7] = (unsigned char)(len >> 8);
  cdb[8] = (unsigned char)len;
}

int ft_upload_chunk(
    int i, const unsigned char code[FT_UPLOAD_CHUNK], unsigned char cdb[10],
    unsigned char *buf, size_t cap, ft_err *e
) {
  if(i < 0 || i >= FT_UPLOAD_CHUNKS)
    return ft_fail(e, "upload: chunk %d (0..15)", i);
  if(cap < HDR + FT_UPLOAD_CHUNK)
    return ft_fail(e, "upload: buffer too small");
  memset(buf, 0, HDR);
  buf[0] = 0x01;                      /* write memory */
  buf[1] = (unsigned char)(0x30 + i); /* segment 0x3i00, offset 0 */
  memcpy(buf + HDR, code, FT_UPLOAD_CHUNK);
  write_buffer_cdb(cdb, HDR + FT_UPLOAD_CHUNK);
  return HDR + FT_UPLOAD_CHUNK;
}

int ft_upload_trailer(
    unsigned char cdb[10], unsigned char *buf, size_t cap, ft_err *e
) {
  static const unsigned char run[HDR] = {0x00, 0x30, 0x01, 0x00, 0x0e,
                                         0,    0,    0,    0,    0};
  if(cap < HDR)
    return ft_fail(e, "upload: buffer too small");
  memcpy(buf, run, HDR);
  write_buffer_cdb(cdb, HDR);
  return HDR;
}

int ft_upload_is_loader(const unsigned char *inq, size_t n) {
  return n >= 27 && memcmp(inq + 16, "SCSI Loader", 11) == 0;
}

/* s[0..n) + t into out (FT_UPLOAD_PATH); 0, or -1 if it does not fit */
static int join(char *out, const char *s, size_t n, const char *t) {
  size_t m = strlen(t);
  if(n + m >= FT_UPLOAD_PATH)
    return -1;
  memcpy(out, s, n);
  memcpy(out + n, t, m + 1);
  return 0;
}

int ft_upload_paths(
    const char *given, const char *exe, char out[2][FT_UPLOAD_PATH]
) {
  size_t i, dir = 0;
  int k = 0;
  if(given)
    return given[0] && join(out[0], "", 0, given) == 0 ? 1 : 0;
  for(i = 0; exe && exe[i]; i++)
    if(exe[i] == '\\' || exe[i] == '/')
      dir = i + 1; /* up to and with the separator */
  if(dir && join(out[k], exe, dir, FT_UPLOAD_FILE) == 0)
    k++;
  if(join(out[k], "", 0, FT_UPLOAD_FILE) == 0)
    k++;
  return k;
}

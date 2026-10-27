/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_maint.h"

#include <string.h>

#define CMD_WRITE 0x01
#define FLAG_IO 0x01
#define FLAG_KEEP_PORT 0x04

static void header(unsigned char* b, unsigned w1, unsigned w2,
                   unsigned char flags) {
  memset(b, 0, FT_MAINT_HDR);
  b[0] = CMD_WRITE;
  b[1] = (unsigned char)(w1 >> 8);
  b[2] = (unsigned char)w1;
  b[3] = (unsigned char)(w2 >> 8);
  b[4] = (unsigned char)w2;
  b[9] = flags;
}

static void put_len(unsigned char cdb[10], size_t len) {
  cdb[6] = (unsigned char)(len >> 16);
  cdb[7] = (unsigned char)(len >> 8);
  cdb[8] = (unsigned char)len;
}

static void write_buffer_cdb(unsigned char cdb[10], size_t len) {
  memset(cdb, 0, 10);
  cdb[0] = 0x3b;
  cdb[1] = 0x01;
  put_len(cdb, len);
}

int ft_maint_io(unsigned port, unsigned char value, unsigned char cdb[10],
                unsigned char* buf, size_t cap, ft_err* e) {
  if(port != 0x342 && port != 0x344)
    return ft_fail(e, "maint: I/O port 0x%x not allowed (only 0x342, 0x344)",
                   port);
  if(cap < FT_MAINT_HDR + 1) return ft_fail(e, "maint: buffer too small");
  header(buf, port, 0, FLAG_IO | FLAG_KEEP_PORT);
  buf[FT_MAINT_HDR] = value;
  write_buffer_cdb(cdb, FT_MAINT_HDR + 1);
  return FT_MAINT_HDR + 1;
}

int ft_maint_mem(unsigned seg, unsigned off, const unsigned char* data,
                 size_t n, unsigned char cdb[10], unsigned char* buf,
                 size_t cap, ft_err* e) {
  if(seg != FT_MAINT_TABLE_SEG)
    return ft_fail(e, "maint: segment 0x%x not allowed (only 4000)", seg);
  if(n == 0 || n > FT_MAINT_MAX_DATA)
    return ft_fail(e, "maint: %u bytes (1..%u allowed)", (unsigned)n,
                   (unsigned)FT_MAINT_MAX_DATA);
  if(off < FT_MAINT_TABLE_LO || off + n > FT_MAINT_TABLE_END)
    return ft_fail(e,
                   "maint: 4000:%04x+%u outside the timing table "
                   "4000:7640..76bb",
                   off, (unsigned)n);
  if(cap < FT_MAINT_HDR + n) return ft_fail(e, "maint: buffer too small");
  header(buf, seg, off, 0);
  memcpy(buf + FT_MAINT_HDR, data, n);
  write_buffer_cdb(cdb, FT_MAINT_HDR + n);
  return (int)(FT_MAINT_HDR + n);
}

int ft_maint_peek(unsigned seg, unsigned off, size_t n, unsigned char cdb[10],
                  ft_err* e) {
  if(seg != 0x3000 &&
     seg != 0x4000 &&
     seg != 0xc000 &&
     seg != 0xe000 &&
     seg != 0xf000)
    return ft_fail(e,
                   "maint: peek segment 0x%x not allowed (3000, 4000, "
                   "c000, e000, f000)",
                   seg);
  if(n == 0 || n > FT_MAINT_MAX_PEEK)
    return ft_fail(e, "maint: peek %u bytes (1..%u allowed)", (unsigned)n,
                   (unsigned)FT_MAINT_MAX_PEEK);
  if(off > 0xffffu || off + n > 0x10000u)
    return ft_fail(e, "maint: peek %04x:%04x+%u wraps the segment", seg, off,
                   (unsigned)n);
  if(seg == 0xc000 && off + n > FT_MAINT_LOADER_LEN)
    return ft_fail(e,
                   "maint: peek %04x:%04x+%u beyond the loader ROM "
                   "C000:0000..7FFF",
                   seg, off, (unsigned)n);
  memset(cdb, 0, 10);
  cdb[0] = 0x3c;
  cdb[2] = (unsigned char)(seg >> 8);
  cdb[3] = (unsigned char)seg;
  cdb[4] = (unsigned char)(off >> 8);
  cdb[5] = (unsigned char)off;
  put_len(cdb, n);
  return 0;
}

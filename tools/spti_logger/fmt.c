/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * fmt.c - JSONL record formatter (pure C90, no libc beyond <stddef.h>).
 */
#include "fmt.h"

/* ---- tiny append helpers (no libc) ------------------------------------ */

static char* ft_str(char* p, const char* s) {
  while(*s) *p++ = *s++;
  return p;
}

/* lowercase hex, no leading zeros ("0" for zero) */
static char* ft_hex(char* p, unsigned long v) {
  char tmp[2 * sizeof(unsigned long)];
  int n = 0;
  int i;
  if(v == 0) {
    *p++ = '0';
    return p;
  }
  while(v != 0) {
    unsigned d = (unsigned)(v & 0xFul);
    tmp[n++] = (char)(d < 10 ? ('0' + d) : ('a' + d - 10));
    v >>= 4;
  }
  for(i = n - 1; i >= 0; i--) *p++ = tmp[i];
  return p;
}

static char* ft_dec(char* p, size_t v) {
  char tmp[3 * sizeof(size_t)];
  int n = 0;
  int i;
  if(v == 0) {
    *p++ = '0';
    return p;
  }
  while(v != 0) {
    tmp[n++] = (char)('0' + (int)(v % 10));
    v /= 10;
  }
  for(i = n - 1; i >= 0; i--) *p++ = tmp[i];
  return p;
}

/* n bytes -> 2n lowercase hex chars */
static char* ft_bytes(char* p, const unsigned char* b, size_t n) {
  static const char hd[] = "0123456789abcdef";
  size_t i;
  for(i = 0; i < n; i++) {
    *p++ = hd[b[i] >> 4];
    *p++ = hd[b[i] & 0xF];
  }
  return p;
}

/* ---- public API -------------------------------------------------------- */

int ft_fmt_ioctl_record(char* buf, size_t cap, unsigned long h_device,
                        unsigned long seq, unsigned long tick,
                        unsigned long io_code, size_t in_len,
                        const unsigned char* in_head, size_t in_head_len,
                        int success, size_t out_len,
                        const unsigned char* out_head, size_t out_head_len) {
  /* worst case: 2*8 hex + 3*10 dec + 2*128 head + ~80 punctuation + NUL */
  char tmp[540];
  char* p = tmp;

  if(in_head == (const unsigned char*)0)
    in_head_len = 0;
  else if(in_head_len > FT_MAX_HEAD)
    in_head_len = FT_MAX_HEAD;
  if(out_head == (const unsigned char*)0)
    out_head_len = 0;
  else if(out_head_len > FT_MAX_HEAD)
    out_head_len = FT_MAX_HEAD;

  p = ft_str(p, "{\"h\":\"0x");
  p = ft_hex(p, h_device);
  p = ft_str(p, "\",\"seq\":");
  p = ft_dec(p, (size_t)seq);
  p = ft_str(p, ",\"t\":");
  p = ft_dec(p, (size_t)tick);
  p = ft_str(p, ",\"code\":\"0x");
  p = ft_hex(p, io_code);
  p = ft_str(p, "\",\"in_len\":");
  p = ft_dec(p, in_len);
  p = ft_str(p, ",\"in_head\":\"");
  p = ft_bytes(p, in_head, in_head_len);
  p = ft_str(p, "\",\"ok\":");
  *p++ = success ? '1' : '0';
  p = ft_str(p, ",\"out_len\":");
  p = ft_dec(p, out_len);
  p = ft_str(p, ",\"out_head\":\"");
  p = ft_bytes(p, out_head, out_head_len);
  p = ft_str(p, "\"}");

  {
    size_t len = (size_t)(p - tmp);
    size_t i;
    if(len + 1 > cap) return -1;
    for(i = 0; i < len; i++) buf[i] = tmp[i];
    buf[len] = '\0';
    return (int)len;
  }
}

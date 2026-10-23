/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_fmt.c - native unit test for ft_fmt_ioctl_record.
 *
 * Build & run (Linux / any hosted C90 toolchain):
 *   gcc -Wall -Wextra -O1 -o /tmp/test_fmt test_fmt.c fmt.c && /tmp/test_fmt
 *
 * Unlike fmt.c, this test MAY use libc freely.
 */
#include "fmt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEAD_CAP 128 /* 64 bytes -> 128 hex chars */

static int g_checks = 0;

static void check(int cond, const char *what) {
  g_checks++;
  if(!cond) {
    fprintf(stderr, "FAIL: %s (check #%d)\n", what, g_checks);
    exit(1);
  }
}

/* Extract the value of "field" from a record; copies into out (cap incl NUL).
 * Returns length or -1 if absent. */
static int field(const char *rec, const char *name, char *out, size_t cap) {
  char key[64];
  const char *q;
  size_t len;
  sprintf(key, "\"%s\":\"", name);
  q = strstr(rec, key);
  if(!q)
    return -1;
  q += strlen(key);
  {
    const char *e = strchr(q, '"');
    if(!e)
      return -1;
    len = (size_t)(e - q);
    if(len + 1 > cap)
      return -1;
    memcpy(out, q, len);
    out[len] = '\0';
    return (int)len;
  }
}

/* Same for bare numeric fields ("in_len":36,) */
static int field_num(const char *rec, const char *name, char *out, size_t cap) {
  char key[64];
  const char *q;
  const char *e;
  size_t len;
  sprintf(key, "\"%s\":", name);
  q = strstr(rec, key);
  if(!q)
    return -1;
  q += strlen(key);
  e = q;
  while(*e >= '0' && *e <= '9')
    e++;
  len = (size_t)(e - q);
  if(len + 1 > cap)
    return -1;
  memcpy(out, q, len);
  out[len] = '\0';
  return (int)len;
}

/* decode lowercase hex string back to bytes; returns count or -1 on bad hex */
static int unhex(const char *s, unsigned char *out, int maxout) {
  int n = 0;
  while(s[0] && s[1]) {
    int hi, lo;
    if(n >= maxout)
      return -1;
    hi = s[0] >= 'a' ? s[0] - 'a' + 10 : s[0] - '0';
    lo = s[1] >= 'a' ? s[1] - 'a' + 10 : s[1] - '0';
    if(hi < 0 || hi > 15 || lo < 0 || lo > 15)
      return -1;
    out[n++] = (unsigned char)((hi << 4) | lo);
    s += 2;
  }
  if(s[0] != '\0') /* odd length */
    return -1;
  return n;
}

/* structural check: balanced braces, no spaces, exactly 9 comma-separated
 * fields */
static void check_structure(const char *rec) {
  int braces = 0, commas = 0, i, inq = 0;
  for(i = 0; rec[i]; i++) {
    if(rec[i] == '"')
      inq = !inq;
    if(inq)
      continue;
    if(rec[i] == '{')
      braces++;
    else if(rec[i] == '}')
      braces--;
    else if(rec[i] == ',')
      commas++;
    else if(rec[i] == ' ') /* contract: no spaces */
      check(0, "no spaces in record");
  }
  check(braces == 0, "balanced braces");
  check(commas == 8, "9 top-level fields (8 commas)");
  check(rec[0] == '{' && rec[strlen(rec) - 1] == '}', "single JSON object");
}

int main(void) {
  static char buf[600];
  char val[HEAD_CAP + 1], num[24];
  unsigned char dec[80];
  int n, r;

  /* (i) empty in/out buffers, failure path */
  {
    static const char *expect = "{\"h\":\"0x12\",\"seq\":0,\"t\":0,\"code\":"
                                "\"0x2a002c\",\"in_len\":0,\"in_head\":\"\","
                                "\"ok\":0,\"out_len\":0,\"out_head\":\"\"}";
    r = ft_fmt_ioctl_record(
        buf, sizeof buf, 0x12, 0, 0, 0x2a002c, 0, (const unsigned char *)"", 0,
        0, 0, (const unsigned char *)"", 0
    );
    check(r == (int)strlen(expect), "record length matches expected");
    check(strcmp(buf, expect) == 0, "exact record string (empty buffers, ok=0)");
    check_structure(buf);
  }

  /* (ii) 4-byte input buffer {0x12,0,0,0x24} */
  {
    static const unsigned char cdb[4] = {0x12, 0x00, 0x00, 0x24};
    r = ft_fmt_ioctl_record(
        buf, sizeof buf, 0x12, 0, 0, 0x2a002c, 4, cdb, 4, 1, 36, cdb, 4
    );
    check(r > 0, "record written");
    check_structure(buf);
    n = field_num(buf, "seq", num, sizeof num);
    check(n == 1 && strcmp(num, "0") == 0, "seq is 0");
    n = field_num(buf, "t", num, sizeof num);
    check(n == 1 && strcmp(num, "0") == 0, "t is 0");
    n = field(buf, "in_head", val, sizeof val);
    check(n == 8 && strcmp(val, "12000024") == 0, "in_head is 12000024");
    n = field_num(buf, "in_len", num, sizeof num);
    check(n == 1 && strcmp(num, "4") == 0, "in_len is 4");
    n = field(buf, "out_head", val, sizeof val);
    check(n == 8 && strcmp(val, "12000024") == 0, "out_head is 12000024");
    /* round-trip: hex decodes back to the input bytes */
    n = unhex(val, dec, sizeof dec);
    check(n == 4 && memcmp(dec, cdb, 4) == 0, "out_head decodes to input bytes");
    n = field(buf, "code", val, sizeof val);
    check(n == 8 && strcmp(val, "0x2a002c") == 0, "code is 0x2a002c");
  }

  /* (iii) in_len > 64 -> head truncated to 64 bytes (128 hex chars) */
  {
    unsigned char big[100];
    int i;
    for(i = 0; i < 100; i++)
      big[i] = (unsigned char)i;
    r = ft_fmt_ioctl_record(
        buf, sizeof buf, 0xAA, 0x123, 9999, 0x2a002c, 100, big, 100, 1, 100,
        big, 100
    );
    check(r > 0, "record written (big buffers)");
    check_structure(buf);
    n = field_num(buf, "seq", num, sizeof num);
    check(n == 3 && strcmp(num, "291") == 0, "seq reports true value 291");
    n = field_num(buf, "t", num, sizeof num);
    check(n == 4 && strcmp(num, "9999") == 0, "t reports true value 9999");
    n = field_num(buf, "in_len", num, sizeof num);
    check(n == 3 && strcmp(num, "100") == 0, "in_len reports true length 100");
    n = field(buf, "in_head", val, sizeof val);
    check(n == HEAD_CAP, "in_head truncated to 64 bytes (128 hex chars)");
    n = unhex(val, dec, sizeof dec);
    check(
        n == 64 && memcmp(dec, big, 64) == 0,
        "in_head decodes to first 64 bytes"
    );
    n = field(buf, "out_head", val, sizeof val);
    check(n == HEAD_CAP, "out_head truncated to 64 bytes");
    n = unhex(val, dec, sizeof dec);
    check(
        n == 64 && memcmp(dec, big, 64) == 0,
        "out_head decodes to first 64 bytes"
    );
  }

  /* (iv) tiny cap -> -1, buffer untouched contract (no partial write) */
  {
    static const unsigned char cdb[4] = {0x12, 0x00, 0x00, 0x24};
    r = ft_fmt_ioctl_record(
        buf, 8, 0x12, 0, 0, 0x2a002c, 4, cdb, 4, 1, 4, cdb, 4
    );
    check(r == -1, "returns -1 when cap is 8");
  }

  /* (v) cap exactly len+1 succeeds */
  {
    static const unsigned char cdb[1] = {0xD0};
    int len = ft_fmt_ioctl_record(
        buf, sizeof buf, 0x1, 0, 0, 0xD0, 1, cdb, 1, 1, 1, cdb, 1
    );
    r = ft_fmt_ioctl_record(
        buf, (size_t)len + 1, 0x1, 0, 0, 0xD0, 1, cdb, 1, 1, 1, cdb, 1
    );
    check(r == len, "cap == len+1 accepted");
    r = ft_fmt_ioctl_record(
        buf, (size_t)len, 0x1, 0, 0, 0xD0, 1, cdb, 1, 1, 1, cdb, 1
    );
    check(r == -1, "cap == len rejected");
  }

  printf("PASS (%d checks)\n", g_checks);
  return 0;
}

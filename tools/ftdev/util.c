/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *load_file(const char *path, long *size) {
  FILE *f = fopen(path, "rb");
  char *buf = NULL;
  long n;
  if(!f)
    return NULL;
  if(fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 &&
     fseek(f, 0, SEEK_SET) == 0 && (buf = malloc((size_t)n + 1)) != NULL) {
    if(fread(buf, 1, (size_t)n, f) != (size_t)n) {
      free(buf);
      buf = NULL;
    } else {
      buf[n] = 0;
      *size = n;
    }
  }
  fclose(f);
  return buf;
}

/* the value after "key=" at the start of a line, or NULL */
static const char *find(const char *meta, const char *key) {
  size_t k = strlen(key);
  const char *p = meta;
  while(p && *p) {
    if(strncmp(p, key, k) == 0 && p[k] == '=')
      return p + k + 1;
    p = strchr(p, '\n');
    if(p)
      p++;
  }
  return NULL;
}

long meta_long(const char *meta, const char *key, long def) {
  const char *v = find(meta, key);
  return v ? strtol(v, NULL, 10) : def;
}

double meta_double(const char *meta, const char *key, double def) {
  const char *v = find(meta, key);
  return v ? strtod(v, NULL) : def;
}

int meta_str(const char *meta, const char *key, char *out, size_t cap) {
  const char *p = find(meta, key);
  size_t n = 0;
  if(!p)
    return 0;
  while(p[n] && p[n] != '\n' && p[n] != '\r' && n + 1 < cap) {
    out[n] = p[n];
    n++;
  }
  out[n] = 0;
  return 1;
}

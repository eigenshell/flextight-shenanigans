/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ftdev helpers: whole files and ftpoc's key=value .txt files */
#ifndef FTDEV_UTIL_H
#define FTDEV_UTIL_H
#include <stddef.h>

/* the whole file, NUL-terminated (size without the NUL); NULL on error */
void* load_file(const char* path, long* size);
/* value of key in a key=value text, or def */
long meta_long(const char* meta, const char* key, long def);
double meta_double(const char* meta, const char* key, double def);
/* 1 with the value (to the end of its line) in out, 0 if absent */
int meta_str(const char* meta, const char* key, char* out, size_t cap);

#endif

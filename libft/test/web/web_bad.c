/* SPDX-License-Identifier: GPL-2.0-or-later */
/* purity_web.sh's negative sample: a browser-tier object must not open
 * files */
#include <stdio.h>

int web_bad(const char* path) {
  FILE* f = fopen(path, "rb");
  return f ? fclose(f) : -1;
}

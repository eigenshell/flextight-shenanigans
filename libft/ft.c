/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft.h"

#include <stdarg.h>
#include <stdio.h>

int ft_fail(ft_err* e, const char* fmt, ...) {
  va_list ap;
  if(e) {
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof e->msg, fmt, ap);
    va_end(ap);
  }
  return -1;
}

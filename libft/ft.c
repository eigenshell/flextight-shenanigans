/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

int32_t ft_fail(ft_err* error, const char* format, ...) {
  if(error != NULL) {
    /* va_list walks the "..." values; vsnprintf formats them like
     * snprintf and never writes past the end of message */
    va_list values;
    va_start(values, format);
    vsnprintf(error->message, sizeof error->message, format, values);
    va_end(values);
  }
  return -1;
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The job file through a stream: ft_job.c works on buffers (it also runs
 * in the browser); these two read and write files for the command line
 * tools. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "ft_job.h"

/* the read buffer's first size; it doubles when full */
#define FIRST_CAPACITY_BYTES 16384

int32_t ft_write_job(const ft_job* job, FILE* file) {
  /* first ask for the length, then format into a buffer that fits */
  size_t length = ft_format_job(job, NULL, 0);
  if(length == FT_JOB_BAD) return -1;
  char* text = malloc(length + 1);
  if(text == NULL) return -1;
  ft_format_job(job, text, length + 1);
  bool is_written = fwrite(text, 1, length, file) == length;
  free(text);
  if(is_written == false || ferror(file)) return -1;
  return 0;
}

int32_t ft_read_job(ft_job* job, FILE* file, ft_err* error) {
  char* text = NULL;
  size_t length = 0;
  size_t capacity = 0;
  size_t bytes_read;
  do {
    if(length == capacity) {
      capacity = capacity > 0 ? 2 * capacity : FIRST_CAPACITY_BYTES;
      char* bigger = realloc(text, capacity);
      if(bigger == NULL) {
        free(text);
        return ft_fail(error, "out of memory reading the job");
      }
      text = bigger;
    }
    bytes_read = fread(text + length, 1, capacity - length, file);
    length += bytes_read;
  } while(bytes_read > 0);
  if(ferror(file)) {
    free(text);
    return ft_fail(error, "cannot read the job");
  }
  int32_t result = ft_parse_job(job, text, length, error);
  free(text);
  return result;
}

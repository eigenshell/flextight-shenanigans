/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The job file through a stream: ft_job.c works on buffers (it also runs
 * in the browser), these two read and write files for the CLIs. */
#include <stdlib.h>

#include "ft_job.h"

int ft_job_write(const ft_job* j, FILE* out) {
  size_t n = ft_job_write_buf(j, NULL, 0);
  char* buf;
  int rc;
  if(n == FT_JOB_BAD || !(buf = malloc(n + 1))) return -1;
  ft_job_write_buf(j, buf, n + 1);
  rc = fwrite(buf, 1, n, out) == n && !ferror(out) ? 0 : -1;
  free(buf);
  return rc;
}

int ft_job_read(ft_job* j, FILE* in, ft_err* e) {
  char *buf = NULL, *nb;
  size_t n = 0, cap = 0, got;
  int rc;
  do {
    if(n == cap) {
      cap = cap ? 2 * cap : 16384;
      if(!(nb = realloc(buf, cap))) {
        free(buf);
        return ft_fail(e, "out of memory reading the job");
      }
      buf = nb;
    }
    got = fread(buf + n, 1, cap - n, in);
    n += got;
  } while(got);
  if(ferror(in)) {
    free(buf);
    return ft_fail(e, "cannot read the job");
  }
  rc = ft_job_parse(j, buf, n, e);
  free(buf);
  return rc;
}

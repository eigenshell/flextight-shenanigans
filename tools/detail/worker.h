/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The worker thread of a detailed scan pass: drains the ring into
 * ft_detail (develop, encode, write). Shared by ftpoc (the scanner box)
 * and ftdev (re-develop a kept raw, the throughput bench). */
#ifndef FT_WORKER_H
#define FT_WORKER_H
#include "ft_detail.h"
#include "ring.h"

typedef struct {
  ring* r;
  ft_detail* d;
  unsigned stall_ms; /* tests and the bench: sleep this long
                        every stall_every lines (a slow card) */
  long stall_every;
  /* out, valid after pass_worker_join */
  int failed; /* ft_add_detail_line failed: e says why */
  ft_err e;
  long lines;
  double max_line_ms; /* longest single line: develop + write */
  double busy_ms;     /* time spent in ft_add_detail_line */
  ring_thread* t;
} pass_worker;

/* starts the thread; -1 if it could not */
int pass_worker_start(pass_worker* w);
/* waits until the ring is drained (ring_end) or failed */
void pass_worker_join(pass_worker* w);
/* one line of the pass summary: ring peak, longest line, speed */
void pass_worker_report(const pass_worker* w, double scan_s, char* buf,
                        size_t cap);

#endif

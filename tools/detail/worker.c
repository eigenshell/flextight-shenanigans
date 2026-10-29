/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "worker.h"

#include <stdio.h>

static void run(void* arg) {
  pass_worker* w = arg;
  for(;;) {
    const void* line = ring_peek(w->r, 250);
    double t0, dt;
    if(!line) {
      if(ring_drained(w->r) || ring_failed(w->r)) break;
      continue;
    }
    t0 = ring_ms();
    if(!w->failed && ft_add_detail_line(w->d, line, &w->e)) {
      w->failed = 1;
      ring_fail(w->r); /* the reader stops the scan */
    }
    w->lines++;
    if(w->stall_every && w->lines % w->stall_every == 0)
      ring_sleep_ms(w->stall_ms);
    dt = ring_ms() - t0;
    w->busy_ms += dt;
    if(dt > w->max_line_ms) w->max_line_ms = dt;
    ring_pop(w->r);
    if(w->failed) break;
  }
}

int pass_worker_start(pass_worker* w) {
  w->failed = 0;
  w->lines = 0;
  w->max_line_ms = w->busy_ms = 0;
  w->t = ring_thread_start(run, w);
  return w->t ? 0 : -1;
}

void pass_worker_join(pass_worker* w) {
  ring_thread_join(w->t);
  w->t = NULL;
}

void pass_worker_report(const pass_worker* w, double scan_s, char* buf,
                        size_t cap) {
  size_t peak, pushed;
  double line_mb = ring_line_bytes(w->r) / 1048576.0;
  ring_stats(w->r, &peak, &pushed);
  snprintf(buf, cap,
           "ring peak %lu of %lu lines (%.1f of %.1f MB); longest line "
           "%.0f ms; worker busy %.1f%% of %.0f s",
           (unsigned long)peak, (unsigned long)ring_slots(w->r), peak * line_mb,
           ring_slots(w->r) * line_mb, w->max_line_ms,
           scan_s > 0 ? 100.0 * w->busy_ms / (scan_s * 1000.0) : 0.0, scan_s);
}

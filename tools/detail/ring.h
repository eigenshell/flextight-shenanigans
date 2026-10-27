/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The RAM between the scanner and the disk (sub-project 2b): a ring of
 * fixed-size lines, one producer (the thread reading the scanner) and one
 * consumer (the worker developing and writing). The producer NEVER
 * blocks: the stock firmware aborts a scan when the host stalls, so a full
 * ring is reported (ring_push -1) and the scan stopped cleanly, never
 * waited on. The consumer waits with a timeout. pthreads, or Win32 on the
 * scanner box (32-bit, XP-era API: a critical section and an event). */
#ifndef FT_RING_H
#define FT_RING_H
#include <stddef.h>

typedef struct ring ring;

/* nslots = total_bytes / line_bytes (at least 2); NULL out of memory */
ring* ring_open(size_t line_bytes, size_t total_bytes);
void ring_close(ring* r);
size_t ring_slots(const ring* r);
size_t ring_line_bytes(const ring* r);

/* producer: copy one line in; -1 if the ring is full or failed */
int ring_push(ring* r, const void* line);
/* producer: a byte stream (reads may end mid-line); whole lines are
 * pushed as they complete. -1 if a push failed. */
int ring_write(ring* r, const void* bytes, size_t n);
/* producer: no more lines (a partial line left by ring_write is dropped) */
void ring_end(ring* r);

/* consumer: the oldest line, waiting up to ms; NULL on timeout or when
 * the ring is ended and empty (ring_drained) */
const void* ring_peek(ring* r, unsigned ms);
/* consumer: release the line ring_peek returned */
void ring_pop(ring* r);
/* ended and empty: the consumer is done */
int ring_drained(ring* r);
/* consumer: give up (ft_detail failed); further pushes fail */
void ring_fail(ring* r);
int ring_failed(ring* r);
/* the most lines ever waiting at once; lines pushed */
void ring_stats(ring* r, size_t* peak, size_t* pushed);

/* milliseconds on a monotonic clock */
double ring_ms(void);
/* sleep (tests, simulated stalls) */
void ring_sleep_ms(unsigned ms);

/* a thread running fn(arg); NULL on failure */
typedef struct ring_thread ring_thread;
ring_thread* ring_thread_start(void (*fn)(void*), void* arg);
void ring_thread_join(ring_thread* t);

#endif

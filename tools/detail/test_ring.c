/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The ring under a consumer that stalls (a slow SD card): nothing lost or
 * reordered while it fits, a full ring refused (never waited on), byte
 * streams reassembled into lines. */
#include "ring.h"
#include "test/ftest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  ring *r;
  long n, got, bad;
  unsigned stall_ms;
  long stall_every;
  unsigned first_ms; /* sleep before consuming anything */
} consumer;

static void consume(void *arg) {
  consumer *c = arg;
  if(c->first_ms)
    ring_sleep_ms(c->first_ms);
  for(;;) {
    const long *line = ring_peek(c->r, 100);
    if(!line) {
      if(ring_drained(c->r))
        break;
      continue;
    }
    if(line[0] != c->got || line[1] != ~c->got)
      c->bad++;
    c->got++;
    ring_pop(c->r);
    if(c->stall_every && c->got % c->stall_every == 0)
      ring_sleep_ms(c->stall_ms);
  }
}

int main(void) {
  /* a steady producer (~9k lines/s), a consumer stalling 20 ms every 400
   * lines: faster on average, but backed up during each stall */
  {
    consumer c;
    ring_thread *t;
    size_t peak, pushed;
    long i, line[2], refused = 0;
    memset(&c, 0, sizeof c);
    c.r = ring_open(sizeof line, 1024 * sizeof line);
    CHECK(c.r != NULL && ring_slots(c.r) == 1024);
    c.stall_ms = 20;
    c.stall_every = 400;
    t = ring_thread_start(consume, &c);
    CHECK(t != NULL);
    for(i = 0; i < 20000; i++) {
      line[0] = i;
      line[1] = ~i;
      refused += ring_push(c.r, line) != 0;
      if(i % 10 == 9)
        ring_sleep_ms(1);
    }
    ring_end(c.r);
    ring_thread_join(t);
    ring_stats(c.r, &peak, &pushed);
    CHECK(refused == 0 && pushed == 20000);
    CHECK(c.got == 20000 && c.bad == 0);
    CHECK(peak > 20 && peak <= 1024); /* the stalls backed it up */
    CHECK(ring_drained(c.r));
    CHECK(ring_push(c.r, line) == -1); /* ended */
    ring_close(c.r);
  }

  /* a consumer that does not start for 300 ms: the ninth push of an
   * 8-line ring is refused at once, the 8 waiting arrive in order */
  {
    consumer c;
    ring_thread *t;
    long i, line[2], first_refused = -1;
    double t0;
    memset(&c, 0, sizeof c);
    c.r = ring_open(sizeof line, 8 * sizeof line + 5);
    CHECK(ring_slots(c.r) == 8);
    c.first_ms = 300;
    t = ring_thread_start(consume, &c);
    t0 = ring_ms();
    for(i = 0; i < 100 && first_refused < 0; i++) {
      line[0] = i;
      line[1] = ~i;
      if(ring_push(c.r, line))
        first_refused = i;
    }
    CHECK(first_refused == 8);
    CHECK(ring_ms() - t0 < 200); /* refused, not waited on */
    ring_end(c.r);
    ring_thread_join(t);
    CHECK(c.got == 8 && c.bad == 0);
    ring_close(c.r);
  }

  /* reads that end mid-line: whole lines out, in order */
  {
    enum { LINE = 24, N = 50 };
    unsigned char stream[LINE * N + 7];
    size_t pos = 0, k;
    unsigned seed = 3;
    ring *r = ring_open(LINE, LINE * (N + 1));
    CHECK(r != NULL);
    for(k = 0; k < sizeof stream; k++)
      stream[k] = (unsigned char)(k * 31 + 7);
    while(pos < sizeof stream) {
      size_t n;
      seed = seed * 1103515245u + 12345u;
      n = 1 + (seed >> 16) % 37;
      if(n > sizeof stream - pos)
        n = sizeof stream - pos;
      CHECK(ring_write(r, stream + pos, n) == 0);
      pos += n;
    }
    ring_end(r); /* the 7 extra bytes dropped */
    for(k = 0; k < N; k++) {
      const unsigned char *l = ring_peek(r, 0);
      CHECK(l != NULL && memcmp(l, stream + k * LINE, LINE) == 0);
      ring_pop(r);
    }
    CHECK(ring_peek(r, 0) == NULL && ring_drained(r));
    ring_close(r);
  }

  /* failed by the consumer: pushes refused; peek times out empty */
  {
    long line[2] = {0, -1};
    ring *r = ring_open(sizeof line, 4 * sizeof line);
    double t0;
    CHECK(ring_push(r, line) == 0);
    ring_pop(r);
    t0 = ring_ms();
    CHECK(ring_peek(r, 50) == NULL);
    CHECK(ring_ms() - t0 >= 40 && !ring_drained(r));
    ring_fail(r);
    CHECK(ring_failed(r) && ring_push(r, line) == -1);
    CHECK(ring_write(r, line, sizeof line) == -1);
    ring_close(r);
    CHECK(ring_open(0, 100) == NULL);
    CHECK(ring_open(16, 16) == NULL); /* fewer than 2 lines */
  }
  return TEST_DONE();
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#define _POSIX_C_SOURCE 200112L
#endif
#include <stdlib.h>
#include <string.h>

#include "ring.h"

#ifdef _WIN32
typedef CRITICAL_SECTION lock_t;
#define LOCK(r) EnterCriticalSection(&(r)->lock)
#define UNLOCK(r) LeaveCriticalSection(&(r)->lock)
#else
#include <errno.h>
#include <pthread.h>
#include <time.h>
typedef pthread_mutex_t lock_t;
#define LOCK(r) pthread_mutex_lock(&(r)->lock)
#define UNLOCK(r) pthread_mutex_unlock(&(r)->lock)
#endif

struct ring {
  unsigned char* buf;
  size_t line, nslots;
  size_t head, count; /* oldest slot, lines waiting */
  size_t peak, pushed;
  int ended, failed;
  unsigned char* part; /* ring_write: the line being filled */
  size_t part_n;
  lock_t lock;
#ifdef _WIN32
  HANDLE evt; /* auto-reset: a line arrived, or end */
#else
  pthread_cond_t cond;
#endif
};

ring* ring_open(size_t line_bytes, size_t total_bytes) {
  ring* r;
  size_t n = line_bytes ? total_bytes / line_bytes : 0;
  if(line_bytes == 0 || n < 2) return NULL;
  r = calloc(1, sizeof *r);
  if(!r) return NULL;
  r->line = line_bytes;
  r->nslots = n;
  r->buf = malloc(n * line_bytes);
  r->part = malloc(line_bytes);
  if(!r->buf || !r->part) {
    free(r->buf);
    free(r->part);
    free(r);
    return NULL;
  }
#ifdef _WIN32
  InitializeCriticalSection(&r->lock);
  r->evt = CreateEventA(NULL, FALSE, FALSE, NULL);
  if(!r->evt) {
    DeleteCriticalSection(&r->lock);
    free(r->buf);
    free(r->part);
    free(r);
    return NULL;
  }
#else
  pthread_mutex_init(&r->lock, NULL);
  pthread_cond_init(&r->cond, NULL);
#endif
  return r;
}

void ring_close(ring* r) {
  if(!r) return;
#ifdef _WIN32
  DeleteCriticalSection(&r->lock);
  CloseHandle(r->evt);
#else
  pthread_mutex_destroy(&r->lock);
  pthread_cond_destroy(&r->cond);
#endif
  free(r->buf);
  free(r->part);
  free(r);
}

size_t ring_slots(const ring* r) { return r->nslots; }

size_t ring_line_bytes(const ring* r) { return r->line; }

static void signal_consumer(ring* r) {
#ifdef _WIN32
  SetEvent(r->evt);
#else
  pthread_cond_signal(&r->cond);
#endif
}

int ring_push(ring* r, const void* line) {
  int rc = -1;
  LOCK(r);
  if(!r->failed && !r->ended && r->count < r->nslots) {
    size_t slot = (r->head + r->count) % r->nslots;
    /* the consumer only touches the head slot, and only while
     * count > 0 covers it: this slot is free to fill */
    UNLOCK(r);
    memcpy(r->buf + slot * r->line, line, r->line);
    LOCK(r);
    r->count++;
    r->pushed++;
    if(r->count > r->peak) r->peak = r->count;
    rc = 0;
    signal_consumer(r);
  }
  UNLOCK(r);
  return rc;
}

int ring_write(ring* r, const void* bytes, size_t n) {
  const unsigned char* p = bytes;
  while(n) {
    size_t take = r->line - r->part_n;
    if(take > n) take = n;
    memcpy(r->part + r->part_n, p, take);
    r->part_n += take;
    p += take;
    n -= take;
    if(r->part_n == r->line) {
      if(ring_push(r, r->part)) return -1;
      r->part_n = 0;
    }
  }
  return 0;
}

void ring_end(ring* r) {
  LOCK(r);
  r->ended = 1;
  signal_consumer(r);
  UNLOCK(r);
}

const void* ring_peek(ring* r, unsigned ms) {
  const void* line = NULL;
#ifdef _WIN32
  DWORD t0 = GetTickCount();
  for(;;) {
    DWORD spent;
    LOCK(r);
    if(r->count) {
      line = r->buf + r->head * r->line;
      UNLOCK(r);
      return line;
    }
    if(r->ended) {
      UNLOCK(r);
      return NULL;
    }
    UNLOCK(r);
    spent = GetTickCount() - t0;
    if(spent >= ms) return NULL;
    WaitForSingleObject(r->evt, ms - spent);
  }
#else
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += ms / 1000;
  ts.tv_nsec += (long)(ms % 1000) * 1000000L;
  if(ts.tv_nsec >= 1000000000L) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000L;
  }
  LOCK(r);
  while(!r->count && !r->ended)
    if(pthread_cond_timedwait(&r->cond, &r->lock, &ts) == ETIMEDOUT) break;
  if(r->count) line = r->buf + r->head * r->line;
  UNLOCK(r);
  return line;
#endif
}

void ring_pop(ring* r) {
  LOCK(r);
  if(r->count) {
    r->head = (r->head + 1) % r->nslots;
    r->count--;
  }
  UNLOCK(r);
}

int ring_drained(ring* r) {
  int d;
  LOCK(r);
  d = r->ended && !r->count;
  UNLOCK(r);
  return d;
}

void ring_fail(ring* r) {
  LOCK(r);
  r->failed = 1;
  UNLOCK(r);
}

int ring_failed(ring* r) {
  int f;
  LOCK(r);
  f = r->failed;
  UNLOCK(r);
  return f;
}

void ring_stats(ring* r, size_t* peak, size_t* pushed) {
  LOCK(r);
  *peak = r->peak;
  *pushed = r->pushed;
  UNLOCK(r);
}

double ring_ms(void) {
#ifdef _WIN32
  return (double)GetTickCount();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

void ring_sleep_ms(unsigned ms) {
#ifdef _WIN32
  Sleep(ms);
#else
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  while(nanosleep(&ts, &ts) == -1 && errno == EINTR);
#endif
}

struct ring_thread {
  void (*fn)(void*);
  void* arg;
#ifdef _WIN32
  HANDLE h;
#else
  pthread_t t;
#endif
};

#ifdef _WIN32
static DWORD WINAPI trampoline(LPVOID p) {
  ring_thread* t = p;
  t->fn(t->arg);
  return 0;
}
#else
static void* trampoline(void* p) {
  ring_thread* t = p;
  t->fn(t->arg);
  return NULL;
}
#endif

ring_thread* ring_thread_start(void (*fn)(void*), void* arg) {
  ring_thread* t = calloc(1, sizeof *t);
  if(!t) return NULL;
  t->fn = fn;
  t->arg = arg;
#ifdef _WIN32
  t->h = CreateThread(NULL, 0, trampoline, t, 0, NULL);
  if(!t->h) {
    free(t);
    return NULL;
  }
#else
  if(pthread_create(&t->t, NULL, trampoline, t) != 0) {
    free(t);
    return NULL;
  }
#endif
  return t;
}

void ring_thread_join(ring_thread* t) {
  if(!t) return;
#ifdef _WIN32
  WaitForSingleObject(t->h, INFINITE);
  CloseHandle(t->h);
#else
  pthread_join(t->t, NULL);
#endif
  free(t);
}

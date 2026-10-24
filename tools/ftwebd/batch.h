/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The batch: a small FIFO of ftpoc jobs (task.h), queued in order and run
 * one at a time. Static slots, no locking: task.c holds its lock around
 * every call. Not persisted (a box reboot loses it). */
#ifndef FTWEBD_BATCH_H
#define FTWEBD_BATCH_H
#include <stddef.h>

#define BATCH_MAX 8

enum { JOB_QUEUED, JOB_RUNNING, JOB_DONE, JOB_FAILED, JOB_STOPPED };

typedef struct {
  int id;        /* 1.., in the order queued; 0: free */
  int state;     /* JOB_* */
  int exit_code; /* -1 until it ends */
  int stopped;   /* a stop was delivered while it ran */
  char op[16], strip[80], film[8], arc[80]; /* arc "": none */
  int share;
  long first, last; /* its lines in task.c's ring (numbered
                       across jobs); last 0 until it ends */
} batch_job;

/* a slot at the tail: a free one, else the oldest ended job's; NULL when
 * every slot is queued or running. The next id, JOB_QUEUED, exit -1, the
 * rest zero */
batch_job* batch_add(void);
batch_job* batch_find(int id); /* NULL: none (or recycled) */
batch_job* batch_next(void);   /* the oldest queued; NULL none */
/* the running job, else the latest; NULL when there has been none */
batch_job* batch_current(void);
void batch_drop(batch_job* j); /* its slot freed */
int batch_ended(const batch_job* j);
const char* batch_state(const batch_job* j); /* "queued", ... */
/* [{"id","op","strip","state","exit"},...] in id order -> out; its
 * length, -1 if it does not fit */
long batch_json(char* out, size_t n);

#endif

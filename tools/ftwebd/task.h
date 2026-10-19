/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ftpoc tasks: the client names an op, ftwebd builds the argv (never the
 * client). Tasks are the batch's jobs (batch.h): queued in order, run one
 * at a time (the scanner has one holder), the next as one ends (whether
 * it ended well, failed or was stopped). A job's
 * output is numbered lines (1..): line 1 is the built command, the last
 * "ftwebd exit <code>". Events resume with since = the last number
 * seen. */
#ifndef FTWEBD_TASK_H
#define FTWEBD_TASK_H
#include <stddef.h>

typedef struct {
    const char *ftpoc;          /* absolute path */
    const char *strips;         /* strip folders, relative to the cwd */
    const char *archive;        /* ARC frames' folder (NULL: none) */
    const char *archive_ppi;    /* "3150" or "6300" */
    const char *microcode;      /* ftpoc --microcode (NULL: ftpoc's
                                   default, beside its exe) */
} task_cfg;

/* what the client asked for (checked by task_start) */
typedef struct {
    const char *op;             /* "preview" or "scan" */
    const char *strip;          /* a checked name */
    const char *film;           /* "neg" | "slide" (preview, ARC) */
    int share;                  /* scan: the share windows of strip.job */
    const char *arc;            /* scan: ARC frames "1,3-4" (NULL: none) */
    const char *job;            /* scan + share: the job's text, kept as
                                   strip_j<id>.job (NULL: a copy of
                                   strip.job) */
    size_t njob;
} task_req;

void task_init(const task_cfg *cfg);
/* queues a job at the batch's tail (it starts at once when the scanner
 * is idle). preview: ftpoc frames (film neg|slide) -> strip.job +
 * strip.png. scan: up to three steps, each run after the last one exits
 * 0: the share windows (scan --job strip_j<id>.job; ftpoc names the
 * outputs after the job: strip_j<id>_winNN.raw), then the ARC frames as
 * a detailed scan into <archive>/<strip>/ (frames --detail neg --select
 * -o strip_j<id>, then scan --job on that job). Each step's command is
 * an event line (a step that cannot start: its error, then the exit
 * line, -1; failed, and the next runs). 0 with *id set; 1 the batch is
 * full (err); -1 bad request,
 * no job to scan, or (idle, nothing queued) its first step cannot start
 * (err; no job kept) */
int task_start(const task_req *rq, int *id, char *err, size_t errn);
/* the running job stopped: it ends "stopped" and no further step of it
 * starts, whether the signal reached the step or it had already exited.
 * 0 stopping, -1 not running, -2 no such task */
int task_stop(int id);
/* b[0..n) as path: written to path.tmp and renamed over it, so a reader
 * has the old file or the new one, whole, and a failed write leaves path
 * as it was. 0 or -1 */
int task_write_file(const char *path, const char *b, size_t n);
/* a queued share scan's job file replaced by text[0..n) (checked by the
 * caller), under the lock that starts jobs: it lands before the start or
 * not at all. 0 written; 1 the job has started (err); -1 not a share
 * scan (err); -2 no such task; -3 cannot write (err) */
int task_put_job(int id, const char *text, size_t n, char *err,
                 size_t errn);
/* a queued job dropped (its job file removed), or the running one
 * stopped (as task_stop; the next runs). 0 dropped, 1 stopping, -1 it
 * has ended, -2 no such task */
int task_delete(int id);
/* every queued job dropped (as task_delete); how many */
int task_clear(void);
/* {"id":..,"op":..,"strip":..,"state":"queued|running|done|failed|stopped","exit":..,
 * "stopped":..,"lines":..} for id, or the running (else the latest) job
 * when id is 0. 0, or -1 if there is none (out: "null") */
int task_json(int id, char *out, size_t n);
/* SSE text for job id's lines after `since` ("id: k\ndata: line\n\n"
 * each), waiting up to wait_s for one (a queued job: until it starts).
 * Returns the bytes written (0: none yet), -1 no such job (or dropped).
 * *since moves past what was written; *ended = the job has ended and
 * every line was written. An ended job's lines stay until newer jobs'
 * lines overwrite them (the ring is shared) */
long task_events(int id, long *since, char *out, size_t n, int wait_s,
                 int *ended);
/* SSE text for the batch: "id: rev\ndata: {"rev":..,"jobs":[..]}\n\n"
 * (batch_json's list) once the batch's revision (+1 on every add, PUT,
 * start, end, drop) differs from *rev (-1: at once), waiting up to
 * wait_s. The bytes written (0: no change yet), -1 if it does not fit;
 * *rev = the revision written */
long task_batch(long *rev, char *out, size_t n, int wait_s);

#endif

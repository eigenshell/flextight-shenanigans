/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ftpoc tasks: the client names an op, ftwebd builds the argv (never the
 * client). One task at a time (the scanner has one holder). Its output is
 * a ring of numbered lines (1..): line 1 is the built command, the last
 * is "ftwebd exit <code>". Events resume with since = the last number
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
/* preview: ftpoc frames (film neg|slide) -> strip.job + strip.png.
 * scan: up to three steps, each run after the last one exits 0: the
 * share windows (scan --job strip_j<id>.job; ftpoc names the outputs
 * after the job: strip_j<id>_winNN.raw), then the ARC frames as a
 * detailed scan into <archive>/<strip>/ (frames --detail neg --select
 * -o strip_j<id>, then scan --job on that job). Each step's command is
 * an event line. 0 with *id set; 1 busy (err names the running task);
 * -1 bad request, no job to scan or spawn failure (err) */
int task_start(const task_req *rq, int *id, char *err, size_t errn);
/* 0 delivered, -1 not running or not delivered, -2 no such task */
int task_stop(int id);
/* {"id":..,"op":..,"strip":..,"state":"running|done","exit":..,
 * "lines":..} for id, or the latest task when id is 0. 0, or -1 if there
 * is none (out: "null") */
int task_json(int id, char *out, size_t n);
/* SSE text for the lines after `since` ("id: k\ndata: line\n\n" each),
 * waiting up to wait_s for one. Returns the bytes written (0: none yet),
 * -1 no such task. *since moves past what was written; *ended = the task
 * has exited and every line was written. */
long task_events(int id, long *since, char *out, size_t n, int wait_s,
                 int *ended);

#endif

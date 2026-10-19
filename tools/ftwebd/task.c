/* SPDX-License-Identifier: GPL-2.0-or-later */
/* task.h: the batch's jobs (batch.h) run one at a time, each a chain of
 * ftpoc children; a reader thread splits a child's output into numbered
 * lines (on '\n' or '\r': ftpoc's human counters use '\r') and wakes the
 * waiters per line. One ring holds every job's lines, numbered across
 * jobs; a job keeps where its lines are, so an ended job's lines stay
 * readable until newer ones overwrite them. One lock (T.mu) covers the
 * batch, the ring and the job files. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "batch.h"
#include "http.h"
#include "proc.h"
#include "task.h"

#define RING 4096               /* lines kept; older ones are dropped */
#define LINE 320
#define STEPS 3
#define ARGS 16

static struct {
    task_cfg cfg;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    batch_job *run;             /* the running job; NULL: idle */
    long rev;                   /* +1 on every change to the batch */
    int nsteps, step;           /* the running job's chain, its step */
    char arg[STEPS][ARGS][300];
    int narg[STEPS];
    proc p;
    long nlines;                /* every job's lines, numbered 1.. */
    char lines[RING][LINE];
    char part[LINE];            /* the line being read */
    size_t npart;
} T = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

void task_init(const task_cfg *cfg)
{
    T.cfg = *cfg;
}

/* under T.mu: the batch changed */
static void changed(void)
{
    T.rev++;
    pthread_cond_broadcast(&T.cv);
}

/* under T.mu */
static void add_line(const char *s, size_t n)
{
    char *d;
    if (n >= LINE)
        n = LINE - 1;
    T.nlines++;
    d = T.lines[(T.nlines - 1) % RING];
    memcpy(d, s, n);
    d[n] = 0;
    pthread_cond_broadcast(&T.cv);
}

/* under T.mu: bytes from the child, split into lines */
static void feed(const char *b, long n)
{
    long i;
    for (i = 0; i < n; i++) {
        if (b[i] == '\n' || b[i] == '\r') {
            if (T.npart)
                add_line(T.part, T.npart);
            T.npart = 0;
        } else if (T.npart < LINE - 1) {
            T.part[T.npart++] = b[i];
        }
    }
}

/* 0, or -1 if from cannot be read or to written */
static int copy_file(const char *from, const char *to)
{
    char buf[4096];
    FILE *in = fopen(from, "rb"), *out;
    size_t n;
    int rc = 0;
    if (!in)
        return -1;
    out = fopen(to, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        rc |= fwrite(buf, 1, n, out) != n;
    rc |= ferror(in) != 0;
    fclose(in);
    rc |= fclose(out) != 0;
    return rc ? -1 : 0;
}

/* b[0..n) as path; 0 or -1 */
static int write_file(const char *path, const char *b, size_t n)
{
    FILE *f = fopen(path, "wb");
    int rc;
    if (!f)
        return -1;
    rc = fwrite(b, 1, n, f) != n;
    rc |= fclose(f) != 0;
    return rc ? -1 : 0;
}

/* a share scan's job file: <strips>/<strip>/strip_j<id>.job */
static void job_file(char *out, size_t n, const batch_job *j)
{
    snprintf(out, n, "%s/%s/strip_j%d.job", T.cfg.strips, j->strip, j->id);
}

/* a good preview: keep its job as strip.preview.job, the planner's
 * starting point (every plan starts from it) */
static void keep_preview_job(const char *strip)
{
    char a[300], b[300];
    snprintf(a, sizeof a, "%s/%s/strip.job", T.cfg.strips, strip);
    snprintf(b, sizeof b, "%s/%s/strip.preview.job", T.cfg.strips, strip);
    copy_file(a, b);
}

/* under T.mu: run step i; its command (to copy to a terminal) is the
 * next event line. 0, or -1 with err */
static int run_step(int i, char *err, size_t errn)
{
    const char *argv[ARGS + 1];
    char cmd[LINE];
    size_t k = 0;
    int a;
    for (a = 0; a < T.narg[i]; a++)
        argv[a] = T.arg[i][a];
    argv[a] = NULL;
    if (proc_spawn(&T.p, argv, ".", err, errn))
        return -1;
    T.step = i;
    for (a = 0; argv[a] && k + 2 < sizeof cmd; a++)
        k += (size_t)snprintf(cmd + k, sizeof cmd - k, "%s%s", a ? " " : "",
                              argv[a]);
    add_line(cmd, strlen(cmd));
    return 0;
}

/* under T.mu: step i's argv from a NULL-terminated list, + --microcode
 * when ftwebd has one */
static void set_step(int i, const char *const *a)
{
    int k;
    for (k = 0; a[k] && k < ARGS; k++)
        snprintf(T.arg[i][k], sizeof T.arg[i][k], "%s", a[k]);
    if (T.cfg.microcode && k + 2 <= ARGS) {
        snprintf(T.arg[i][k++], sizeof T.arg[i][0], "--microcode");
        snprintf(T.arg[i][k++], sizeof T.arg[i][0], "%s", T.cfg.microcode);
    }
    T.narg[i] = k;
}

/* under T.mu: job j's chain -> T.arg, T.nsteps. The job's id names its
 * files, so a rescan never overwrites an earlier scan */
static void build(const batch_job *j)
{
    char base[300], job[310], abase[300], ajob[310];
    int ns = 0;
    snprintf(base, sizeof base, "%s/%s/strip", T.cfg.strips, j->strip);
    job_file(job, sizeof job, j);
    snprintf(abase, sizeof abase, "%s/%s/strip_j%d",
             T.cfg.archive ? T.cfg.archive : "", j->strip, j->id);
    snprintf(ajob, sizeof ajob, "%s.job", abase);
    if (strcmp(j->op, "preview") == 0) {
        /* preview + detection, every whole frame planned at 4MP (no
         * 3-point focus check; ~40 s); the page re-plans sizes */
        const char *a[] = { T.cfg.ftpoc, "frames", "--film", j->film,
                            "--share", "2450", "--progress", "-o", base,
                            NULL };
        set_step(ns++, a);
    } else {
        const char *a[] = { T.cfg.ftpoc, "scan", "--job", job, "--progress",
                            NULL };
        /* ARC: a detailed scan of its own (spec section 9): preview at
         * the scan's optics, focus per frame, then its passes */
        const char *d[] = { T.cfg.ftpoc, "frames", "--detail", "neg",
                            "--ppi", T.cfg.archive_ppi, "--film", j->film,
                            "--select", j->arc, "--progress", "-o", abase,
                            NULL };
        const char *b[] = { T.cfg.ftpoc, "scan", "--job", ajob, "--progress",
                            NULL };
        if (j->share)
            set_step(ns++, a);
        if (j->arc[0]) {
            set_step(ns++, d);
            set_step(ns++, b);
        }
    }
    T.nsteps = ns;
}

/* under T.mu: the running job ended with code. A good preview's job is
 * kept first: the page reads it at the exit line */
static void finish(int code)
{
    char end[48];
    batch_job *j = T.run;
    if (code == 0 && strcmp(j->op, "preview") == 0)
        keep_preview_job(j->strip);
    snprintf(end, sizeof end, "ftwebd exit %d", code);
    add_line(end, strlen(end));
    j->exit_code = code;
    j->last = T.nlines;
    j->state = JOB_DONE;
    T.run = NULL;
    changed();
}

static void *reader(void *arg);

/* under T.mu: job j runs (its first step and the reader). 0, or -1 with
 * err and j still T.run (the caller ends or drops it); a first step that
 * cannot start removes the job file ftpoc never read */
static int run_job(batch_job *j, char *err, size_t errn)
{
    char job[310];
    pthread_t th;
    T.run = j;
    j->state = JOB_RUNNING;
    j->first = T.nlines + 1;
    T.npart = 0;
    build(j);
    if (run_step(0, err, errn)) {
        if (j->share) {
            job_file(job, sizeof job, j);
            remove(job);
        }
        return -1;
    }
    if (pthread_create(&th, NULL, reader, NULL)) {
        /* cannot happen in practice */
        snprintf(err, errn, "cannot start the reader thread");
        proc_stop(&T.p);
        return -1;
    }
    pthread_detach(th);
    changed();
    return 0;
}

/* under T.mu, when idle: the oldest queued job runs. A job whose first
 * step cannot start ends at once (its error, exit -1) */
static void start_next(void)
{
    char err[300];
    batch_job *j;
    if (T.run || !(j = batch_next()))
        return;
    if (run_job(j, err, sizeof err)) {
        add_line(err, strlen(err));
        finish(-1);
    }
}

static void *reader(void *arg)
{
    char buf[4096], err[300];
    long n;
    int code;
    (void)arg;
    for (;;) {
        while ((n = proc_read(&T.p, buf, sizeof buf)) > 0) {
            pthread_mutex_lock(&T.mu);
            feed(buf, n);
            pthread_mutex_unlock(&T.mu);
        }
        code = proc_wait(&T.p);
        pthread_mutex_lock(&T.mu);
        if (T.npart)
            add_line(T.part, T.npart);
        T.npart = 0;
        if (code != 0 || T.run->stopped || T.step + 1 >= T.nsteps)
            break;
        if (run_step(T.step + 1, err, sizeof err)) {
            add_line(err, strlen(err));
            code = -1;
            break;
        }
        pthread_mutex_unlock(&T.mu);
    }
    finish(code);
    if (code == 0)                      /* a failure halts the batch */
        start_next();
    pthread_mutex_unlock(&T.mu);
    return NULL;
}

/* frame numbers and ranges: 1,3-6 */
static int select_ok(const char *s)
{
    size_t n = strlen(s), i;
    if (n < 1 || n > 64)
        return 0;
    for (i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || s[i] == ',' || s[i] == '-'))
            return 0;
    return 1;
}

int task_start(const task_req *rq, int *id, char *err, size_t errn)
{
    char job[310], from[310];
    const char *f = rq->film;
    batch_job *j;
    int idle;

    if (strcmp(rq->op, "preview") && strcmp(rq->op, "scan")) {
        snprintf(err, errn, "unknown op '%s'", rq->op);
        return -1;
    }
    if ((strcmp(rq->op, "preview") == 0 || rq->arc) &&
        (!f || (strcmp(f, "neg") && strcmp(f, "slide")))) {
        snprintf(err, errn, "film must be neg or slide");
        return -1;
    }
    if (strcmp(rq->op, "scan") == 0) {
        if (rq->arc && !select_ok(rq->arc)) {
            snprintf(err, errn, "arc: frame numbers, e.g. 1,3-4");
            return -1;
        }
        if (rq->arc && !T.cfg.archive) {
            snprintf(err, errn, "no archive folder (ftwebd --archive)");
            return -1;
        }
        if (!rq->share && !rq->arc) {
            snprintf(err, errn, "nothing to scan");
            return -1;
        }
    }

    pthread_mutex_lock(&T.mu);
    idle = !T.run && !batch_next();
    j = batch_add();
    if (!j) {
        snprintf(err, errn, "the batch is full: %d jobs queued or running",
                 BATCH_MAX);
        pthread_mutex_unlock(&T.mu);
        return 1;
    }
    snprintf(j->op, sizeof j->op, "%s", rq->op);
    snprintf(j->strip, sizeof j->strip, "%s", rq->strip);
    snprintf(j->film, sizeof j->film, "%s", f ? f : "");
    snprintf(j->arc, sizeof j->arc, "%s", rq->arc ? rq->arc : "");
    j->share = strcmp(rq->op, "scan") == 0 && rq->share;
    if (j->share) {
        job_file(job, sizeof job, j);
        snprintf(from, sizeof from, "%s/%s/strip.job", T.cfg.strips,
                 rq->strip);
        if (rq->job ? write_file(job, rq->job, rq->njob)
                    : copy_file(from, job)) {
            if (rq->job)
                snprintf(err, errn, "cannot write %s", job);
            else
                snprintf(err, errn, "no strip.job to scan (send job=)");
            batch_drop(j);
            pthread_mutex_unlock(&T.mu);
            return -1;
        }
    }
    /* idle with nothing queued: j starts now, and a first step that
     * cannot start refuses the request (no job file, no slot left) */
    if (idle && run_job(j, err, errn)) {
        T.run = NULL;
        batch_drop(j);
        pthread_mutex_unlock(&T.mu);
        return -1;
    }
    *id = j->id;
    changed();
    start_next();
    pthread_mutex_unlock(&T.mu);
    return 0;
}

int task_stop(int id)
{
    batch_job *j;
    int rc = -1;
    pthread_mutex_lock(&T.mu);
    j = batch_find(id);
    if (!j)
        rc = -2;
    else if (j == T.run && proc_stop(&T.p) == 0) {
        j->stopped = 1;
        changed();
        rc = 0;
    }
    pthread_mutex_unlock(&T.mu);
    return rc;
}

int task_json(int id, char *out, size_t n)
{
    char op[40], strip[200];
    batch_job *j;
    long lines;
    pthread_mutex_lock(&T.mu);
    j = id ? batch_find(id) : batch_current();
    if (!j) {
        pthread_mutex_unlock(&T.mu);
        snprintf(out, n, "null");
        return -1;
    }
    lines = j->state == JOB_QUEUED ? 0
          : (j->last ? j->last : T.nlines) - j->first + 1;
    json_esc(op, sizeof op, j->op);
    json_esc(strip, sizeof strip, j->strip);
    snprintf(out, n, "{\"id\":%d,\"op\":%s,\"strip\":%s,\"state\":\"%s\","
             "\"exit\":%d,\"stopped\":%s,\"lines\":%ld}", j->id, op, strip,
             batch_state(j), j->exit_code, j->stopped ? "true" : "false",
             lines);
    pthread_mutex_unlock(&T.mu);
    return 0;
}

long task_events(int id, long *since, char *out, size_t n, int wait_s,
                 int *ended)
{
    struct timespec ts;
    batch_job *j;
    size_t k = 0;
    long i, top, gone;
    pthread_mutex_lock(&T.mu);
    j = batch_find(id);
    if (!j) {
        pthread_mutex_unlock(&T.mu);
        return -1;
    }
    ts.tv_sec = time(NULL) + wait_s;
    ts.tv_nsec = 0;
    while (j->id == id && (j->state == JOB_QUEUED ||
                           (!j->last && *since >= T.nlines - j->first + 1)))
        if (pthread_cond_timedwait(&T.cv, &T.mu, &ts))
            break;
    if (j->id != id) {                  /* dropped, or its slot reused */
        pthread_mutex_unlock(&T.mu);
        return -1;
    }
    *ended = 0;
    if (j->state == JOB_QUEUED) {
        if (n)
            out[0] = 0;
        pthread_mutex_unlock(&T.mu);
        return 0;
    }
    top = j->last ? j->last : T.nlines;     /* its last line so far */
    gone = T.nlines - RING;                 /* lines up to it: overwritten */
    if (*since < gone - j->first + 1)
        *since = gone - j->first + 1;
    if (*since < 0)
        *since = 0;
    for (i = *since + 1; j->first + i - 1 <= top; i++) {
        int m = snprintf(out + k, n - k, "id: %ld\ndata: %s\n\n", i,
                         T.lines[(j->first + i - 2) % RING]);
        if (m < 0 || (size_t)m >= n - k)
            break;
        k += (size_t)m;
        *since = i;
    }
    if (k < n)
        out[k] = 0;
    *ended = j->last && j->first + *since - 1 >= j->last;
    pthread_mutex_unlock(&T.mu);
    return (long)k;
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* task.h: one ftpoc child at a time, a reader thread splitting its output
 * into numbered lines (on '\n' or '\r': ftpoc's human counters use '\r'),
 * and waiters woken per line. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
    int id, running, exit_code, stopped;
    char op[16], strip[80];
    int nsteps, step;           /* the chain, and the one running */
    char arg[STEPS][ARGS][300];
    int narg[STEPS];
    proc p;
    long nlines;                /* lines numbered 1..nlines so far */
    char lines[RING][LINE];
    char part[LINE];            /* the line being read */
    size_t npart;
} T = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

void task_init(const task_cfg *cfg)
{
    T.cfg = *cfg;
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

/* a good preview: keep its job as strip.preview.job, the planner's
 * starting point (re-plans exclude frames from strip.job) */
static void keep_preview_job(void)
{
    char a[300], b[300];
    snprintf(a, sizeof a, "%s/%s/strip.job", T.cfg.strips, T.strip);
    snprintf(b, sizeof b, "%s/%s/strip.preview.job", T.cfg.strips, T.strip);
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

static void *reader(void *arg)
{
    char buf[4096], end[48], err[300];
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
        if (code != 0 || T.stopped || T.step + 1 >= T.nsteps)
            break;
        if (run_step(T.step + 1, err, sizeof err)) {
            add_line(err, strlen(err));
            code = -1;
            break;
        }
        pthread_mutex_unlock(&T.mu);
    }
    snprintf(end, sizeof end, "ftwebd exit %d", code);
    T.exit_code = code;
    if (code == 0 && strcmp(T.op, "preview") == 0)
        keep_preview_job();
    T.running = 0;
    add_line(end, strlen(end));
    pthread_mutex_unlock(&T.mu);
    return NULL;
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
    char base[300], from[310], job[310], abase[300], ajob[310];
    const char *f = rq->film;
    pthread_t th;
    int ns = 0;

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
    if (T.running) {
        snprintf(err, errn, "scanner busy: %s %s (task %d)", T.op, T.strip,
                 T.id);
        pthread_mutex_unlock(&T.mu);
        return 1;
    }
    /* the task's id names its files: strip_j<id>.job, and ftpoc names
     * the outputs after the job (strip_j<id>_winNN.raw), so a rescan
     * never overwrites an earlier scan */
    snprintf(base, sizeof base, "%s/%s/strip", T.cfg.strips, rq->strip);
    snprintf(from, sizeof from, "%s.job", base);
    snprintf(job, sizeof job, "%s/%s/strip_j%d.job", T.cfg.strips,
             rq->strip, T.id + 1);
    snprintf(abase, sizeof abase, "%s/%s/strip_j%d",
             T.cfg.archive ? T.cfg.archive : "", rq->strip, T.id + 1);
    snprintf(ajob, sizeof ajob, "%s.job", abase);
    if (strcmp(rq->op, "preview") == 0) {
        /* preview + detection, every whole frame planned at 4MP (no
         * 3-point focus check; ~40 s); the page re-plans sizes */
        const char *a[] = { T.cfg.ftpoc, "frames", "--film", f, "--share",
                            "2450", "--progress", "-o", base, NULL };
        set_step(ns++, a);
    } else {
        const char *a[] = { T.cfg.ftpoc, "scan", "--job", job, "--progress",
                            NULL };
        /* ARC: a detailed scan of its own (spec section 9): preview at
         * the scan's optics, focus per frame, then its passes */
        const char *d[] = { T.cfg.ftpoc, "frames", "--detail", "neg",
                            "--ppi", T.cfg.archive_ppi, "--film", f,
                            "--select", rq->arc, "--progress", "-o", abase,
                            NULL };
        const char *b[] = { T.cfg.ftpoc, "scan", "--job", ajob, "--progress",
                            NULL };
        if (rq->share) {
            if (rq->job ? write_file(job, rq->job, rq->njob)
                        : copy_file(from, job)) {
                if (rq->job)
                    snprintf(err, errn, "cannot write %s", job);
                else
                    snprintf(err, errn, "no strip.job to scan (send job=)");
                pthread_mutex_unlock(&T.mu);
                return -1;
            }
            set_step(ns++, a);
        }
        if (rq->arc) {
            set_step(ns++, d);
            set_step(ns++, b);
        }
    }
    T.nsteps = ns;
    T.nlines = 0;
    T.npart = 0;
    if (run_step(0, err, errn)) {
        if (strcmp(rq->op, "scan") == 0 && rq->share)
            remove(job);        /* no task: its job file goes too */
        pthread_mutex_unlock(&T.mu);
        return -1;
    }
    T.id++;
    T.running = 1;
    T.stopped = 0;
    T.exit_code = -1;
    snprintf(T.op, sizeof T.op, "%s", rq->op);
    snprintf(T.strip, sizeof T.strip, "%s", rq->strip);
    *id = T.id;
    if (pthread_create(&th, NULL, reader, NULL)) {
        /* cannot happen in practice; the child runs on unread */
        snprintf(err, errn, "cannot start the reader thread");
        pthread_mutex_unlock(&T.mu);
        return -1;
    }
    pthread_detach(th);
    pthread_mutex_unlock(&T.mu);
    return 0;
}

int task_stop(int id)
{
    int rc = -1;
    pthread_mutex_lock(&T.mu);
    if (id != T.id || !T.id)
        rc = -2;
    else if (T.running && proc_stop(&T.p) == 0) {
        T.stopped = 1;
        rc = 0;
    }
    pthread_mutex_unlock(&T.mu);
    return rc;
}

int task_json(int id, char *out, size_t n)
{
    char op[40], strip[200];
    pthread_mutex_lock(&T.mu);
    if (!T.id || (id && id != T.id)) {
        pthread_mutex_unlock(&T.mu);
        snprintf(out, n, "null");
        return -1;
    }
    json_esc(op, sizeof op, T.op);
    json_esc(strip, sizeof strip, T.strip);
    snprintf(out, n, "{\"id\":%d,\"op\":%s,\"strip\":%s,\"state\":\"%s\","
             "\"exit\":%d,\"stopped\":%s,\"lines\":%ld}", T.id, op, strip,
             T.running ? "running" : "done", T.exit_code,
             T.stopped ? "true" : "false", T.nlines);
    pthread_mutex_unlock(&T.mu);
    return 0;
}

long task_events(int id, long *since, char *out, size_t n, int wait_s,
                 int *ended)
{
    struct timespec ts;
    size_t k = 0;
    long i;
    pthread_mutex_lock(&T.mu);
    if (!T.id || id != T.id) {
        pthread_mutex_unlock(&T.mu);
        return -1;
    }
    ts.tv_sec = time(NULL) + wait_s;
    ts.tv_nsec = 0;
    while (*since >= T.nlines && T.running && id == T.id)
        if (pthread_cond_timedwait(&T.cv, &T.mu, &ts))
            break;
    if (id != T.id) {               /* a new task started meanwhile */
        pthread_mutex_unlock(&T.mu);
        return -1;
    }
    if (*since < T.nlines - RING)
        *since = T.nlines - RING;
    if (*since < 0)
        *since = 0;
    for (i = *since + 1; i <= T.nlines; i++) {
        int m = snprintf(out + k, n - k, "id: %ld\ndata: %s\n\n", i,
                         T.lines[(i - 1) % RING]);
        if (m < 0 || (size_t)m >= n - k)
            break;
        k += (size_t)m;
        *since = i;
    }
    if (k < n)
        out[k] = 0;
    *ended = !T.running && *since >= T.nlines;
    pthread_mutex_unlock(&T.mu);
    return (long)k;
}

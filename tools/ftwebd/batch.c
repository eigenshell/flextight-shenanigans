/* SPDX-License-Identifier: GPL-2.0-or-later */
/* batch.h: BATCH_MAX static slots; ids only grow, so id order is the
 * queue's order */
#include <stdio.h>
#include <string.h>
#include "batch.h"
#include "http.h"

static batch_job B[BATCH_MAX];
static int last_id;

int batch_ended(const batch_job *j)
{
    return j->state >= JOB_DONE;
}

batch_job *batch_add(void)
{
    batch_job *j = NULL;
    int i;
    for (i = 0; i < BATCH_MAX && !j; i++)
        if (!B[i].id)
            j = &B[i];
    if (!j)                             /* the oldest ended job's slot */
        for (i = 0; i < BATCH_MAX; i++)
            if (batch_ended(&B[i]) && (!j || B[i].id < j->id))
                j = &B[i];
    if (!j)
        return NULL;
    memset(j, 0, sizeof *j);
    j->id = ++last_id;
    j->state = JOB_QUEUED;
    j->exit_code = -1;
    return j;
}

batch_job *batch_find(int id)
{
    int i;
    for (i = 0; id > 0 && i < BATCH_MAX; i++)
        if (B[i].id == id)
            return &B[i];
    return NULL;
}

batch_job *batch_next(void)
{
    batch_job *j = NULL;
    int i;
    for (i = 0; i < BATCH_MAX; i++)
        if (B[i].id && B[i].state == JOB_QUEUED && (!j || B[i].id < j->id))
            j = &B[i];
    return j;
}

batch_job *batch_current(void)
{
    batch_job *j = NULL;
    int i;
    for (i = 0; i < BATCH_MAX; i++)
        if (B[i].id && B[i].state == JOB_RUNNING)
            return &B[i];
    for (i = 0; i < BATCH_MAX; i++)
        if (B[i].id && (!j || B[i].id > j->id))
            j = &B[i];
    return j;
}

void batch_drop(batch_job *j)
{
    memset(j, 0, sizeof *j);
}

const char *batch_state(const batch_job *j)
{
    static const char *const name[] = { "queued", "running", "done",
                                        "failed", "stopped" };
    return name[j->state];
}

long batch_json(char *out, size_t n)
{
    char op[40], strip[200];
    size_t k = 0;
    int i, id = 0;
    if (n < 3)
        return -1;
    out[k++] = '[';
    for (;;) {
        const batch_job *j = NULL;
        int m;
        for (i = 0; i < BATCH_MAX; i++)
            if (B[i].id > id && (!j || B[i].id < j->id))
                j = &B[i];
        if (!j)
            break;
        id = j->id;
        json_esc(op, sizeof op, j->op);
        json_esc(strip, sizeof strip, j->strip);
        m = snprintf(out + k, n - k, "%s{\"id\":%d,\"op\":%s,\"strip\":%s,"
                     "\"state\":\"%s\",\"exit\":%d}", k > 1 ? "," : "",
                     j->id, op, strip, batch_state(j), j->exit_code);
        if (m < 0 || (size_t)m >= n - k)
            return -1;
        k += (size_t)m;
    }
    if (k + 2 > n)
        return -1;
    out[k++] = ']';
    out[k] = 0;
    return (long)k;
}

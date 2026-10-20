/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ftio_fake.c — a scripted stand-in for ftio_posix.c, so ftpoc's commands
 * run without a scanner (tests/test_ftpoc_posix.py). Builds ftpoc_fake.
 *
 *   FTIO_FAKE_DEVS  comma-separated personas of /dev/fake0, 1, ...:
 *                   none (absent), foreign (another vendor), dead (no
 *                   INQUIRY answer), loader ("SCSI Loader"), micro (the
 *                   microcode, "FlexTight II M333")
 *   FTIO_FAKE_LOG   every command, one line: "<dev> <dir> <len> <cdb hex>
 *                   [<data hex>]" (data for host -> device only)
 *
 * Answers: INQUIRY by persona, ScannerReady 00 00 (idle), READ BUFFER
 * bytes i & 0xff, SetWindow accepted unchanged. The flood queue is not
 * faked (ioq_open fails): nothing here scans.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ftio.h"

io_dev g_dev = -1;
static const char *g_device;

static const char *persona(int i)
{
    static char buf[32];
    const char *s = getenv("FTIO_FAKE_DEVS");
    int k = 0;
    size_t n;
    if (!s)
        s = "micro";
    while (k < i && s) {
        s = strchr(s, ',');
        if (s)
            s++;
        k++;
    }
    if (!s)
        return NULL;
    n = strcspn(s, ",");
    if (n >= sizeof buf)
        n = sizeof buf - 1;
    memcpy(buf, s, n);
    buf[n] = 0;
    return buf;
}

void io_init(void)
{
}

void io_set_device(const char *path)
{
    g_device = path;
}

int io_dev_path(int i, char *path, size_t cap)
{
    if (g_device) {
        if (i > 0 || strlen(g_device) >= cap)
            return 0;
        strcpy(path, g_device);
        return 1;
    }
    if (!persona(i) || cap < 16)
        return 0;
    sprintf(path, "/dev/fake%d", i);
    return 1;
}

io_dev io_open(const char *path)
{
    int i;
    const char *p;
    if (sscanf(path, "/dev/fake%d", &i) != 1 || !(p = persona(i)) ||
        strcmp(p, "none") == 0)
        return -1;
    return i;
}

void io_close(io_dev d)
{
    (void)d;
}

static void log_cmd(const BYTE *cdb, int cdb_len, BYTE dir, const BYTE *data,
                    DWORD len)
{
    const char *path = getenv("FTIO_FAKE_LOG");
    FILE *f;
    DWORD i;
    if (!path || !(f = fopen(path, "a")))
        return;
    fprintf(f, "%d %02x %lu ", g_dev, dir, (unsigned long)len);
    for (i = 0; i < (DWORD)cdb_len; i++)
        fprintf(f, "%02x", cdb[i]);
    if (dir == SRB_DIR_OUT) {
        fputc(' ', f);
        for (i = 0; i < len; i++)
            fprintf(f, "%02x", data[i]);
    }
    fputc('\n', f);
    fclose(f);
}

int srb_exec(const BYTE *cdb, int cdb_len, BYTE dir,
             BYTE *data, DWORD data_len, DWORD timeout_ms)
{
    static const BYTE head[8] = { 0x06, 0x00, 0x02, 0x02, 0x1f, 0, 0, 0 };
    const char *p = persona(g_dev);
    DWORD i;
    (void)timeout_ms;
    log_cmd(cdb, cdb_len, dir, data, data_len);
    if (!p)
        return -1;
    if (cdb[0] == 0x12) {
        const char *id;
        if (strcmp(p, "dead") == 0)
            return -3;
        id = strcmp(p, "loader") == 0 ? "Imacon  SCSI Loader     L302" :
             strcmp(p, "micro") == 0  ? "Imacon  FlexTight II    M333" :
                                        "ACME    Flatbed 9000    1.00";
        memset(data, 0, data_len);
        memcpy(data, head, 8);
        memcpy(data + 8, id, 28);
        return 36;
    }
    if (dir == SRB_DIR_IN) {
        if (cdb[0] == 0xdf) {
            memset(data, 0, data_len);
            return (int)data_len;
        }
        for (i = 0; i < data_len; i++)
            data[i] = (BYTE)i;
        return (int)data_len;
    }
    return (int)data_len;
}

const char *srb_errstr(int r)
{
    static char buf[32];
    sprintf(buf, "fake code %d", r);
    return buf;
}

io_queue *ioq_open(int depth, const BYTE cdb[10], WORD blklen,
                   DWORD timeout_ms)
{
    (void)depth; (void)cdb; (void)blklen; (void)timeout_ms;
    return NULL;
}

BYTE *ioq_buf(io_queue *q, int slot)
{
    (void)q; (void)slot;
    return NULL;
}

int ioq_issue(io_queue *q, int slot, long seq)
{
    (void)q; (void)slot; (void)seq;
    return -1;
}

int ioq_reap(io_queue *q, int slot, long seq, DWORD timeout_ms, DWORD *got)
{
    (void)q; (void)slot; (void)seq; (void)timeout_ms;
    *got = 0;
    return IOQ_ERROR;
}

void ioq_close(io_queue *q, int cancel)
{
    (void)q; (void)cancel;
}

int io_busreset(void)
{
    printf("fake: no bus reset\n");
    return 1;
}

void io_catch_interrupt(volatile int *flag)
{
    (void)flag;
}

DWORD io_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)((unsigned long long)ts.tv_sec * 1000u +
                   (unsigned long long)ts.tv_nsec / 1000000u);
}

void io_sleep(DWORD ms)
{
    (void)ms;
}

void io_exe_path(char exe[IO_PATH_MAX + 1])
{
    exe[0] = 0;
}

int io_replace(const char *tmp, const char *path)
{
    return rename(tmp, path) ? -1 : 0;
}

int io_free_bytes(const char *dir, double *bytes)
{
    (void)dir;
    *bytes = 1e12;
    return 0;
}

const char *io_syserr(void)
{
    return "fake";
}

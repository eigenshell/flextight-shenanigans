/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ftio_fake.c — a scripted stand-in for ftio_posix.c, so ftpoc's commands
 * run without a scanner (tests/test_ftpoc_posix.py). Builds ftpoc_fake.
 *
 *   FTIO_FAKE_DEVS  comma-separated personas of /dev/fake0, 1, ...:
 *                   none (absent), foreign (another vendor), dead (no
 *                   INQUIRY answer), loader ("SCSI Loader"), micro (the
 *                   microcode, "FlexTight II M333"), micro-empty (the
 *                   microcode, its polls come back empty), micro-scan
 *                   (the microcode, scanning: see below)
 *   FTIO_FAKE_LOG   every command, one line: "<dev> <dir> <len> <cdb hex>
 *                   [<data hex>]" (data for host -> device only)
 *
 * Answers: INQUIRY by persona, ScannerReady 00 00 (idle), READ BUFFER
 * bytes i & 0xff, SetWindow accepted unchanged. The flood queue fails to
 * open, except for micro-scan: there the flood's reads alternate, as on
 * Linux, an empty read (an underrun bringing the 10 bytes ff ff ff ff ff
 * ff 00 00 00 00) and a full line (bytes 0x01, line k's first byte k &
 * 0x7f), and one ScannerReady after every SetWindow and every flood says
 * 08 00 (busy) so the settle and carriage waits go on. Time is virtual:
 * sleep_ms moves now_ms on.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ftio.h"

io_scanner_handle g_scanner = -1;
static const char* g_device;
static DWORD g_now;  /* virtual ms: sleep_ms moves it */
static int g_busy;   /* micro-scan: next poll is busy */
static long g_lines; /* micro-scan: full lines so far */

static const char* persona(int i) {
  static char buf[32];
  const char* s = getenv("FTIO_FAKE_DEVS");
  int k = 0;
  size_t n;
  if (!s) s = "micro";
  while (k < i && s) {
    s = strchr(s, ',');
    if (s) s++;
    k++;
  }
  if (!s) return NULL;
  n = strcspn(s, ",");
  if (n >= sizeof buf) n = sizeof buf - 1;
  memcpy(buf, s, n);
  buf[n] = 0;
  return buf;
}

void os_layer_init(void) {}

void scanner_set_path(const char* path) { g_device = path; }

int scanner_candidate_path(int i, char* path, size_t cap) {
  if (g_device) {
    if (i > 0 || strlen(g_device) >= cap) return 0;
    strcpy(path, g_device);
    return 1;
  }
  if (!persona(i) || cap < 16) return 0;
  sprintf(path, "/dev/fake%d", i);
  return 1;
}

io_scanner_handle scanner_open(const char* path) {
  int i;
  const char* p;
  if (sscanf(path, "/dev/fake%d", &i) != 1 ||
      !(p = persona(i)) ||
      strcmp(p, "none") == 0)
    return -1;
  return i;
}

void scanner_close(io_scanner_handle d) { (void)d; }

static void log_cmd(io_scanner_handle device, const BYTE* cdb, int cdb_len, BYTE dir,
                    const BYTE* data, DWORD len) {
  const char* path = getenv("FTIO_FAKE_LOG");
  FILE* f;
  DWORD i;
  if (!path || !(f = fopen(path, "a"))) return;
  fprintf(f, "%d %02x %lu ", device, dir, (unsigned long)len);
  for (i = 0; i < (DWORD)cdb_len; i++) fprintf(f, "%02x", cdb[i]);
  if (dir == SCSI_TO_DEVICE) {
    fputc(' ', f);
    for (i = 0; i < len; i++) fprintf(f, "%02x", data[i]);
  }
  fputc('\n', f);
  fclose(f);
}

int io_send_command_to(io_scanner_handle device, const BYTE* cdb, int cdb_len, BYTE dir,
                       BYTE* data, DWORD data_len, DWORD timeout_ms) {
  static const BYTE head[8] = {0x06, 0x00, 0x02, 0x02, 0x1f, 0, 0, 0};
  const char* p = persona(device);
  DWORD i;
  (void)timeout_ms;
  log_cmd(device, cdb, cdb_len, dir, data, data_len);
  if (!p) return -1;
  if (cdb[0] == 0x12) {
    const char* id;
    if (strcmp(p, "dead") == 0) return -3;
    id = strcmp(p, "loader") == 0      ? "Imacon  SCSI Loader     L302"
         : strncmp(p, "micro", 5) == 0 ? "Imacon  FlexTight II    M333"
                                       : "ACME    Flatbed 9000    1.00";
    memset(data, 0, data_len);
    memcpy(data, head, 8);
    memcpy(data + 8, id, 28);
    return 36;
  }
  if (dir == SCSI_FROM_DEVICE) {
    if (cdb[0] == 0xdf) {
      memset(data, 0, data_len);
      if (g_busy && data_len >= 1) {
        data[0] = 0x08;
        g_busy = 0;
      }
      return strcmp(p, "micro-empty") == 0 ? 0 : (int)data_len;
    }
    for (i = 0; i < data_len; i++) data[i] = (BYTE)i;
    return (int)data_len;
  }
  if (cdb[0] == 0x24 && strcmp(p, "micro-scan") == 0) g_busy = 1;
  return (int)data_len;
}

int scsi_send_command(const BYTE* cdb, int cdb_len, BYTE dir, BYTE* data, DWORD data_len,
             DWORD timeout_ms) {
  return io_send_command_to(g_scanner, cdb, cdb_len, dir, data, data_len, timeout_ms);
}

const char* scsi_error_text(int r) {
  static char buf[32];
  if (r == SCSI_SHORT_REPLY) return "short reply (underrun)";
  sprintf(buf, "fake code %d", r);
  return buf;
}

struct read_queue {
  BYTE* buf;
  WORD blklen;
  long seq;
};

read_queue* read_queue_open(int depth, const BYTE cdb[10], WORD blklen,
                   DWORD timeout_ms) {
  const char* p = persona(g_scanner);
  read_queue* q;
  (void)depth;
  (void)timeout_ms;
  log_cmd(g_scanner, cdb, 10, SCSI_FROM_DEVICE, NULL, blklen);
  if (!p || strcmp(p, "micro-scan") != 0 || !(q = calloc(1, sizeof *q)))
    return NULL;
  q->blklen = blklen;
  if (!(q->buf = malloc(blklen))) {
    free(q);
    return NULL;
  }
  return q;
}

BYTE* read_queue_buffer(read_queue* q, int slot) {
  (void)slot;
  return q->buf; /* one buffer: reaped before the next reap */
}

int read_queue_issue(read_queue* q, int slot, long seq) {
  (void)q;
  (void)slot;
  (void)seq;
  return 0;
}

int read_queue_wait(read_queue* q, int slot, long seq, DWORD timeout_ms, DWORD* got) {
  static const BYTE empty[10] = {0xff, 0xff, 0xff, 0xff, 0xff,
                                 0xff, 0,    0,    0,    0};
  (void)slot;
  (void)timeout_ms;
  if (seq % 2 == 0) {
    memcpy(q->buf, empty, sizeof empty);
    *got = sizeof empty;
    return READ_SHORT;
  }
  memset(q->buf, 0x01, q->blklen);
  q->buf[0] = (BYTE)(g_lines++ & 0x7f);
  *got = q->blklen;
  return READ_FULL;
}

void read_queue_close(read_queue* q, int cancel) {
  (void)cancel;
  if (!q) return;
  g_busy = 1; /* the carriage moves after a batch */
  free(q->buf);
  free(q);
}

int scsi_bus_reset(void) {
  printf("fake: no bus reset\n");
  return 1;
}

void catch_ctrl_c(volatile int* flag) { (void)flag; }

DWORD
now_ms(void) { return g_now; }

void sleep_ms(DWORD ms) { g_now += ms; }

void own_exe_path(char exe[PATH_BUF_MAX + 1]) { exe[0] = 0; }

int replace_file(const char* tmp, const char* path) {
  return rename(tmp, path) ? -1 : 0;
}

int disk_free_bytes(const char* dir, double* bytes) {
  (void)dir;
  *bytes = 1e12;
  return 0;
}

const char* os_error_text(void) { return "fake"; }

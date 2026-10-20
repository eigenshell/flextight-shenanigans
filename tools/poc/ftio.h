/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ftio.h — ftpoc's OS layer: the scanner transport and the few OS calls
 * around it. One header, two implementations:
 *
 *   ftio_win.c    Windows: the scsiscan.sys 44-byte SRB + ioctl 0x190012
 *                 that FlexColor uses, overlapped (build.bat)
 *   ftio_posix.c  Linux: SG_IO on /dev/sgN (Makefile)
 *
 * Everything that decides WHAT goes on the wire (CDBs, descriptors, the
 * loader-vs-microcode choice, the flood's line counting) stays in
 * ftpoc.c; this layer only moves the bytes and reports how it went, in
 * ftpoc's own codes (srb_exec: bytes, or -1 rejected, -2 timed out, -3
 * completed with an error).
 */
#ifndef FTIO_H
#define FTIO_H

#include <stddef.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef HANDLE io_dev;
#define IO_NONE INVALID_HANDLE_VALUE
#define IO_PATH_MAX MAX_PATH
#define IO_DEV_NAME "\\\\.\\ScannerN"
#else
#include <stdint.h>
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;     /* 32 bits, so io_ms() differences wrap as
                               GetTickCount()'s do */
typedef int io_dev;
#define IO_NONE (-1)
#define IO_PATH_MAX 4096
#define IO_DEV_NAME "/dev/sgN"
#endif

#define SRB_DIR_OUT 0x80        /* host -> device */
#define SRB_DIR_IN  0x40        /* device -> host */

/* The open scanner (IO_NONE: none). */
extern io_dev g_dev;

/* Once, before anything else; exits on failure. */
void io_init(void);

/* --device PATH: probe only this device (NULL: every candidate). */
void io_set_device(const char *path);

/* The i-th device to probe (i = 0, 1, ...) into path; 0 past the last. */
int io_dev_path(int i, char *path, size_t cap);

/* Open one candidate for srb_exec (IO_NONE: absent, not the scanner's
 * kind, or busy; a busy or forbidden scanner is reported on stderr). */
io_dev io_open(const char *path);
void io_close(io_dev d);

/* One command to g_dev; dir SRB_DIR_IN / SRB_DIR_OUT, or 0 with
 * data_len 0 (no data phase). Returns the bytes transferred, or
 * -1 rejected, -2 timed out, -3 completed with an error (srb_errstr).
 * IO_SHORT is the callers' own: a reply shorter than it must be (POSIX
 * only; srb_errstr names it). */
#define IO_SHORT (-4)
int srb_exec(const BYTE *cdb, int cdb_len, BYTE dir,
             BYTE *data, DWORD data_len, DWORD timeout_ms);
const char *srb_errstr(int r);

/* The flood's queue: depth ReadExt-style data-in commands (cdb, blklen
 * bytes each) in flight at once, issued and reaped in order by slot
 * (seq % depth). Each slot owns its buffer. */
typedef struct io_queue io_queue;
enum { IOQ_FULL = 0, IOQ_SHORT = 1, IOQ_TIMEOUT = -2, IOQ_ERROR = -3 };
io_queue *ioq_open(int depth, const BYTE cdb[10], WORD blklen,
                   DWORD timeout_ms);
BYTE *ioq_buf(io_queue *q, int slot);
/* 0 queued, -1 refused (printed as "flood issue <seq>: ...") */
int ioq_issue(io_queue *q, int slot, long seq);
/* Wait up to timeout_ms for the slot's command. IOQ_FULL: complete,
 * IOQ_SHORT: an underrun (normal: the host is ahead of the CCD; *got
 * bytes, maybe 0, never line data: Linux's empty read has 10), IOQ_TIMEOUT, IOQ_ERROR (printed as "flood read
 * <seq>: ..."). */
int ioq_reap(io_queue *q, int slot, long seq, DWORD timeout_ms, DWORD *got);
/* Free the queue; cancel: first cancel or drain what is still in flight */
void ioq_close(io_queue *q, int cancel);

/* busreset: SCSI bus reset of the bus that hosts the scanner (refused
 * when anything else is on it). 0 issued. */
int io_busreset(void);

/* Ctrl+C (and SIGTERM on POSIX): set *flag instead of ending the process. */
void io_catch_interrupt(volatile int *flag);

/* Monotonic milliseconds (wraps at 2^32) and a sleep. */
DWORD io_ms(void);
void io_sleep(DWORD ms);

/* This executable's path into exe ("" if unknown). */
void io_exe_path(char exe[IO_PATH_MAX + 1]);
/* Atomically replace path by tmp: 0, or -1 (io_syserr says why). */
int io_replace(const char *tmp, const char *path);
/* Free bytes for the caller in dir (NULL: the current directory): 0, or
 * -1 (io_syserr says why). */
int io_free_bytes(const char *dir, double *bytes);
/* The last OS error, as ftpoc prints it ("GLE=5" / "errno 13: ..."). */
const char *io_syserr(void);

#endif

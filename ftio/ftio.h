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
 * ftpoc's own codes (scsi_send_command: bytes, or -1 rejected, -2 timed out, -3
 * completed with an error).
 */
#ifndef FTIO_H
#define FTIO_H

#include <stddef.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef HANDLE io_scanner_handle;
#define NO_SCANNER INVALID_HANDLE_VALUE
#define PATH_BUF_MAX MAX_PATH
#define SCANNER_DEVICE_NAME "\\\\.\\ScannerN"
#else
#include <stdint.h>
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD; /* 32 bits, so now_ms() differences wrap as
                           GetTickCount()'s do */
typedef int io_scanner_handle;
#define NO_SCANNER (-1)
#define PATH_BUF_MAX 4096
#define SCANNER_DEVICE_NAME "/dev/sgN"
#endif

#define SCSI_TO_DEVICE 0x80   /* host -> device */
#define SCSI_FROM_DEVICE 0x40 /* device -> host */

/* The open scanner (NO_SCANNER: none). */
extern io_scanner_handle g_scanner;

/* Once, before anything else; exits on failure. */
void os_layer_init(void);

/* --device PATH: probe only this device (NULL: every candidate). */
void scanner_set_path(const char* path);

/* The i-th device to probe (i = 0, 1, ...) into path; 0 past the last. */
int scanner_candidate_path(int i, char* path, size_t cap);

/* Open one candidate for scsi_send_command (NO_SCANNER: absent, not the
 * scanner's kind, or busy; a busy or forbidden scanner is reported on stderr).
 */
io_scanner_handle scanner_open(const char* path);
void scanner_close(io_scanner_handle d);

/* One command to g_scanner; dir SCSI_FROM_DEVICE / SCSI_TO_DEVICE, or 0 with
 * data_len 0 (no data phase). Returns the bytes transferred, or
 * -1 rejected, -2 timed out, -3 completed with an error (scsi_error_text).
 * SCSI_SHORT_REPLY is the callers' own: a reply shorter than it must be (POSIX
 * only; scsi_error_text names it). */
#define SCSI_SHORT_REPLY (-4)
int scsi_send_command(const BYTE* cdb, int cdb_len, BYTE dir, BYTE* data,
                      DWORD data_len, DWORD timeout_ms);
/* Same as scsi_send_command, but to device instead of g_scanner
 * (used while probing candidates, before one is known to be the scanner). */
int io_send_command_to(io_scanner_handle device, const BYTE* cdb, int cdb_len,
                       BYTE dir, BYTE* data, DWORD data_len, DWORD timeout_ms);
const char* scsi_error_text(int r);

/* The flood's queue: depth ReadExt-style data-in commands (cdb, blklen
 * bytes each) in flight at once, issued and reaped in order by slot
 * (seq % depth). Each slot owns its buffer. */
typedef struct read_queue read_queue;
enum { READ_FULL = 0, READ_SHORT = 1, READ_TIMEOUT = -2, READ_ERROR = -3 };
read_queue* read_queue_open(int depth, const BYTE cdb[10], WORD blklen,
                            DWORD timeout_ms);
BYTE* read_queue_buffer(read_queue* q, int slot);
/* 0 queued, -1 refused (printed as "flood issue <seq>: ...") */
int read_queue_issue(read_queue* q, int slot, long seq);
/* Wait up to timeout_ms for the slot's command. READ_FULL: complete,
 * READ_SHORT: an underrun (normal: the host is ahead of the CCD; *got
 * bytes, maybe 0, never line data: Linux's empty read has 10), READ_TIMEOUT,
 * READ_ERROR (printed as "flood read <seq>: ..."). */
int read_queue_wait(read_queue* q, int slot, long seq, DWORD timeout_ms,
                    DWORD* got);
/* Free the queue; cancel: first cancel or drain what is still in flight */
void read_queue_close(read_queue* q, int cancel);

/* busreset: SCSI bus reset of the bus that hosts the scanner (refused
 * when anything else is on it). 0 issued. */
int scsi_bus_reset(void);

/* Ctrl+C (and SIGTERM on POSIX): set *flag instead of ending the process. */
void catch_ctrl_c(volatile int* flag);

/* Monotonic milliseconds (wraps at 2^32) and a sleep. */
DWORD now_ms(void);
void sleep_ms(DWORD ms);

/* This executable's path into exe ("" if unknown). */
void own_exe_path(char exe[PATH_BUF_MAX + 1]);
/* Atomically replace path by tmp: 0, or -1 (os_error_text says why). */
int replace_file(const char* tmp, const char* path);
/* Free bytes for the caller in dir (NULL: the current directory): 0, or
 * -1 (os_error_text says why). */
int disk_free_bytes(const char* dir, double* bytes);
/* The last OS error, as ftpoc prints it ("GLE=5" / "errno 13: ..."). */
const char* os_error_text(void);

io_scanner_handle io_open_candidate(
  uint32_t index); /* NO_SCANNER if absent or unusable */
uint32_t io_candidate_count(void);

#endif

/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SC_LIVE_H
#define SC_LIVE_H

#include <stdint.h>

#include "ft.h"

/* Live mode: change the scanner's memory while a scan is running
 * (notes/handoff-live-mode-focus-test.md). For now only the first step,
 * test H10: write a few bytes and read them back.
 *
 * Every function talks to the open scanner (g_scanner, see ftio.h). */

/* How long to stay off the bus after a write, in milliseconds. The
 * scanner runs a write (0x3b) later, in its main loop. If another command
 * (even a status poll or a read) arrives before that, the write is lost.
 * The handoff says "a few ms"; ftpoc maint waits 3000 ms when idle. */
#define SC_LIVE_DEFAULT_SETTLE_MS 10

/* INQUIRY byte 0x35 is the microcode's scan state (verified, test H1). */
#define SC_LIVE_STATE_IDLE 8
#define SC_LIVE_STATE_SCANNING 36
/* sc_live_read_scan_state's answer when the scanner did not tell us */
#define SC_LIVE_STATE_UNKNOWN (-1)

/* How a poke-and-verify went. */
typedef enum {
  /* written, and the same bytes read back */
  SC_LIVE_OK,
  /* not on the live whitelist (ft_build_live_write): nothing was sent */
  SC_LIVE_REFUSED,
  /* the write command itself failed on the bus */
  SC_LIVE_SEND_FAILED,
  /* the write went out, but reading it back failed */
  SC_LIVE_PEEK_FAILED,
  /* the read-back bytes differ: the write was lost or not run (yet) */
  SC_LIVE_MISMATCH
} sc_live_result;

/* Asks the scanner (INQUIRY) for its scan state, byte 0x35:
 * SC_LIVE_STATE_IDLE, SC_LIVE_STATE_SCANNING, or another state number.
 * Returns SC_LIVE_STATE_UNKNOWN when INQUIRY failed or its answer was too
 * short to hold byte 0x35. */
int32_t sc_live_read_scan_state(void);

/* Writes length bytes to scanner memory 4000:offset, waits settle_ms,
 * then reads the same bytes back and compares them.
 *
 * offset      address inside segment 4000; offset/length must be on the
 *             ft_build_live_write whitelist
 * data        the bytes to write, in memory order (little-endian values)
 * length      how many bytes (1..4)
 * read_back   receives the bytes read back (length bytes); filled on
 *             SC_LIVE_OK and SC_LIVE_MISMATCH
 * scsi_code   receives the failing ftio code (scsi_error_text) on
 *             SC_LIVE_SEND_FAILED / SC_LIVE_PEEK_FAILED, else 0
 * error       receives the reason on SC_LIVE_REFUSED
 * settle_ms   bus silence between write and read back; callers normally
 *             pass SC_LIVE_DEFAULT_SETTLE_MS
 *
 * Sends nothing at all when the write is refused. Does not check the scan
 * state: the caller decides when a write is safe. */
sc_live_result sc_live_poke_and_verify(uint32_t offset, const uint8_t* data,
                                       uint32_t length, uint8_t* read_back,
                                       int32_t* scsi_code, ft_err* error,
                                       uint32_t settle_ms);

#endif

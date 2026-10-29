/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Microcode maintenance packets.
 *
 * Writes: WRITE BUFFER (0x3b) whose data is a 10-byte header + payload.
 * Header: [0] command, [1..2] W1 big-endian, [3..4] W2 big-endian,
 * [9] flags. Command 0x01 writes memory W1:W2 (flags bit 0 clear) or I/O
 * port W1 (bit 0 set; bit 2 = keep the port). The device copies
 * length - 10 bytes without checking, so every packet is longer than the
 * header.
 *
 * Only a whitelist can be built: one byte to I/O port 0x342 or 0x344, and
 * memory writes inside the timing table 4000:7640..4000:76BB. Jump (0x00),
 * FPGA load (0x02), flash (0x10/0x20/0x30/0x50), other ports and other
 * memory are refused.
 *
 * Reads: READ BUFFER (0x3c) is a memory read, CDB bytes 2..3 segment,
 * 4..5 offset, 6..8 length (all big-endian). Allowed: segments 3000
 * (microcode), 4000 (data), C000:0000..7FFF (loader ROM, chip select 3:
 * the reset stub ends in a jump to C000:3CDF), E000 and F000 (upper chip
 * select, enabled by the handler first: parameter flash with Faul Pixs at
 * E000:0400, reset stub at F000:C000), 1..FT_MAINT_MAX_PEEK_BYTES bytes, no
 * wrap. C800..D7FF (chip selects 0 and 1, unidentified devices) stays
 * refused.
 *
 * Live writes (ft_build_live_write) are a second, separate whitelist for
 * "live mode" (notes/handoff-live-mode-focus-test.md): a few exact
 * addresses in segment 4000 that the microcode reads while it scans.
 * They do not widen the timing-table range above.
 *
 * See tools/poc/README.md, "Maintenance writes and memory reads". */
#ifndef FT_MAINT_H
#define FT_MAINT_H

#include <stddef.h>
#include <stdint.h>

#include "ft.h"

/* the packet header at the start of every WRITE BUFFER's data */
#define FT_MAINT_HEADER_BYTES 10
/* the most payload bytes one memory write carries */
#define FT_MAINT_MAX_DATA_BYTES 0x7c
/* the most bytes one memory read returns */
#define FT_MAINT_MAX_PEEK_BYTES 0x400
/* the loader ROM's size, from C000:0000 */
#define FT_MAINT_LOADER_BYTES 0x8000u
/* the timing table: segment 4000, offsets 7640 up to (not including) 76bc */
#define FT_MAINT_TABLE_SEGMENT 0x4000u
#define FT_MAINT_TABLE_START 0x7640u
#define FT_MAINT_TABLE_END 0x76bcu

/* Build a one-byte write to an I/O port: 0x342 or 0x344 only.
 *
 * command   receives the 10-byte SCSI command (CDB)
 * packet    receives the header and the byte; capacity is its size
 *
 * Returns the packet length (FT_MAINT_HEADER_BYTES + 1), or -1 with error
 * filled when the port is not allowed or packet is too small. */
int32_t ft_build_port_write(uint32_t port, uint8_t value, uint8_t command[10],
                            uint8_t* packet, size_t capacity, ft_err* error);

/* Build a memory write inside the timing table (segment 4000,
 * FT_MAINT_TABLE_START..FT_MAINT_TABLE_END).
 *
 * data, length  the bytes to write, 1..FT_MAINT_MAX_DATA_BYTES
 *
 * Returns the packet length (FT_MAINT_HEADER_BYTES + length), or -1 with
 * error filled when the write is outside the table or packet is too
 * small. */
int32_t ft_build_memory_write(uint32_t segment, uint32_t offset,
                              const uint8_t* data, size_t length,
                              uint8_t command[10], uint8_t* packet,
                              size_t capacity, ft_err* error);

/* Build a READ BUFFER that reads length bytes at segment:offset.
 * Returns 0, or -1 with error filled when the read is not allowed. */
int32_t ft_build_memory_read(uint32_t segment, uint32_t offset, size_t length,
                             uint8_t command[10], ft_err* error);

/* Build one live-mode memory write: the same WRITE BUFFER (0x3b) packet
 * as ft_build_memory_write, always to segment 4000. Only these exact
 * writes are allowed (offset in segment 4000, length in bytes):
 *
 *   7557  1 or 2   drum seek mode (7557), drum enabled (7558)
 *   8ED4  4        drum target
 *   8EDC  4        CCD target
 *   8EE4  4        lens target
 *   8EEE  4        stored but never read by the microcode: a harmless
 *                  target for testing that a write arrives
 *
 * offset          address inside segment 4000
 * data, length    the bytes to write, in the order they go to memory
 *                 (the scanner is little-endian: lowest byte first)
 * command         receives the 10-byte command
 * packet          receives the header + data; capacity is its size
 *
 * Returns the packet length (FT_MAINT_HEADER_BYTES + length), or -1 with
 * error filled when the write is not on the list or packet is too
 * small. */
int32_t ft_build_live_write(uint32_t offset, const uint8_t* data, size_t length,
                            uint8_t command[10], uint8_t* packet,
                            size_t capacity, ft_err* error);

#endif

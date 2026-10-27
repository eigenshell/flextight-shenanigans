/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FT_MAINT_H
#define FT_MAINT_H
#include <stddef.h>

#include "ft.h"

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
 * E000:0400, reset stub at F000:C000), 1..FT_MAINT_MAX_PEEK bytes, no
 * wrap. C800..D7FF (chip selects 0 and 1, unidentified devices) stays
 * refused.
 *
 * See tools/poc/README.md, "Maintenance writes and memory reads". */
#define FT_MAINT_HDR 10
#define FT_MAINT_MAX_DATA 0x7c
#define FT_MAINT_MAX_PEEK 0x400
#define FT_MAINT_LOADER_LEN 0x8000u /* C000:0000.. */
#define FT_MAINT_TABLE_SEG 0x4000u
#define FT_MAINT_TABLE_LO 0x7640u
#define FT_MAINT_TABLE_END 0x76bcu /* exclusive */

/* Both fill cdb[10] and buf (cap bytes) and return the data length
 * (FT_MAINT_HDR + payload), or -1 with e filled. */
int ft_maint_io(unsigned port, unsigned char value, unsigned char cdb[10],
                unsigned char* buf, size_t cap, ft_err* e);
int ft_maint_mem(unsigned seg, unsigned off, const unsigned char* data,
                 size_t n, unsigned char cdb[10], unsigned char* buf,
                 size_t cap, ft_err* e);

/* Fills cdb[10] for reading n bytes at seg:off; 0, or -1 with e filled. */
int ft_maint_peek(unsigned seg, unsigned off, size_t n, unsigned char cdb[10],
                  ft_err* e);

#endif

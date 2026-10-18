/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FT_UPLOAD_H
#define FT_UPLOAD_H
#include <stddef.h>
#include "ft.h"

/* MICROCOD.3XX upload to the resident loader, byte for byte as FlexColor
 * sends it (session 2/3 traces). Same packet format as ft_maint.h: each
 * WRITE BUFFER carries a 10-byte header in its data. Chunk i (0..15) is
 * command 0x01, memory write to 0x3i00:0000 (the 64 KiB at linear
 * 0x30000), header + 4096 code bytes. The trailer is command 0x00, run
 * from 3001:000E, header only.
 *
 * The header is in the data, not the CDB: data byte 0 is the command, so
 * a bare code chunk would be parsed as a command (chunk 0 starts with
 * 00 00 00 00 00 = run from 0000:0000). */
#define FT_UPLOAD_CHUNK 4096
#define FT_UPLOAD_CHUNKS 16

/* Fills cdb[10] and buf (cap bytes); returns the data length, or -1 with
 * e filled. */
int ft_upload_chunk(int i, const unsigned char code[FT_UPLOAD_CHUNK],
                    unsigned char cdb[10], unsigned char *buf, size_t cap,
                    ft_err *e);
int ft_upload_trailer(unsigned char cdb[10], unsigned char *buf, size_t cap,
                      ft_err *e);

/* 1 if the INQUIRY data (n bytes) names the loader ("SCSI Loader" at
 * product byte 16), the only state in which FlexColor uploads. */
int ft_upload_is_loader(const unsigned char *inq, size_t n);

/* Where ftpoc looks for the microcode when the loader answers, in order:
 * the given path alone (--microcode; no other place is tried), else
 * FT_UPLOAD_FILE in the directory of exe (the running program's own
 * path, '\\' or '/' separators; NULL or no directory: skipped), then
 * FT_UPLOAD_FILE in the current directory. Fills out[0..) and returns
 * how many (0..2); a path that does not fit FT_UPLOAD_PATH is left out
 * (0: the given path is too long or empty). */
#define FT_UPLOAD_FILE "MICROCOD.3XX"
#define FT_UPLOAD_PATH 520
int ft_upload_paths(const char *given, const char *exe,
                    char out[2][FT_UPLOAD_PATH]);

#endif

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* MICROCOD.3XX upload to the scanner's resident loader, byte for byte as
 * FlexColor sends it (session 2/3 traces).
 *
 * The packet format is the same as ft_maint.h: each WRITE BUFFER carries
 * a 10-byte header in its data. Chunk i (0..15) is command 0x01, a memory
 * write to 0x3i00:0000 (the 64 KiB at linear 0x30000): header + 4096 code
 * bytes. The trailer is command 0x00, run from 3001:000E, header only.
 *
 * The header is in the data, not the CDB: data byte 0 is the command, so
 * a bare code chunk would be read as a command (chunk 0 starts with
 * 00 00 00 00 00 = run from 0000:0000). */
#ifndef FT_UPLOAD_H
#define FT_UPLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ft.h"

/* code bytes in one chunk */
#define FT_UPLOAD_CHUNK_BYTES 4096
/* chunks in the microcode: 16 x 4096 = 64 KiB */
#define FT_UPLOAD_CHUNK_COUNT 16
/* the microcode's file name */
#define FT_MICROCODE_FILE "MICROCOD.3XX"
/* the longest path ft_find_microcode_paths returns, NUL included */
#define FT_UPLOAD_PATH_BYTES 520

/* Build the WRITE BUFFER for one chunk of microcode.
 *
 * chunk_index    which chunk, 0..15
 * code           the chunk's 4096 code bytes
 * command        receives the 10-byte SCSI command (CDB)
 * data           receives the header and the code
 * data_capacity  bytes available in data
 *
 * Returns the data length, or -1 when chunk_index is out of range or
 * data is too small. */
int32_t ft_build_upload_chunk(uint32_t chunk_index,
                              const uint8_t code[FT_UPLOAD_CHUNK_BYTES],
                              uint8_t command[10], uint8_t* data,
                              size_t data_capacity, ft_err* error);

/* Build the WRITE BUFFER that starts the uploaded microcode. The same
 * arguments and result as ft_build_upload_chunk, without the code. */
int32_t ft_build_upload_trailer(uint8_t command[10], uint8_t* data,
                                size_t data_capacity, ft_err* error);

/* Is this INQUIRY answer from the loader ("SCSI Loader" at product
 * byte 16)? Only then does FlexColor upload the microcode.
 *
 * inquiry         the INQUIRY data
 * inquiry_length  its length in bytes */
bool ft_is_loader(const uint8_t* inquiry, size_t inquiry_length);

/* List where to look for the microcode when the loader answers, in order:
 * - given alone, when it is not NULL (--microcode; nothing else is tried)
 * - FT_MICROCODE_FILE in the directory of program_path (the running
 *   program's own path, '\\' or '/' separators); skipped when
 *   program_path is NULL or has no directory
 * - FT_MICROCODE_FILE in the current directory
 *
 * paths  receives the paths. A path that does not fit
 *        FT_UPLOAD_PATH_BYTES is left out.
 *
 * Returns how many paths were filled in, 0..2 (0: given is too long or
 * empty). */
uint32_t ft_find_microcode_paths(const char* given, const char* program_path,
                                 char paths[2][FT_UPLOAD_PATH_BYTES]);

#endif

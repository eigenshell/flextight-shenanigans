/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A store-only zip archive in memory (Quixote's "All": every frame's JPEG
 * and its .txt). Pure: heap only, no files, so it runs in the browser.
 *
 * No compression (JPEGs do not shrink), no zip64 (an archive stays under
 * 4 GB), and every entry is dated 1980-01-01 00:00, so the bytes depend
 * only on the contents. */
#ifndef FT_ZIP_H
#define FT_ZIP_H

#include <stddef.h>
#include <stdint.h>

#include "ft.h"

/* An archive being built. Start it with ft_init_zip. */
typedef struct ft_zip {
  /* the local entries: header, name and data of every file */
  uint8_t* entries;
  /* bytes used in entries */
  size_t entries_length;
  /* bytes allocated for entries */
  size_t entries_capacity;
  /* the central directory: one record per file, written at the end */
  uint8_t* directory;
  /* bytes used in directory */
  size_t directory_length;
  /* bytes allocated for directory */
  size_t directory_capacity;
  /* files added so far */
  uint32_t entry_count;
} ft_zip;

/* Continue a CRC-32 (ISO 3309, as zip and PNG use it) over length more
 * bytes. Start with crc = 0. */
uint32_t ft_compute_crc32(uint32_t crc, const void* bytes, size_t length);

/* Start an empty archive. */
void ft_init_zip(ft_zip* zip);

/* Add one file to the archive.
 *
 * name    the path inside the archive: relative, no "..", no '\\',
 *         at most 65535 bytes
 * data    the file's contents
 * length  bytes of data
 * error   receives the message on failure
 *
 * Returns 0, or -1 on a bad name, a full archive or no memory; the
 * archive is unchanged then. */
int32_t ft_add_to_zip(ft_zip* zip, const char* name, const void* data,
                      size_t length, ft_err* error);

/* Finish the archive and hand it over.
 *
 * archive         receives the archive in a malloc'd buffer; the caller
 *                 frees it. NULL on failure.
 * archive_length  receives its length in bytes
 *
 * zip is freed either way. Returns 0, or -1 when the archive would pass
 * 4 GB or memory runs out. */
int32_t ft_finish_zip(ft_zip* zip, uint8_t** archive, size_t* archive_length,
                      ft_err* error);

/* Drop an unfinished archive. Safe to call on an empty one. */
void ft_free_zip(ft_zip* zip);

#endif

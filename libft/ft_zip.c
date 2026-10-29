/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Store-only zip (PKWARE APPNOTE 4.3: local headers, central directory,
 * end record; version 1.0, method 0). Every number in a zip is little
 * endian. */
#include "ft_zip.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* 1980-01-01 in MS-DOS date format: (year - 1980) << 9 | month << 5 | day */
#define DOS_DATE 0x21
/* the largest number a 32-bit zip field holds */
#define ZIP32_MAX 0xFFFFFFFFu
/* the largest number a 16-bit zip field holds */
#define ZIP16_MAX 0xFFFFu
/* "version needed" and "made by": 1.0, MS-DOS */
#define ZIP_VERSION 10
/* the polynomial of CRC-32, bit-reversed */
#define CRC_POLYNOMIAL 0xEDB88320u

/* the fixed part of each record, without the name */
#define LOCAL_HEADER_BYTES 30
#define DIRECTORY_RECORD_BYTES 46
#define END_RECORD_BYTES 22

/* the signatures that start each record */
#define LOCAL_HEADER_SIGNATURE 0x04034b50u
#define DIRECTORY_RECORD_SIGNATURE 0x02014b50u
#define END_RECORD_SIGNATURE 0x06054b50u

/* buffers start this big and double when full */
#define FIRST_CAPACITY_BYTES 4096

uint32_t ft_compute_crc32(uint32_t crc, const void* bytes, size_t length) {
  const uint8_t* byte = bytes;
  /* the CRC is kept inverted while it runs */
  uint32_t value = crc ^ ZIP32_MAX;
  for(size_t index = 0; index < length; index++) {
    value ^= byte[index];
    /* one step per bit: shift right, and XOR the polynomial in when the
     * bit shifted out was 1 */
    for(uint32_t bit = 0; bit < 8; bit++) {
      if((value & 1) == 1) {
        value = CRC_POLYNOMIAL ^ (value >> 1);
      } else {
        value = value >> 1;
      }
    }
  }
  return value ^ ZIP32_MAX;
}

void ft_init_zip(ft_zip* zip) { memset(zip, 0, sizeof *zip); }

void ft_free_zip(ft_zip* zip) {
  free(zip->entries);
  free(zip->directory);
  ft_init_zip(zip);
}

/* Make room for needed bytes in *buffer, doubling its capacity.
 * Returns false when memory runs out (*buffer is unchanged then). */
static bool grow_buffer(uint8_t** buffer, size_t* capacity, size_t needed) {
  if(needed <= *capacity) return true;
  size_t new_capacity = *capacity;
  if(new_capacity == 0) new_capacity = FIRST_CAPACITY_BYTES;
  while(new_capacity < needed) new_capacity *= 2;
  uint8_t* new_buffer = realloc(*buffer, new_capacity);
  if(new_buffer == NULL) return false;
  *buffer = new_buffer;
  *capacity = new_capacity;
  return true;
}

/* Write a 16-bit little-endian number; returns the byte after it. */
static uint8_t* write_le16(uint8_t* out, uint32_t value) {
  out[0] = (uint8_t)value;
  out[1] = (uint8_t)(value >> 8);
  return out + 2;
}

/* Write a 32-bit little-endian number; returns the byte after it. */
static uint8_t* write_le32(uint8_t* out, uint32_t value) {
  out = write_le16(out, value & 0xFFFF);
  return write_le16(out, value >> 16);
}

/* Write the fields the local header and the directory record share,
 * from "version needed" to "extra field length". */
static uint8_t* write_shared_fields(uint8_t* out, uint32_t crc,
                                    uint32_t data_length,
                                    uint32_t name_length) {
  out = write_le16(out, ZIP_VERSION);
  /* flags */
  out = write_le16(out, 0);
  /* method 0: stored */
  out = write_le16(out, 0);
  /* time 00:00 */
  out = write_le16(out, 0);
  out = write_le16(out, DOS_DATE);
  out = write_le32(out, crc);
  /* compressed size = uncompressed size */
  out = write_le32(out, data_length);
  out = write_le32(out, data_length);
  out = write_le16(out, name_length);
  /* no extra field */
  return write_le16(out, 0);
}

static bool is_safe_name(const char* name) {
  bool is_absolute = name[0] == '/';
  bool has_parent = strstr(name, "..") != NULL;
  bool has_backslash = strchr(name, '\\') != NULL;
  return is_absolute == false && has_parent == false && has_backslash == false;
}

int32_t ft_add_to_zip(ft_zip* zip, const char* name, const void* data,
                      size_t length, ft_err* error) {
  size_t name_length = strlen(name);
  size_t entry_bytes = LOCAL_HEADER_BYTES + name_length + length;
  size_t record_bytes = DIRECTORY_RECORD_BYTES + name_length;

  if(name_length > ZIP16_MAX) return ft_fail(error, "zip: name too long");
  if(name_length == 0) return ft_fail(error, "zip: bad name");
  if(is_safe_name(name) == false) {
    return ft_fail(error, "zip: bad name %.60s", name);
  }
  bool is_full = zip->entry_count == ZIP16_MAX ||
                 length > ZIP32_MAX ||
                 zip->entries_length + entry_bytes > ZIP32_MAX;
  if(is_full) return ft_fail(error, "zip: archive full");
  bool has_room = grow_buffer(&zip->entries, &zip->entries_capacity,
                              zip->entries_length + entry_bytes) &&
                  grow_buffer(&zip->directory, &zip->directory_capacity,
                              zip->directory_length + record_bytes);
  if(has_room == false) return ft_fail(error, "zip: out of memory");

  /* safe: the checks above keep these within 32 and 16 bits */
  uint32_t data_length = (uint32_t)length;
  uint32_t name_length16 = (uint32_t)name_length;
  uint32_t entry_offset = (uint32_t)zip->entries_length;
  uint32_t crc = ft_compute_crc32(0, data, length);

  /* the local entry: header, name, data */
  uint8_t* out = zip->entries + zip->entries_length;
  out = write_le32(out, LOCAL_HEADER_SIGNATURE);
  out = write_shared_fields(out, crc, data_length, name_length16);
  memcpy(out, name, name_length);
  if(length > 0) memcpy(out + name_length, data, length);
  zip->entries_length += entry_bytes;

  /* the directory record: points back at the local entry */
  out = zip->directory + zip->directory_length;
  out = write_le32(out, DIRECTORY_RECORD_SIGNATURE);
  out = write_le16(out, ZIP_VERSION);
  out = write_shared_fields(out, crc, data_length, name_length16);
  /* comment length, disk number, internal and external attributes */
  out = write_le16(out, 0);
  out = write_le16(out, 0);
  out = write_le16(out, 0);
  out = write_le32(out, 0);
  out = write_le32(out, entry_offset);
  memcpy(out, name, name_length);
  zip->directory_length += record_bytes;

  zip->entry_count++;
  return 0;
}

int32_t ft_finish_zip(ft_zip* zip, uint8_t** archive, size_t* archive_length,
                      ft_err* error) {
  size_t total_bytes =
    zip->entries_length + zip->directory_length + END_RECORD_BYTES;
  *archive = NULL;
  *archive_length = 0;
  if(total_bytes > ZIP32_MAX) {
    ft_free_zip(zip);
    return ft_fail(error, "zip: archive full");
  }
  if(grow_buffer(&zip->entries, &zip->entries_capacity, total_bytes) == false) {
    ft_free_zip(zip);
    return ft_fail(error, "zip: out of memory");
  }

  /* the archive is: the entries, then the directory, then the end record */
  if(zip->directory_length > 0) {
    memcpy(zip->entries + zip->entries_length, zip->directory,
           zip->directory_length);
  }
  uint8_t* out = zip->entries + zip->entries_length + zip->directory_length;
  out = write_le32(out, END_RECORD_SIGNATURE);
  /* this disk, and the disk the directory starts on */
  out = write_le16(out, 0);
  out = write_le16(out, 0);
  /* entries on this disk, and in total */
  out = write_le16(out, zip->entry_count);
  out = write_le16(out, zip->entry_count);
  /* safe: total_bytes <= ZIP32_MAX was checked above */
  out = write_le32(out, (uint32_t)zip->directory_length);
  out = write_le32(out, (uint32_t)zip->entries_length);
  /* no comment */
  write_le16(out, 0);

  *archive = zip->entries;
  *archive_length = total_bytes;
  free(zip->directory);
  ft_init_zip(zip);
  return 0;
}

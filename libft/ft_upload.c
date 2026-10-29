/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_upload.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* the packet header at the start of every WRITE BUFFER's data */
#define HEADER_BYTES 10
/* header byte 0: the loader commands */
#define LOADER_RUN 0x00
#define LOADER_WRITE_MEMORY 0x01
/* chunk i goes to segment 0x3i00 */
#define FIRST_SEGMENT_HIGH 0x30

/* WRITE BUFFER (opcode 0x3b), mode 1 (vendor specific), with the data
 * length in bytes 6..8, big endian */
static void build_write_buffer_command(uint8_t command[10], size_t length) {
  memset(command, 0, 10);
  command[0] = 0x3b;
  command[1] = 0x01;
  command[6] = (uint8_t)(length >> 16);
  command[7] = (uint8_t)(length >> 8);
  command[8] = (uint8_t)length;
}

int32_t ft_build_upload_chunk(uint32_t chunk_index,
                              const uint8_t code[FT_UPLOAD_CHUNK_BYTES],
                              uint8_t command[10], uint8_t* data,
                              size_t data_capacity, ft_err* error) {
  if(chunk_index >= FT_UPLOAD_CHUNK_COUNT) {
    return ft_fail(error, "upload: chunk %u (0..15)", (unsigned)chunk_index);
  }
  if(data_capacity < HEADER_BYTES + FT_UPLOAD_CHUNK_BYTES) {
    return ft_fail(error, "upload: buffer too small");
  }
  memset(data, 0, HEADER_BYTES);
  data[0] = LOADER_WRITE_MEMORY;
  /* the segment's high byte: 0x3i00; the offset stays 0 */
  data[1] = (uint8_t)(FIRST_SEGMENT_HIGH + chunk_index);
  memcpy(data + HEADER_BYTES, code, FT_UPLOAD_CHUNK_BYTES);
  build_write_buffer_command(command, HEADER_BYTES + FT_UPLOAD_CHUNK_BYTES);
  return HEADER_BYTES + FT_UPLOAD_CHUNK_BYTES;
}

int32_t ft_build_upload_trailer(uint8_t command[10], uint8_t* data,
                                size_t data_capacity, ft_err* error) {
  /* run from 3001:000E: segment 0x3001 and offset 0x000E, high byte
   * first, as FlexColor sends them */
  static const uint8_t RUN_HEADER[HEADER_BYTES] = {
    LOADER_RUN, 0x30, 0x01, 0x00, 0x0e, 0, 0, 0, 0, 0};
  if(data_capacity < HEADER_BYTES) {
    return ft_fail(error, "upload: buffer too small");
  }
  memcpy(data, RUN_HEADER, HEADER_BYTES);
  build_write_buffer_command(command, HEADER_BYTES);
  return HEADER_BYTES;
}

bool ft_is_loader(const uint8_t* inquiry, size_t inquiry_length) {
  /* the product name starts at byte 16 */
  return inquiry_length >= 16 + 11 &&
         memcmp(inquiry + 16, "SCSI Loader", 11) == 0;
}

/* Join the first prefix_length bytes of prefix and all of name into path.
 * Returns false when the result does not fit FT_UPLOAD_PATH_BYTES. */
static bool join_path(char* path, const char* prefix, size_t prefix_length,
                      const char* name) {
  size_t name_length = strlen(name);
  if(prefix_length + name_length >= FT_UPLOAD_PATH_BYTES) return false;
  memcpy(path, prefix, prefix_length);
  /* + 1 copies the NUL at the end of name too */
  memcpy(path + prefix_length, name, name_length + 1);
  return true;
}

uint32_t ft_find_microcode_paths(const char* given, const char* program_path,
                                 char paths[2][FT_UPLOAD_PATH_BYTES]) {
  if(given != NULL) {
    if(given[0] != '\0' && join_path(paths[0], "", 0, given)) return 1;
    return 0;
  }

  /* the directory part of program_path: up to and with its last
   * separator; 0 when there is none */
  size_t directory_length = 0;
  for(size_t index = 0; program_path != NULL && program_path[index] != '\0';
      index++) {
    if(program_path[index] == '\\' || program_path[index] == '/') {
      directory_length = index + 1;
    }
  }

  uint32_t path_count = 0;
  if(directory_length > 0 && join_path(paths[path_count], program_path,
                                       directory_length, FT_MICROCODE_FILE)) {
    path_count++;
  }
  if(join_path(paths[path_count], "", 0, FT_MICROCODE_FILE)) path_count++;
  return path_count;
}

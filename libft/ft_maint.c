/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_maint.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* header byte 0: the loader command "write" */
#define COMMAND_WRITE 0x01
/* header byte 9: flags */
#define FLAG_IO_PORT 0x01
#define FLAG_KEEP_PORT 0x04
/* the only I/O ports a write may go to */
#define PORT_A 0x342u
#define PORT_B 0x344u
/* a segment holds 64 KiB */
#define SEGMENT_BYTES 0x10000u

/* Fill the 10-byte packet header: command "write", W1 and W2 big
 * endian, flags. */
static void build_header(uint8_t* packet, uint32_t word1, uint32_t word2,
                         uint8_t flags) {
  memset(packet, 0, FT_MAINT_HEADER_BYTES);
  packet[0] = COMMAND_WRITE;
  packet[1] = (uint8_t)(word1 >> 8);
  packet[2] = (uint8_t)word1;
  packet[3] = (uint8_t)(word2 >> 8);
  packet[4] = (uint8_t)word2;
  packet[9] = flags;
}

/* the transfer length in CDB bytes 6..8, big endian */
static void put_length(uint8_t command[10], size_t length) {
  command[6] = (uint8_t)(length >> 16);
  command[7] = (uint8_t)(length >> 8);
  command[8] = (uint8_t)length;
}

/* WRITE BUFFER (opcode 0x3b), mode 1 (vendor specific) */
static void build_write_buffer_command(uint8_t command[10], size_t length) {
  memset(command, 0, 10);
  command[0] = 0x3b;
  command[1] = 0x01;
  put_length(command, length);
}

int32_t ft_build_port_write(uint32_t port, uint8_t value, uint8_t command[10],
                            uint8_t* packet, size_t capacity, ft_err* error) {
  if(port != PORT_A && port != PORT_B) {
    return ft_fail(error,
                   "maint: I/O port 0x%x not allowed (only 0x342, 0x344)",
                   (unsigned)port);
  }
  if(capacity < FT_MAINT_HEADER_BYTES + 1) {
    return ft_fail(error, "maint: buffer too small");
  }
  build_header(packet, port, 0, FLAG_IO_PORT | FLAG_KEEP_PORT);
  packet[FT_MAINT_HEADER_BYTES] = value;
  build_write_buffer_command(command, FT_MAINT_HEADER_BYTES + 1);
  return FT_MAINT_HEADER_BYTES + 1;
}

int32_t ft_build_memory_write(uint32_t segment, uint32_t offset,
                              const uint8_t* data, size_t length,
                              uint8_t command[10], uint8_t* packet,
                              size_t capacity, ft_err* error) {
  if(segment != FT_MAINT_TABLE_SEGMENT) {
    return ft_fail(error, "maint: segment 0x%x not allowed (only 4000)",
                   (unsigned)segment);
  }
  if(length == 0 || length > FT_MAINT_MAX_DATA_BYTES) {
    return ft_fail(error, "maint: %u bytes (1..%u allowed)", (unsigned)length,
                   (unsigned)FT_MAINT_MAX_DATA_BYTES);
  }
  if(offset < FT_MAINT_TABLE_START || offset + length > FT_MAINT_TABLE_END) {
    return ft_fail(error,
                   "maint: 4000:%04x+%u outside the timing table "
                   "4000:7640..76bb",
                   (unsigned)offset, (unsigned)length);
  }
  if(capacity < FT_MAINT_HEADER_BYTES + length) {
    return ft_fail(error, "maint: buffer too small");
  }
  build_header(packet, segment, offset, 0);
  memcpy(packet + FT_MAINT_HEADER_BYTES, data, length);
  build_write_buffer_command(command, FT_MAINT_HEADER_BYTES + length);
  /* safe: length is at most FT_MAINT_MAX_DATA_BYTES here */
  return (int32_t)(FT_MAINT_HEADER_BYTES + length);
}

/* One allowed live-mode write: an address and the lengths that may start
 * there. */
typedef struct live_target {
  /* offset inside segment 4000 */
  uint16_t offset;
  /* shortest and longest allowed write, in bytes */
  uint8_t shortest_length;
  uint8_t longest_length;
} live_target;

/* The live-mode whitelist (see ft_maint.h for what each address is). */
static const live_target LIVE_TARGETS[] = {
  {.offset = 0x7557, .shortest_length = 1, .longest_length = 2},
  {.offset = 0x8ed4, .shortest_length = 4, .longest_length = 4},
  {.offset = 0x8edc, .shortest_length = 4, .longest_length = 4},
  {.offset = 0x8ee4, .shortest_length = 4, .longest_length = 4},
  {.offset = 0x8eee, .shortest_length = 4, .longest_length = 4},
};

/* true when offset/length is exactly one of LIVE_TARGETS. */
static bool is_live_target(uint32_t offset, size_t length) {
  /* sizeof the whole array / sizeof one entry = number of entries */
  size_t target_count = sizeof LIVE_TARGETS / sizeof LIVE_TARGETS[0];
  for(size_t index = 0; index < target_count; index++) {
    const live_target* target = &LIVE_TARGETS[index];
    if(offset == target->offset &&
       length >= target->shortest_length &&
       length <= target->longest_length) {
      return true;
    }
  }
  return false;
}

int32_t ft_build_live_write(uint32_t offset, const uint8_t* data, size_t length,
                            uint8_t command[10], uint8_t* packet,
                            size_t capacity, ft_err* error) {
  if(is_live_target(offset, length) == false) {
    return ft_fail(error,
                   "live: 4000:%04x+%u not allowed (only 7557+1/2, "
                   "8ed4+4, 8edc+4, 8ee4+4, 8eee+4)",
                   (unsigned)offset, (unsigned)length);
  }
  if(capacity < FT_MAINT_HEADER_BYTES + length) {
    return ft_fail(error, "live: buffer too small");
  }
  /* Same header as ft_build_memory_write: command 01, W1 = segment 4000,
   * W2 = offset, flags 0 (memory, not an I/O port). */
  build_header(packet, FT_MAINT_TABLE_SEGMENT, offset, 0);
  memcpy(packet + FT_MAINT_HEADER_BYTES, data, length);
  build_write_buffer_command(command, FT_MAINT_HEADER_BYTES + length);
  /* safe: length is at most 4 here, so the sum fits easily */
  return (int32_t)(FT_MAINT_HEADER_BYTES + length);
}

/* The segments a memory read may look at. */
static bool is_readable_segment(uint32_t segment) {
  /* 3000 microcode, 4000 data, C000 loader ROM, E000 and F000 upper
   * chip select (parameter flash, reset stub) */
  return segment == 0x3000 ||
         segment == 0x4000 ||
         segment == 0xc000 ||
         segment == 0xe000 ||
         segment == 0xf000;
}

int32_t ft_build_memory_read(uint32_t segment, uint32_t offset, size_t length,
                             uint8_t command[10], ft_err* error) {
  if(is_readable_segment(segment) == false) {
    return ft_fail(error,
                   "maint: peek segment 0x%x not allowed (3000, 4000, "
                   "c000, e000, f000)",
                   (unsigned)segment);
  }
  if(length == 0 || length > FT_MAINT_MAX_PEEK_BYTES) {
    return ft_fail(error, "maint: peek %u bytes (1..%u allowed)",
                   (unsigned)length, (unsigned)FT_MAINT_MAX_PEEK_BYTES);
  }
  if(offset > 0xffffu || offset + length > SEGMENT_BYTES) {
    return ft_fail(error, "maint: peek %04x:%04x+%u wraps the segment",
                   (unsigned)segment, (unsigned)offset, (unsigned)length);
  }
  if(segment == 0xc000 && offset + length > FT_MAINT_LOADER_BYTES) {
    return ft_fail(error,
                   "maint: peek %04x:%04x+%u beyond the loader ROM "
                   "C000:0000..7FFF",
                   (unsigned)segment, (unsigned)offset, (unsigned)length);
  }
  /* READ BUFFER (opcode 0x3c): segment in bytes 2..3, offset in 4..5,
   * both big endian */
  memset(command, 0, 10);
  command[0] = 0x3c;
  command[2] = (uint8_t)(segment >> 8);
  command[3] = (uint8_t)segment;
  command[4] = (uint8_t)(offset >> 8);
  command[5] = (uint8_t)offset;
  put_length(command, length);
  return 0;
}

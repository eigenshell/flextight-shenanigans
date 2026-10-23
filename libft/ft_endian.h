// SPDX-License-Identifier: GPL-2.0-or-later

// endian helper functions to read and write 32 bit numbers
// from and to a buffer. No bounds checking, caller must
// ensure data fits in buffer.

#ifndef FT_ENDIAN_H
#define FT_ENDIAN_H

#include <stdint.h>

uint32_t read_big_endian32(
    const uint8_t *buffer,
    int offset
); // read a 4 byte big endian int from buffer, starting at offset

void write_big_endian32(
    uint8_t *buffer, int offset,
    uint32_t value
); // write a 4 byte big endian int
   // tobuffer, starting at offset

uint32_t read_little_endian32(
    const uint8_t *buffer
); // read a 4 byte little endian int from buffer

#endif

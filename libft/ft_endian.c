/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_endian.h"

#include <stdint.h>

uint32_t ft_read_big_endian32(const uint8_t* buffer, uint32_t offset) {
  uint32_t value = 0;
  for(uint32_t index = 0; index < 4; index++) {
    /* Shift the value left by 8 bits to make room for the new byte,
     * then OR the new byte into the bottom 8 bits. */
    value = (value << 8) | buffer[offset + index];
  }
  return value;
}

void ft_write_big_endian16(uint8_t* buffer, uint32_t offset, uint32_t value) {
  buffer[offset] = (uint8_t)(value >> 8);
  buffer[offset + 1] = (uint8_t)value;
}

void ft_write_big_endian32(uint8_t* buffer, uint32_t offset, uint32_t value) {
  /* Shift each byte down to the bottom 8 bits; the cast keeps only those
   * 8 bits. The most significant byte goes first. */
  buffer[offset] = (uint8_t)(value >> 24);
  buffer[offset + 1] = (uint8_t)(value >> 16);
  buffer[offset + 2] = (uint8_t)(value >> 8);
  buffer[offset + 3] = (uint8_t)value;
}

void ft_write_little_endian16(uint8_t* buffer, uint32_t value) {
  /* the least significant byte goes first */
  buffer[0] = (uint8_t)value;
  buffer[1] = (uint8_t)(value >> 8);
}

void ft_write_little_endian32(uint8_t* buffer, uint32_t value) {
  buffer[0] = (uint8_t)value;
  buffer[1] = (uint8_t)(value >> 8);
  buffer[2] = (uint8_t)(value >> 16);
  buffer[3] = (uint8_t)(value >> 24);
}

uint32_t ft_read_little_endian16(const uint8_t* buffer) {
  /* the second byte is the high one: shift it up by 8 bits */
  return (uint32_t)buffer[0] | (uint32_t)buffer[1] << 8;
}

uint32_t ft_read_little_endian32(const uint8_t* buffer) {
  uint32_t value = 0;
  /* Start at the last byte, the most significant one: after four shifts
   * of 8 bits it has moved to the top, and buffer[0] ends at the bottom. */
  for(uint32_t count = 0; count < 4; count++) {
    value = (value << 8) | buffer[3 - count];
  }
  return value;
}

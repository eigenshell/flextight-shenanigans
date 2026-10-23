#include "ft_endian.h"
#include <stdint.h>

uint32_t read_big_endian32(const uint8_t *buffer, int offset) {
  uint32_t value = 0;

  for(int i = 0; i < 4; i++) {
    // Shift the current value left by 8 bits to make room for the new byte
    // then 'OR' the new byte into the bottom 8 bits.
    value = (value << 8) | buffer[offset + i];
  }

  return value;
}

void write_big_endian32(uint8_t *buffer, int offset, uint32_t value) {
  buffer[offset] = (uint8_t)(value >> 24);
  buffer[offset + 1] = (uint8_t)(value >> 16);
  buffer[offset + 2] = (uint8_t)(value >> 8);
  buffer[offset + 3] = (uint8_t)value;
}

uint32_t read_little_endian32(const uint8_t *buffer) {
  uint32_t value = 0;
  for(int i = 3; i >= 0; i--) {
    // starting from the least significant byte, shift value left by 8 bits,
    // then OR with the byte at index i.
    // this will move each byte 8 bits to the left, and OR it with the byte at
    // that index.
    value = (value << 8) | buffer[i];
  }
  return value;
}

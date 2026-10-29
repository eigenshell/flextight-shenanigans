/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Read and write 32-bit numbers in a byte buffer, in a fixed byte order.
 *
 * Big endian puts the most significant byte first (the SCSI and PNG
 * order); little endian puts it last (the x86 and TIFF "II" order).
 * There is no bounds checking: the caller makes sure the four bytes fit
 * in the buffer. */
#ifndef FT_ENDIAN_H
#define FT_ENDIAN_H

#include <stdint.h>

/* Read 4 bytes at buffer + offset as a big-endian number. */
uint32_t ft_read_big_endian32(const uint8_t* buffer, uint32_t offset);

/* Write value as 2 big-endian bytes at buffer + offset. */
void ft_write_big_endian16(uint8_t* buffer, uint32_t offset, uint32_t value);

/* Write value as 4 big-endian bytes at buffer + offset. */
void ft_write_big_endian32(uint8_t* buffer, uint32_t offset, uint32_t value);

/* Write value as 2 little-endian bytes at the start of buffer. */
void ft_write_little_endian16(uint8_t* buffer, uint32_t value);

/* Write value as 4 little-endian bytes at the start of buffer. */
void ft_write_little_endian32(uint8_t* buffer, uint32_t value);

/* Read the first 2 bytes of buffer as a little-endian number. */
uint32_t ft_read_little_endian16(const uint8_t* buffer);

/* Read the first 4 bytes of buffer as a little-endian number. */
uint32_t ft_read_little_endian32(const uint8_t* buffer);

#endif

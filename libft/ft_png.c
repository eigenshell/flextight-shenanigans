/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_png.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_endian.h"
#include "ft_zip.h"

/* a stored deflate block holds at most 65535 bytes */
#define STORED_BLOCK_MAX_BYTES 65535
/* the modulus of zlib's Adler-32 checksum: the largest prime below 2^16 */
#define ADLER_MODULUS 65521
/* PNG colour type 2: RGB, no palette, no alpha */
#define COLOUR_TYPE_RGB 2

/* Write one PNG chunk: length, type, data, CRC of type and data.
 * Returns true when every byte was written. */
static bool write_chunk(FILE* file, const char* type, const uint8_t* data,
                        size_t length) {
  uint8_t header[8];
  uint8_t crc_bytes[4];
  ft_write_big_endian32(header, 0, (uint32_t)length);
  memcpy(header + 4, type, 4);
  /* the CRC covers the type and the data, not the length */
  uint32_t crc = ft_compute_crc32(0, header + 4, 4);
  if(length > 0) crc = ft_compute_crc32(crc, data, length);
  ft_write_big_endian32(crc_bytes, 0, crc);

  bool is_header_written = fwrite(header, 1, 8, file) == 8;
  bool is_data_written = length == 0 || fwrite(data, 1, length, file) == length;
  bool is_crc_written = fwrite(crc_bytes, 1, 4, file) == 4;
  return is_header_written && is_data_written && is_crc_written;
}

/* Wrap the image rows in a zlib stream of stored (uncompressed) blocks.
 * Every row gets a leading filter byte 0 (no filter).
 *
 * samples          height rows of row_bytes - 1 bytes
 * row_bytes        bytes per row in the stream, filter byte included
 * stream_length    receives the length of the stream
 *
 * Returns the stream (free it), or NULL when memory runs out. */
static uint8_t* make_zlib_stream(const uint8_t* samples, size_t row_bytes,
                                 uint32_t height, size_t* stream_length) {
  size_t raw_length = row_bytes * height;
  size_t block_count =
    (raw_length + STORED_BLOCK_MAX_BYTES - 1) / STORED_BLOCK_MAX_BYTES;
  /* 2 bytes zlib header, 5 bytes per block header, 4 bytes checksum */
  uint8_t* stream = malloc(2 + raw_length + 5 * block_count + 4);
  if(stream == NULL) return NULL;

  uint8_t* out = stream;
  /* zlib header: deflate with a 32K window, no preset dictionary */
  *out++ = 0x78;
  *out++ = 0x01;

  /* the two halves of the Adler-32 checksum */
  uint32_t adler_sum = 1;
  uint32_t adler_sum_of_sums = 0;
  size_t position = 0;
  while(position < raw_length) {
    size_t block_length = raw_length - position;
    if(block_length > STORED_BLOCK_MAX_BYTES) {
      block_length = STORED_BLOCK_MAX_BYTES;
    }
    bool is_last_block = position + block_length == raw_length;
    /* the block's length and its one's complement, both little endian */
    uint16_t length_complement = (uint16_t)(0xffff ^ block_length);
    /* 1 = BFINAL, the last block; BTYPE 00 = stored */
    *out++ = is_last_block ? 1 : 0;
    *out++ = (uint8_t)(block_length & 0xff);
    *out++ = (uint8_t)(block_length >> 8);
    *out++ = (uint8_t)(length_complement & 0xff);
    *out++ = (uint8_t)(length_complement >> 8);

    for(size_t index = 0; index < block_length; index++, position++) {
      size_t row = position / row_bytes;
      size_t column = position % row_bytes;
      /* column 0 is the filter byte; the samples have no filter bytes,
       * so each row in samples is one byte shorter */
      uint8_t value = 0;
      if(column > 0) value = samples[row * (row_bytes - 1) + column - 1];
      *out++ = value;
      adler_sum = (adler_sum + value) % ADLER_MODULUS;
      adler_sum_of_sums = (adler_sum_of_sums + adler_sum) % ADLER_MODULUS;
    }
  }
  ft_write_big_endian32(out, 0, (adler_sum_of_sums << 16) | adler_sum);
  out += 4;

  /* safe: out never moves before stream */
  *stream_length = (size_t)(out - stream);
  return stream;
}

/* samples: height rows of 3 * width values, bit_depth 8 (one byte each)
 * or 16 (two bytes each, big endian) */
static int32_t write_png(const char* path, const uint8_t* samples,
                         uint32_t width, uint32_t height, uint8_t bit_depth,
                         ft_err* error) {
  static const uint8_t PNG_SIGNATURE[8] = {137, 80, 78, 71, 13, 10, 26, 10};

  if(width == 0 || height == 0) return ft_fail(error, "png: empty image");
  /* the filter byte, then the pixels */
  size_t row_bytes = 3 * (size_t)width * (bit_depth / 8) + 1;
  size_t stream_length;
  uint8_t* stream =
    make_zlib_stream(samples, row_bytes, height, &stream_length);
  if(stream == NULL) return ft_fail(error, "png: out of memory");

  /* the IHDR chunk: the image's size and format */
  uint8_t image_header[13];
  ft_write_big_endian32(image_header, 0, width);
  ft_write_big_endian32(image_header, 4, height);
  image_header[8] = bit_depth;
  image_header[9] = COLOUR_TYPE_RGB;
  /* compression, filter method and interlace: 0 = the standard ones */
  image_header[10] = 0;
  image_header[11] = 0;
  image_header[12] = 0;

  FILE* file = fopen(path, "wb");
  if(file == NULL) {
    free(stream);
    return ft_fail(error, "png: cannot write %s", path);
  }
  bool is_written =
    fwrite(PNG_SIGNATURE, 1, sizeof PNG_SIGNATURE, file) ==
      sizeof PNG_SIGNATURE &&
    write_chunk(file, "IHDR", image_header, sizeof image_header) &&
    write_chunk(file, "IDAT", stream, stream_length) &&
    write_chunk(file, "IEND", NULL, 0);
  /* close first, so a failing close is noticed too */
  bool is_closed = fclose(file) == 0;
  free(stream);
  if(is_written == false || is_closed == false) {
    return ft_fail(error, "png: write error on %s", path);
  }
  return 0;
}

int32_t ft_write_png_rgb8(const char* path, const uint8_t* pixels,
                          uint32_t width, uint32_t height, ft_err* error) {
  return write_png(path, pixels, width, height, 8, error);
}

int32_t ft_write_png_rgb16(const char* path, const uint16_t* pixels,
                           uint32_t width, uint32_t height, ft_err* error) {
  if(width == 0 || height == 0) return ft_fail(error, "png: empty image");
  size_t sample_count = 3 * (size_t)width * height;
  uint8_t* big_endian = malloc(2 * sample_count);
  if(big_endian == NULL) return ft_fail(error, "png: out of memory");
  /* PNG stores 16-bit samples big endian: the high byte first */
  for(size_t index = 0; index < sample_count; index++) {
    big_endian[2 * index] = (uint8_t)(pixels[index] >> 8);
    big_endian[2 * index + 1] = (uint8_t)(pixels[index] & 0xff);
  }
  int32_t result = write_png(path, big_endian, width, height, 16, error);
  free(big_endian);
  return result;
}

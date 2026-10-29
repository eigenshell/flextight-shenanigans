/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The DNG's file pair (Sancho's side: stdio); the layout is ft_dng.c's. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_dng.h"
#include "ft_endian.h"
#include "ft_tiff_ifd.h"

/* aim for strips of about this many bytes */
#define STRIP_BYTES 262144
/* a classic TIFF's offsets are 32 bits: stay well under 4 GB */
#define MAX_FILE_BYTES 4.0e9
/* bytes per pixel: 3 samples of 16 bits */
#define PIXEL_BYTES 6
/* the strip tags, and NoiseProfile */
#define TAG_STRIP_OFFSETS 273
#define TAG_ROWS_PER_STRIP 278
#define TAG_STRIP_BYTE_COUNTS 279
#define TAG_NOISE_PROFILE 51041

/* the rows of strip number strip: rows_per_strip, less for the last */
static uint32_t get_strip_rows(uint32_t strip, uint32_t strip_count,
                               uint32_t rows_per_strip, uint32_t height) {
  if(strip + 1 < strip_count) return rows_per_strip;
  return height - strip * rows_per_strip;
}

int32_t ft_write_dng(const char* path, const ft_image* image, ft_err* error) {
  if(image->pixels == NULL || image->width < 1 || image->height < 1) {
    return ft_fail(error, "dng: empty image");
  }
  uint32_t row_bytes = image->width * PIXEL_BYTES;
  if((double)row_bytes * image->height > MAX_FILE_BYTES) {
    return ft_fail(error, "dng: %ux%u is over 4 GB", (unsigned)image->width,
                   (unsigned)image->height);
  }
  uint32_t rows_per_strip = STRIP_BYTES / row_bytes;
  if(rows_per_strip < 1) rows_per_strip = 1;
  if(rows_per_strip > image->height) rows_per_strip = image->height;
  uint32_t strip_count = (image->height + rows_per_strip - 1) / rows_per_strip;

  ft_ifd ifd;
  memset(&ifd, 0, sizeof ifd);
  FILE* file = NULL;
  uint8_t* row = NULL;
  int32_t result = -1;

  ft_add_ifd_common_tags(&ifd, image, image->width, image->height, 3, true);
  /* StripOffsets: filled in after the layout below */
  ft_add_ifd_entry(&ifd, TAG_STRIP_OFFSETS, FT_TIFF_LONG, strip_count);
  ft_add_ifd_long(&ifd, TAG_ROWS_PER_STRIP, rows_per_strip);
  uint8_t* byte_counts =
    ft_add_ifd_entry(&ifd, TAG_STRIP_BYTE_COUNTS, FT_TIFF_LONG, strip_count);
  for(uint32_t strip = 0; byte_counts != NULL && strip < strip_count; strip++) {
    uint32_t rows =
      get_strip_rows(strip, strip_count, rows_per_strip, image->height);
    ft_write_little_endian32(byte_counts + 4 * strip, rows * row_bytes);
  }
  if(image->has_noise_profile &&
     ft_is_noise_profile_usable(image->noise_profile)) {
    ft_add_ifd_doubles(&ifd, TAG_NOISE_PROFILE, image->noise_profile, 6);
  }
  if(ifd.is_out_of_memory) {
    ft_fail(error, "dng: out of memory");
    goto cleanup;
  }
  /* the pixel data starts right after the IFD and its values */
  uint32_t data_start = ft_lay_out_ifd(&ifd);
  ft_ifd_entry* strip_offsets = ft_find_ifd_entry(&ifd, TAG_STRIP_OFFSETS);
  for(uint32_t strip = 0; strip < strip_count; strip++) {
    ft_write_little_endian32(strip_offsets->data + 4 * strip,
                             data_start + strip * rows_per_strip * row_bytes);
  }

  file = fopen(path, "wb");
  if(file == NULL) {
    ft_fail(error, "dng: cannot write %s", path);
    goto cleanup;
  }
  row = malloc(row_bytes);
  if(row == NULL) {
    ft_fail(error, "dng: out of memory");
    goto cleanup;
  }
  if(ft_write_ifd(&ifd, file) != 0) {
    ft_fail(error, "dng: write error on %s", path);
    goto cleanup;
  }
  bool is_written = true;
  for(uint32_t y = 0; y < image->height && is_written; y++) {
    const float* source = image->pixels + (size_t)y * image->width * 3;
    for(size_t sample = 0; sample < (size_t)image->width * 3; sample++) {
      ft_write_little_endian16(row + 2 * sample,
                               ft_quantise_dng_sample(source[sample]));
    }
    is_written = fwrite(row, 1, row_bytes, file) == row_bytes;
  }
  if(is_written) {
    result = 0;
  } else {
    result = ft_fail(error, "dng: write error on %s", path);
  }

cleanup:
  /* close here too, so a failing close is noticed */
  if(file != NULL && fclose(file) != 0 && result == 0) {
    result = ft_fail(error, "dng: write error on %s", path);
  }
  free(row);
  ft_free_ifd(&ifd);
  return result;
}

int32_t ft_read_dng(const char* path, ft_image* image, ft_err* error) {
  FILE* file = fopen(path, "rb");
  if(file == NULL) return ft_fail(error, "dng: cannot read %s", path);
  uint8_t* bytes = NULL;
  int32_t result = -1;
  long length = -1;
  /* the file's length: seek to the end and ask where we are */
  bool has_length = fseek(file, 0, SEEK_END) == 0 &&
                    (length = ftell(file)) >= 0 &&
                    fseek(file, 0, SEEK_SET) == 0;
  if(has_length) {
    /* malloc(0) may return NULL, so ask for at least 1 byte */
    bytes = malloc(length > 0 ? (size_t)length : 1);
  }
  if(bytes != NULL && fread(bytes, 1, (size_t)length, file) == (size_t)length) {
    result = ft_parse_dng(bytes, (size_t)length, image, error);
    /* put the path into the parser's message: "dng: <path>: <reason>" */
    if(result != 0 &&
       error != NULL &&
       strncmp(error->message, "dng: ", 5) == 0) {
      char reason[sizeof error->message];
      snprintf(reason, sizeof reason, "%s", error->message + 5);
      ft_fail(error, "dng: %s: %s", path, reason);
    }
  } else {
    ft_fail(error, "dng: cannot read %s", path);
  }
  free(bytes);
  fclose(file);
  return result;
}

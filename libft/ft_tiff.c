/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_tiff.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_endian.h"
#include "ft_tiff_ifd.h"

/* the tags this writer adds to the common ones */
enum {
  TAG_PHOTOMETRIC = 262,
  TAG_TILE_WIDTH = 322,
  TAG_TILE_LENGTH = 323,
  TAG_TILE_OFFSETS = 324,
  TAG_TILE_BYTE_COUNTS = 325,
  TAG_ICC_PROFILE = 34675,
  TAG_DEFAULT_CROP_ORIGIN = 50719,
  TAG_DEFAULT_CROP_SIZE = 50720
};
/* PhotometricInterpretation: BlackIsZero grey, or RGB */
#define PHOTOMETRIC_BLACK_IS_ZERO 1
#define PHOTOMETRIC_RGB 2
/* room kept for the header when checking the file size */
#define HEADER_ALLOWANCE_BYTES 65536.0
/* a classic TIFF's limit; 2 GB where fseek takes a 32-bit long */
#define MAX_FILE_BYTES 4.0e9
#define MAX_FILE_BYTES_32BIT_LONG 2.0e9
/* the first tile starts on a 16-byte boundary */
#define TILE_ALIGNMENT 16

struct ft_tiff_writer {
  /* the open file; NULL after close */
  FILE* file;
  ft_tiff_kind kind;
  /* the image's columns and rows */
  uint32_t width;
  uint32_t height;
  /* samples per pixel: 1 or 3 */
  uint32_t samples_per_pixel;
  /* tiles across and down */
  uint32_t tiles_across;
  uint32_t tiles_down;
  /* columns received so far */
  uint32_t columns_received;
  /* columns in the band now */
  uint32_t band_columns;
  /* up to FT_TILE_PIXELS columns of height x samples_per_pixel */
  uint16_t* band;
  /* one tile, little endian, ready to write */
  uint8_t* tile;
  /* bytes per tile */
  uint32_t tile_bytes;
  /* DNG: file offsets of DefaultCropOrigin and DefaultCropSize */
  uint32_t crop_offsets[2];
  /* DNG: the crop's x, y, width and height */
  int32_t crop[4];
};

static uint32_t get_samples_per_pixel(ft_tiff_kind kind) {
  return kind == FT_TIFF_GRAY ? 1 : 3;
}

/* tiles needed to cover pixels, rounding up */
static uint32_t count_tiles(uint32_t pixels) {
  return (pixels + FT_TILE_PIXELS - 1) / FT_TILE_PIXELS;
}

double ft_estimate_tiff_bytes(ft_tiff_kind kind, uint32_t width,
                              uint32_t height) {
  double tiles_across = count_tiles(width);
  double tiles_down = count_tiles(height);
  return tiles_across *
         tiles_down *
         FT_TILE_PIXELS *
         FT_TILE_PIXELS *
         2.0 *
         get_samples_per_pixel(kind);
}

void ft_abandon_tiff_writer(ft_tiff_writer* writer) {
  if(writer == NULL) return;
  if(writer->file != NULL) fclose(writer->file);
  free(writer->band);
  free(writer->tile);
  free(writer);
}

/* the options a writer can work with */
static bool are_options_valid(const ft_tiff_options* options) {
  bool is_known_kind = options->kind == FT_TIFF_DNG ||
                       options->kind == FT_TIFF_RGB ||
                       options->kind == FT_TIFF_GRAY;
  return options->width >= 1 &&
         options->height >= 1 &&
         options->metadata != NULL &&
         is_known_kind;
}

static bool does_fit_one_file(const ft_tiff_options* options) {
  double bytes =
    ft_estimate_tiff_bytes(options->kind, options->width, options->height) +
    HEADER_ALLOWANCE_BYTES;
  if(bytes > MAX_FILE_BYTES) return false;
  /* fseek takes a long: where that is 32 bits, 2 GB is the end */
  if(sizeof(long) < 8 && bytes > MAX_FILE_BYTES_32BIT_LONG) return false;
  return true;
}

/* Build the IFD for the writer. Returns the first tile's file offset,
 * or 0 when memory ran out. */
static uint32_t build_ifd(ft_ifd* ifd, ft_tiff_writer* writer,
                          const ft_tiff_options* options) {
  bool is_dng = options->kind == FT_TIFF_DNG;
  ft_add_ifd_common_tags(ifd, options->metadata, options->width,
                         options->height, writer->samples_per_pixel, is_dng);
  if(is_dng) {
    /* filled in at close */
    ft_add_ifd_entry(ifd, TAG_DEFAULT_CROP_ORIGIN, FT_TIFF_LONG, 2);
    ft_add_ifd_entry(ifd, TAG_DEFAULT_CROP_SIZE, FT_TIFF_LONG, 2);
  } else {
    uint32_t photometric = options->kind == FT_TIFF_GRAY
                             ? PHOTOMETRIC_BLACK_IS_ZERO
                             : PHOTOMETRIC_RGB;
    ft_add_ifd_short(ifd, TAG_PHOTOMETRIC, photometric);
    if(options->icc_profile != NULL && options->icc_profile_length > 0) {
      ft_add_ifd_bytes(ifd, TAG_ICC_PROFILE, FT_TIFF_UNDEFINED,
                       options->icc_profile, options->icc_profile_length);
    }
  }
  uint32_t tile_count = writer->tiles_across * writer->tiles_down;
  ft_add_ifd_long(ifd, TAG_TILE_WIDTH, FT_TILE_PIXELS);
  ft_add_ifd_long(ifd, TAG_TILE_LENGTH, FT_TILE_PIXELS);
  /* TileOffsets: filled in after the layout below */
  ft_add_ifd_entry(ifd, TAG_TILE_OFFSETS, FT_TIFF_LONG, tile_count);
  uint8_t* byte_counts =
    ft_add_ifd_entry(ifd, TAG_TILE_BYTE_COUNTS, FT_TIFF_LONG, tile_count);
  for(uint32_t tile = 0; byte_counts != NULL && tile < tile_count; tile++) {
    ft_write_little_endian32(byte_counts + 4 * tile, writer->tile_bytes);
  }
  if(ifd->is_out_of_memory) return 0;

  uint32_t data_start = ft_lay_out_ifd(ifd);
  /* round up to the alignment: clear the low bits after adding 15 */
  data_start =
    (data_start + TILE_ALIGNMENT - 1) & ~(uint32_t)(TILE_ALIGNMENT - 1);
  /* Tiles go into the file tile column by tile column, top to bottom;
   * TileOffsets is indexed row by row (down * across + across). */
  ft_ifd_entry* offsets = ft_find_ifd_entry(ifd, TAG_TILE_OFFSETS);
  for(uint32_t across = 0; across < writer->tiles_across; across++) {
    for(uint32_t down = 0; down < writer->tiles_down; down++) {
      uint32_t index = down * writer->tiles_across + across;
      uint32_t file_order = across * writer->tiles_down + down;
      ft_write_little_endian32(offsets->data + 4 * index,
                               data_start + file_order * writer->tile_bytes);
    }
  }
  if(is_dng) {
    writer->crop_offsets[0] =
      ft_find_ifd_entry(ifd, TAG_DEFAULT_CROP_ORIGIN)->offset;
    writer->crop_offsets[1] =
      ft_find_ifd_entry(ifd, TAG_DEFAULT_CROP_SIZE)->offset;
  }
  return data_start;
}

ft_tiff_writer* ft_open_tiff_writer(const char* path,
                                    const ft_tiff_options* options,
                                    ft_err* error) {
  if(are_options_valid(options) == false) {
    ft_fail(error, "tiff: bad image (%ux%u, kind %d)", (unsigned)options->width,
            (unsigned)options->height, (int)options->kind);
    return NULL;
  }
  if(does_fit_one_file(options) == false) {
    ft_fail(error, "tiff: %ux%u is too large for one file",
            (unsigned)options->width, (unsigned)options->height);
    return NULL;
  }
  ft_tiff_writer* writer = calloc(1, sizeof *writer);
  if(writer == NULL) {
    ft_fail(error, "tiff: out of memory");
    return NULL;
  }
  writer->kind = options->kind;
  writer->width = options->width;
  writer->height = options->height;
  writer->samples_per_pixel = get_samples_per_pixel(options->kind);
  writer->tiles_across = count_tiles(options->width);
  writer->tiles_down = count_tiles(options->height);
  writer->tile_bytes =
    FT_TILE_PIXELS * FT_TILE_PIXELS * 2 * writer->samples_per_pixel;
  writer->crop[2] = (int32_t)options->width;
  writer->crop[3] = (int32_t)options->height;

  ft_ifd ifd;
  memset(&ifd, 0, sizeof ifd);
  uint32_t data_start = build_ifd(&ifd, writer, options);
  if(data_start == 0) {
    ft_fail(error, "tiff: out of memory");
    goto fail;
  }
  writer->file = fopen(path, "wb");
  if(writer->file == NULL) {
    ft_fail(error, "tiff: cannot write %s", path);
    goto fail;
  }
  if(ft_write_ifd(&ifd, writer->file) != 0) {
    ft_fail(error, "tiff: write error on %s", path);
    goto fail;
  }
  /* zeros up to the first tile: the file is only ever appended to */
  for(long position = ftell(writer->file); position < (long)data_start;
      position++) {
    if(fputc(0, writer->file) == EOF) {
      ft_fail(error, "tiff: write error on %s", path);
      goto fail;
    }
  }
  ft_free_ifd(&ifd);
  if(options->kind == FT_TIFF_DNG) {
    ft_set_tiff_crop(writer, 0, 0, (int32_t)options->width,
                     (int32_t)options->height);
  }
  return writer;

fail:
  ft_free_ifd(&ifd);
  ft_abandon_tiff_writer(writer);
  return NULL;
}

/* Write the band's tile column (band_columns columns, zeros past them
 * and past the image's last row). Returns false on a write error. */
static bool write_band(ft_tiff_writer* writer) {
  for(uint32_t down = 0; down < writer->tiles_down; down++) {
    uint8_t* out = writer->tile;
    for(uint32_t tile_row = 0; tile_row < FT_TILE_PIXELS; tile_row++) {
      uint32_t row = down * FT_TILE_PIXELS + tile_row;
      for(uint32_t column = 0; column < FT_TILE_PIXELS; column++) {
        for(uint32_t sample = 0; sample < writer->samples_per_pixel; sample++) {
          uint32_t value = 0;
          if(row < writer->height && column < writer->band_columns) {
            size_t pixel = (size_t)column * writer->height + row;
            value = writer->band[pixel * writer->samples_per_pixel + sample];
          }
          ft_write_little_endian16(out, value);
          out += 2;
        }
      }
    }
    if(fwrite(writer->tile, 1, writer->tile_bytes, writer->file) !=
       writer->tile_bytes) {
      return false;
    }
  }
  writer->band_columns = 0;
  return true;
}

/* the band and tile buffers exist only while columns arrive */
static void free_buffers(ft_tiff_writer* writer) {
  free(writer->band);
  free(writer->tile);
  writer->band = NULL;
  writer->tile = NULL;
}

int32_t ft_write_tiff_column(ft_tiff_writer* writer, const uint16_t* column,
                             ft_err* error) {
  if(writer->columns_received >= writer->width) {
    return ft_fail(error, "tiff: more than %u columns",
                   (unsigned)writer->width);
  }
  size_t column_values = (size_t)writer->height * writer->samples_per_pixel;
  if(writer->band == NULL) {
    writer->band =
      malloc(FT_TILE_PIXELS * column_values * sizeof *writer->band);
    writer->tile = malloc(writer->tile_bytes);
    if(writer->band == NULL || writer->tile == NULL) {
      free_buffers(writer);
      return ft_fail(error, "tiff: out of memory for %u rows",
                     (unsigned)writer->height);
    }
  }
  memcpy(writer->band + writer->band_columns * column_values, column,
         column_values * sizeof *column);
  writer->band_columns++;
  writer->columns_received++;
  bool is_last_column = writer->columns_received == writer->width;
  if(writer->band_columns == FT_TILE_PIXELS || is_last_column) {
    if(write_band(writer) == false) return ft_fail(error, "tiff: write error");
  }
  if(is_last_column) free_buffers(writer);
  return 0;
}

void ft_set_tiff_crop(ft_tiff_writer* writer, int32_t x, int32_t y,
                      int32_t crop_width, int32_t crop_height) {
  int32_t width = (int32_t)writer->width;
  int32_t height = (int32_t)writer->height;
  /* a negative origin shrinks the crop by as much */
  if(x < 0) {
    crop_width += x;
    x = 0;
  }
  if(y < 0) {
    crop_height += y;
    y = 0;
  }
  if(x > width - 1) x = width - 1;
  if(y > height - 1) y = height - 1;
  if(crop_width > width - x) crop_width = width - x;
  if(crop_height > height - y) crop_height = height - y;
  if(crop_width < 1) crop_width = 1;
  if(crop_height < 1) crop_height = 1;
  writer->crop[0] = x;
  writer->crop[1] = y;
  writer->crop[2] = crop_width;
  writer->crop[3] = crop_height;
}

int32_t ft_close_tiff_writer(ft_tiff_writer* writer, ft_err* error) {
  int32_t result = 0;
  if(writer->columns_received != writer->width) {
    result =
      ft_fail(error, "tiff: %u of %u columns arrived",
              (unsigned)writer->columns_received, (unsigned)writer->width);
  }
  if(result == 0 && writer->kind == FT_TIFF_DNG) {
    /* origin x, y, then size width, height: 4 LONGs */
    uint8_t crop_bytes[16];
    for(uint32_t index = 0; index < 4; index++) {
      ft_write_little_endian32(crop_bytes + 4 * index,
                               (uint32_t)writer->crop[index]);
    }
    for(uint32_t tag = 0; tag < 2 && result == 0; tag++) {
      if(fseek(writer->file, (long)writer->crop_offsets[tag], SEEK_SET) != 0 ||
         fwrite(crop_bytes + 8 * tag, 1, 8, writer->file) != 8) {
        result = ft_fail(error, "tiff: cannot patch the crop");
      }
    }
  }
  if(fclose(writer->file) != 0 && result == 0) {
    result = ft_fail(error, "tiff: write error on close");
  }
  writer->file = NULL;
  ft_abandon_tiff_writer(writer);
  return result;
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Streaming tiled TIFF / DNG writer for frames too large to hold in RAM.
 *
 * Pixels arrive one COLUMN at a time, left to right (a scan line is an
 * image column: ft_crop_and_orient). Every FT_TILE_PIXELS columns make
 * one column of FT_TILE_PIXELS x FT_TILE_PIXELS tiles. The image size is
 * fixed at open and tiles are uncompressed, so every tile's offset is
 * known before the first pixel: the header (with TileOffsets) is written
 * at open and the tiles are appended strictly in file order, column of
 * tiles by column of tiles; the only seek is the DNG crop patch at close.
 * Tiles past the image edge are zero-padded (TIFF 6.0 section 15).
 * Classic TIFF, so a file is at most 4 GB (2 GB where long is 32-bit:
 * fseek). */
#ifndef FT_TIFF_H
#define FT_TIFF_H

#include <stdint.h>

#include "ft.h"
#include "ft_image.h"

/* a tile's width and height, pixels */
#define FT_TILE_PIXELS 256

/* What kind of file to write. */
typedef enum ft_tiff_kind {
  /* LinearRaw RGB with the DNG 1.4 tags (ft_dng.h) and crop tags */
  FT_TIFF_DNG,
  /* RGB, 3 x 16 bits */
  FT_TIFF_RGB,
  /* BlackIsZero grey, 1 x 16 bits */
  FT_TIFF_GRAY
} ft_tiff_kind;

/* What to write. */
typedef struct ft_tiff_options {
  ft_tiff_kind kind;
  /* columns (scan lines) */
  uint32_t width;
  /* rows (CCD pixels) */
  uint32_t height;
  /* make, model, description, ppi and xyz_to_rgb come from here; its
   * pixels are not used */
  const ft_image* metadata;
  /* RGB and GRAY: an ICC profile to embed, or NULL */
  const uint8_t* icc_profile;
  uint32_t icc_profile_length;
} ft_tiff_options;

/* A file being written. Its fields are private to ft_tiff.c. */
typedef struct ft_tiff_writer ft_tiff_writer;

/* The bytes the file will take, without the header (< 4 KB more). */
double ft_estimate_tiff_bytes(ft_tiff_kind kind, uint32_t width,
                              uint32_t height);

/* Create path and write the header.
 * Returns the writer, or NULL with error filled. */
ft_tiff_writer* ft_open_tiff_writer(const char* path,
                                    const ft_tiff_options* options,
                                    ft_err* error);

/* Write one column: height pixels top to bottom, 1 or 3 samples each
 * (the kind's). One tile column (FT_TILE_PIXELS x height pixels) is
 * buffered, allocated at the first column and freed after the last, when
 * every tile is on disk. Returns 0, or -1 with error filled. */
int32_t ft_write_tiff_column(ft_tiff_writer* writer, const uint16_t* column,
                             ft_err* error);

/* DNG only: set DefaultCropOrigin / DefaultCropSize, written at close
 * and clamped into the image. The default is the whole image. */
void ft_set_tiff_crop(ft_tiff_writer* writer, int32_t x, int32_t y,
                      int32_t crop_width, int32_t crop_height);

/* Finish the file: every column must have arrived. Patches the crop,
 * closes and frees writer. Returns 0, or -1 with error filled (the file
 * is left as written). */
int32_t ft_close_tiff_writer(ft_tiff_writer* writer, ft_err* error);

/* Abandon the file: close and free without finishing (the file stays
 * partial). Safe to call with NULL. */
void ft_abandon_tiff_writer(ft_tiff_writer* writer);

#endif

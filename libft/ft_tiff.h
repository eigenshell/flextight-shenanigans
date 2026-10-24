/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Streaming tiled TIFF / DNG writer for frames too large to hold in RAM.
 *
 * Pixels arrive one COLUMN at a time, left to right (a scan line is an
 * image column: ft_crop_orient). Every FT_TILE columns make one column
 * of FT_TILE x FT_TILE tiles. The image size is fixed at open and tiles
 * are uncompressed, so every tile's offset is known before the first
 * pixel: the header (with TileOffsets) is written at open and the tiles
 * are appended strictly in file order, column of tiles by column of
 * tiles; the only seek is the DNG crop patch at close. Tiles past the
 * image edge are zero-padded (TIFF 6.0 section 15). Classic TIFF, so a
 * file is at most 4 GB (2 GB where long is 32-bit: fseek). */
#ifndef FT_TIFF_H
#define FT_TIFF_H
#include "ft.h"
#include "ft_image.h"

#define FT_TILE 256

typedef enum {
  FT_TIFF_DNG, /* LinearRaw RGB, DNG 1.4 tags (ft_dng.h), crop tags */
  FT_TIFF_RGB, /* RGB, 3 x 16 bit */
  FT_TIFF_GRAY /* BlackIsZero, 1 x 16 bit */
} ft_tiff_kind;

typedef struct {
  ft_tiff_kind kind;
  int w, h;                 /* columns (scan lines), rows (CCD px) */
  const ft_image* meta;     /* make, model, desc, ppi, xyz_to_rgb;
                               its pixels are not used */
  const unsigned char* icc; /* RGB / GRAY: ICC profile or NULL */
  unsigned long icc_n;
} ft_tiff_opts;

typedef struct ft_tiffw ft_tiffw;

/* bytes the file will take (header excluded: < 4 KB more) */
double ft_tiff_bytes(ft_tiff_kind kind, int w, int h);
/* creates path and writes the header; NULL with e filled on failure */
ft_tiffw* ft_tiffw_open(const char* path, const ft_tiff_opts* o, ft_err* e);
/* one column: h pixels top to bottom, 1 or 3 samples each (the kind's).
 * Buffers one tile column (FT_TILE x h pixels), allocated at the first
 * column and freed after the last, when every tile is on disk. */
int ft_tiffw_column(ft_tiffw* w, const unsigned short* col, ft_err* e);
/* DNG only: DefaultCropOrigin / DefaultCropSize, written at close;
 * clamped into the image. Default: the whole image. */
void ft_tiffw_crop(ft_tiffw* w, int x, int y, int cw, int ch);
/* every column must have arrived: patches the crop, closes, frees w.
 * -1 with e filled (file left as written). */
int ft_tiffw_close(ft_tiffw* w, ft_err* e);
/* abandon: close and free without completing (file stays partial) */
void ft_tiffw_free(ft_tiffw* w);

#endif

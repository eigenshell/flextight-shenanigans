/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_tiff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_tiff_ifd.h"

struct ft_tiffw {
  FILE* f;
  ft_tiff_kind kind;
  int w, h, spp, ntx, nty;
  int col;              /* columns received */
  int band_n;           /* columns in the band */
  unsigned short* band; /* FT_TILE columns of h x spp */
  unsigned char* tile;  /* one tile, little-endian */
  unsigned long tile_bytes, crop_off[2];
  int crop[4];
};

static int spp_of(ft_tiff_kind k) { return k == FT_TIFF_GRAY ? 1 : 3; }

double ft_tiff_bytes(ft_tiff_kind kind, int w, int h) {
  double ntx = (w + FT_TILE - 1) / FT_TILE, nty = (h + FT_TILE - 1) / FT_TILE;
  return ntx * nty * FT_TILE * FT_TILE * 2.0 * spp_of(kind);
}

void ft_tiffw_free(ft_tiffw* w) {
  if(!w) return;
  if(w->f) fclose(w->f);
  free(w->band);
  free(w->tile);
  free(w);
}

ft_tiffw* ft_tiffw_open(const char* path, const ft_tiff_opts* o, ft_err* e) {
  ft_tiffw* w;
  ft_ifd d;
  ft_ifd_entry* to;
  unsigned char* p;
  unsigned long data0, n, i;
  int tx, ty;

  if(o->w < 1 ||
     o->h < 1 ||
     !o->meta ||
     (o->kind != FT_TIFF_DNG &&
      o->kind != FT_TIFF_RGB &&
      o->kind != FT_TIFF_GRAY)) {
    ft_fail(e, "tiff: bad image (%dx%d, kind %d)", o->w, o->h, (int)o->kind);
    return NULL;
  }
  if(ft_tiff_bytes(o->kind, o->w, o->h) + 65536.0 > 4.0e9 ||
     (sizeof(long) < 8 &&
      ft_tiff_bytes(o->kind, o->w, o->h) + 65536.0 > 2.0e9)) {
    ft_fail(e, "tiff: %dx%d is too large for one file", o->w, o->h);
    return NULL;
  }
  w = calloc(1, sizeof *w);
  if(!w) {
    ft_fail(e, "tiff: out of memory");
    return NULL;
  }
  w->kind = o->kind;
  w->w = o->w;
  w->h = o->h;
  w->spp = spp_of(o->kind);
  w->ntx = (o->w + FT_TILE - 1) / FT_TILE;
  w->nty = (o->h + FT_TILE - 1) / FT_TILE;
  w->tile_bytes = (unsigned long)FT_TILE * FT_TILE * 2 * w->spp;
  w->crop[2] = o->w;
  w->crop[3] = o->h;

  memset(&d, 0, sizeof d);
  ft_ifd_common(&d, o->meta, o->w, o->h, w->spp, o->kind == FT_TIFF_DNG);
  if(o->kind != FT_TIFF_DNG) {
    ft_ifd_short(&d, 262, o->kind == FT_TIFF_GRAY ? 1 : 2);
    if(o->icc && o->icc_n)
      ft_ifd_bytes(&d, 34675, FT_T_UNDEFINED, o->icc, o->icc_n);
  } else {
    ft_ifd_add(&d, 50719, FT_T_LONG, 2); /* DefaultCropOrigin */
    ft_ifd_add(&d, 50720, FT_T_LONG, 2); /* DefaultCropSize */
  }
  n = (unsigned long)w->ntx * w->nty;
  ft_ifd_long(&d, 322, FT_TILE);     /* TileWidth */
  ft_ifd_long(&d, 323, FT_TILE);     /* TileLength */
  ft_ifd_add(&d, 324, FT_T_LONG, n); /* TileOffsets: below */
  if((p = ft_ifd_add(&d, 325, FT_T_LONG, n)) != NULL)
    for(i = 0; i < n; i++) ft_le32(p + 4 * i, w->tile_bytes);
  if(d.oom) {
    ft_fail(e, "tiff: out of memory");
    goto fail;
  }
  data0 = ft_ifd_layout(&d);
  data0 = (data0 + 15) & ~15UL;
  /* tiles in file order: tile column by tile column, top to bottom;
   * TileOffsets is indexed row-major (ty * ntx + tx) */
  to = ft_ifd_find(&d, 324);
  for(tx = 0; tx < w->ntx; tx++)
    for(ty = 0; ty < w->nty; ty++)
      ft_le32(to->data + 4 * ((unsigned long)ty * w->ntx + tx),
              data0 + ((unsigned long)tx * w->nty + ty) * w->tile_bytes);
  if(o->kind == FT_TIFF_DNG) {
    w->crop_off[0] = ft_ifd_find(&d, 50719)->off;
    w->crop_off[1] = ft_ifd_find(&d, 50720)->off;
  }

  w->f = fopen(path, "wb");
  if(!w->f) {
    ft_fail(e, "tiff: cannot write %s", path);
    goto fail;
  }
  if(ft_ifd_write(&d, w->f)) {
    ft_fail(e, "tiff: write error on %s", path);
    goto fail;
  }
  /* zeros up to the first tile: the file is only ever appended to */
  for(i = (unsigned long)ftell(w->f); i < data0; i++)
    if(fputc(0, w->f) == EOF) {
      ft_fail(e, "tiff: write error on %s", path);
      goto fail;
    }
  ft_ifd_free(&d);
  if(o->kind == FT_TIFF_DNG) ft_tiffw_crop(w, 0, 0, o->w, o->h);
  return w;
fail:
  ft_ifd_free(&d);
  ft_tiffw_free(w);
  return NULL;
}

/* write the band's tile column (band_n columns, zero beyond) */
static int flush_band(ft_tiffw* w) {
  int ty, r, c, s;
  for(ty = 0; ty < w->nty; ty++) {
    unsigned char* q = w->tile;
    for(r = 0; r < FT_TILE; r++) {
      int row = ty * FT_TILE + r;
      for(c = 0; c < FT_TILE; c++)
        for(s = 0; s < w->spp; s++) {
          unsigned v = 0;
          if(row < w->h && c < w->band_n)
            v = w->band[((size_t)c * w->h + row) * w->spp + s];
          q[0] = (unsigned char)v;
          q[1] = (unsigned char)(v >> 8);
          q += 2;
        }
    }
    if(fwrite(w->tile, 1, w->tile_bytes, w->f) != w->tile_bytes) return -1;
  }
  w->band_n = 0;
  return 0;
}

/* the band and tile buffers exist only while columns arrive */
static void free_buffers(ft_tiffw* w) {
  free(w->band);
  free(w->tile);
  w->band = NULL;
  w->tile = NULL;
}

int ft_tiffw_column(ft_tiffw* w, const unsigned short* col, ft_err* e) {
  if(w->col >= w->w) return ft_fail(e, "tiff: more than %d columns", w->w);
  if(!w->band) {
    w->band = malloc((size_t)FT_TILE * w->h * w->spp * sizeof *w->band);
    w->tile = malloc(w->tile_bytes);
    if(!w->band || !w->tile) {
      free_buffers(w);
      return ft_fail(e, "tiff: out of memory for %d rows", w->h);
    }
  }
  memcpy(w->band + (size_t)w->band_n * w->h * w->spp, col,
         (size_t)w->h * w->spp * sizeof *col);
  w->band_n++;
  w->col++;
  if((w->band_n == FT_TILE || w->col == w->w) && flush_band(w))
    return ft_fail(e, "tiff: write error");
  if(w->col == w->w) free_buffers(w);
  return 0;
}

void ft_tiffw_crop(ft_tiffw* w, int x, int y, int cw, int ch) {
  if(x < 0) {
    cw += x;
    x = 0;
  }
  if(y < 0) {
    ch += y;
    y = 0;
  }
  if(x > w->w - 1) x = w->w - 1;
  if(y > w->h - 1) y = w->h - 1;
  if(cw > w->w - x) cw = w->w - x;
  if(ch > w->h - y) ch = w->h - y;
  if(cw < 1) cw = 1;
  if(ch < 1) ch = 1;
  w->crop[0] = x;
  w->crop[1] = y;
  w->crop[2] = cw;
  w->crop[3] = ch;
}

int ft_tiffw_close(ft_tiffw* w, ft_err* e) {
  int rc = 0;
  if(w->col != w->w)
    rc = ft_fail(e, "tiff: %d of %d columns arrived", w->col, w->w);
  if(!rc && w->kind == FT_TIFF_DNG) {
    unsigned char b[16];
    int i;
    for(i = 0; i < 4; i++) ft_le32(b + 4 * i, (unsigned long)w->crop[i]);
    for(i = 0; i < 2 && !rc; i++)
      if(fseek(w->f, (long)w->crop_off[i], SEEK_SET) != 0 ||
         fwrite(b + 8 * i, 1, 8, w->f) != 8)
        rc = ft_fail(e, "tiff: cannot patch the crop");
  }
  if(fclose(w->f) != 0 && !rc) rc = ft_fail(e, "tiff: write error on close");
  w->f = NULL;
  ft_tiffw_free(w);
  return rc;
}

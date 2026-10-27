/* SPDX-License-Identifier: GPL-2.0-or-later */
/* One pass of a detailed scan (sub-project 2b), developed and written
 * while it arrives: the worker side of the scan. Feed it the pass's raw
 * lines in order (ft_detail_line); it develops each (ft_stream), keeps a
 * box-averaged thumbnail at the detection grid, cuts every frame's lines
 * out by the crop map (frame box + margin, from the preview), encodes
 * them (ft_density.h) and appends them to that frame's tiled file
 * (ft_tiff.h). Nothing is held whole: memory is the stream's ring, the
 * thumbnail and one tile column per frame receiving lines.
 *
 * Files are written as <name>.partial and renamed when complete. At the
 * end (ft_detail_close) ft_detect runs on the thumbnail; each frame is
 * matched to the detected frame overlapping it most, and the detected box
 * (creep since the preview) goes into the DNG's DefaultCrop and the
 * frame's sidecar <name>.txt. A frame not found keeps the preview box
 * shifted by the median shift of the frames found, marked estimated. */
#ifndef FT_DETAIL_H
#define FT_DETAIL_H
#include <stddef.h>

#include "ft.h"
#include "ft_develop.h"
#include "ft_dplan.h"
#include "ft_holder.h"
#include "ft_image.h"
#include "ft_job.h"
#include "ft_tiff.h"

typedef struct {
  int index;             /* job frame number */
  double y0, y1, x0, x1; /* the preview's box, holder mm */
} ft_detail_frame;

typedef struct {
  ft_dscan scan; /* the pass as sent */
  ft_film film;
  ft_detail_out out;   /* NEG or POS */
  ft_detail_fmt fmt;   /* NEG: DNG or TIFF (POS: TIFF); a
                          negative TIFF carries a linear ICC
                          profile of the scanner RGB (ft_icc) */
  ft_detail_bw bw;     /* POS: OFF (colour), MONO, RGB */
  double film_base[3]; /* POS: D of the film base per channel */
  double margin_mm;    /* kept around each frame box */
  const ft_holder* holder;
  const ft_defects* defects; /* NULL: none */
  const ft_image* meta;      /* make, model, xyz_to_rgb */
  const char* base;          /* files <base>_frameNN.dng / .tif */
  int pass;
  long focus;   /* the pass's focus offset (sidecar) */
  int keep_raw; /* also <base>_passNN.raw (+ .cal .faul
                   .txt), the lines as they arrived */
  int nframes;
  ft_detail_frame f[FT_MAX_FRAMES];
} ft_detail_opts;

typedef struct {
  int index;
  char path[600];  /* the file (final name) */
  int w, h;        /* its pixels: lines along, px across */
  int box[4];      /* the frame in it: x, y, w, h */
  double shift_mm; /* along: detected - preview */
  int estimated;   /* box from the preview, not detected */
  int edge;        /* box touches the file edge: creep
                      beyond the margin, frame may be cut */
} ft_detail_frame_result;

typedef struct {
  long lines;   /* raw lines received */
  int detected; /* ft_detect ran on the thumbnail */
  int nframes;
  ft_detail_frame_result f[FT_MAX_FRAMES];
} ft_detail_result;

typedef struct ft_detail ft_detail;

/* the tiled file a frame becomes for these choices */
ft_tiff_kind ft_detail_kind(ft_detail_out out, ft_detail_fmt fmt,
                            ft_detail_bw bw);
/* <base>_frameNN<.dng|.tif> into buf; -1 if it does not fit */
int ft_detail_path(const char* base, int index, int tiff, char* buf,
                   size_t cap);
/* creates every frame's .partial file (header written) and, with
 * keep_raw, the raw files. cal / faul: the pass's calibration lines and
 * Faul Pixs dump (ft_stream_open). NULL with e filled. */
ft_detail* ft_detail_open(const ft_detail_opts* o, const unsigned short* cal,
                          int cal_lines, const char* faul, size_t faul_n,
                          ft_err* e);
/* one raw line (scan.px x 3), in order; -1 with e filled */
int ft_detail_line(ft_detail* d, const unsigned short* raw, ft_err* e);
/* finishes the pass and frees d. 0: every frame complete, cropped,
 * sidecar written, renamed (r filled). -1 with e filled: incomplete scan
 * or I/O error; unfinished frames stay .partial (r filled as far as it
 * got). */
int ft_detail_close(ft_detail* d, ft_detail_result* r, ft_err* e);
/* abandons the pass and frees d: every frame stays .partial */
void ft_detail_abort(ft_detail* d);

#endif

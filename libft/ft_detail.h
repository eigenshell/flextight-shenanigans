/* SPDX-License-Identifier: GPL-2.0-or-later */
/* One pass of a detailed scan (sub-project 2b), developed and written
 * while it arrives: the worker side of the scan. Feed it the pass's raw
 * lines in order (ft_add_detail_line); it develops each (ft_stream),
 * keeps a box-averaged thumbnail at the detection grid, cuts every
 * frame's lines out by the crop map (frame box + margin, from the
 * preview), encodes them (ft_density.h) and appends them to that frame's
 * tiled file (ft_tiff.h). Nothing is held whole: memory is the stream's
 * ring, the thumbnail and one tile column per frame receiving lines.
 *
 * Files are written as <name>.partial and renamed when complete. At the
 * end (ft_close_detail) ft_detect_frames runs on the thumbnail; each
 * frame is matched to the detected frame overlapping it most, and the
 * detected box (creep since the preview) goes into the DNG's DefaultCrop
 * and the frame's sidecar <name>.txt. A frame not found keeps the preview
 * box shifted by the median shift of the frames found, marked
 * estimated. */
#ifndef FT_DETAIL_H
#define FT_DETAIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ft.h"
#include "ft_develop.h"
#include "ft_dplan.h"
#include "ft_holder.h"
#include "ft_image.h"
#include "ft_job.h"
#include "ft_tiff.h"

/* the longest output path, NUL included */
#define FT_DETAIL_PATH_BYTES 600

/* One frame of the pass, as the preview saw it. */
typedef struct ft_detail_box {
  /* the job's frame number */
  uint32_t index;
  /* the preview's box, holder mm: along start..end, across start..end */
  double start_mm;
  double end_mm;
  double across_start_mm;
  double across_end_mm;
} ft_detail_box;

/* What one pass scans and writes. */
typedef struct ft_detail_options {
  /* the pass as sent */
  ft_detail_scan scan;
  ft_film film;
  /* NEGATIVE or POSITIVE */
  ft_detail_output output;
  /* NEGATIVE: DNG or TIFF (POSITIVE: always TIFF). A negative TIFF
   * carries a linear ICC profile of the scanner RGB (ft_icc). */
  ft_detail_format format;
  /* POSITIVE: OFF (colour), MONO or RGB */
  ft_detail_bw bw;
  /* POSITIVE: the film base density per channel */
  double film_base_density[3];
  /* kept around each frame box */
  double margin_mm;
  const ft_holder* holder;
  /* the user's defects; NULL = none */
  const ft_defects* defects;
  /* make, model, xyz_to_rgb come from here */
  const ft_image* metadata;
  /* the files are <base_path>_frameNN.dng / .tif */
  const char* base_path;
  /* the pass's number */
  int32_t pass;
  /* the pass's focus offset (for the sidecar) */
  int64_t focus_offset_steps;
  /* also write <base_path>_passNN.raw (+ .cal .faul .txt): the lines as
   * they arrived */
  bool keep_raw;
  /* frames in frames[] */
  uint32_t frame_count;
  ft_detail_box frames[FT_MAX_FRAMES];
} ft_detail_options;

/* What happened to one frame. */
typedef struct ft_detail_frame_result {
  /* the job's frame number */
  uint32_t index;
  /* the file (its final name) */
  char path[FT_DETAIL_PATH_BYTES];
  /* its pixels: lines along, pixels across */
  int32_t width;
  int32_t height;
  /* the frame in it: x, y, width, height */
  int32_t box[4];
  /* along: detected - preview */
  double shift_mm;
  /* the box is the preview's, not detected */
  bool is_estimated;
  /* the box touches the file's edge: creep beyond the margin, the frame
   * may be cut */
  bool touches_edge;
} ft_detail_frame_result;

/* What happened to the pass. */
typedef struct ft_detail_result {
  /* raw lines received */
  int64_t lines;
  /* ft_detect_frames ran on the thumbnail */
  bool was_detected;
  /* frames in frames[] */
  uint32_t frame_count;
  ft_detail_frame_result frames[FT_MAX_FRAMES];
} ft_detail_result;

/* A pass being written. Its fields are private to ft_detail.c. */
typedef struct ft_detail ft_detail;

/* The tiled file a frame becomes for these choices. */
ft_tiff_kind ft_get_detail_kind(ft_detail_output output,
                                ft_detail_format format, ft_detail_bw bw);

/* Make <base_path>_frameNN<.dng|.tif> in path.
 * Returns 0, or -1 when it (with ".partial") does not fit capacity. */
int32_t ft_make_detail_path(const char* base_path, uint32_t index, bool is_tiff,
                            char* path, size_t capacity);

/* Create every frame's .partial file (header written) and, with
 * keep_raw, the raw files.
 *
 * calibration   the pass's calibration lines
 * faulty_dump   the Faul Pixs dump (ft_open_stream)
 *
 * Returns the pass, or NULL with error filled. */
ft_detail* ft_open_detail(const ft_detail_options* options,
                          const uint16_t* calibration,
                          uint32_t calibration_lines, const char* faulty_dump,
                          size_t faulty_dump_length, ft_err* error);

/* Add one raw line (scan.pixel_count x 3), in order.
 * Returns 0, or -1 with error filled. */
int32_t ft_add_detail_line(ft_detail* detail, const uint16_t* raw,
                           ft_err* error);

/* Finish the pass and free detail.
 * Returns 0 when every frame is complete, cropped, has its sidecar and
 * is renamed (result filled). Returns -1 with error filled on an
 * incomplete scan or an I/O error; unfinished frames stay .partial
 * (result filled as far as it got). */
int32_t ft_close_detail(ft_detail* detail, ft_detail_result* result,
                        ft_err* error);

/* Abandon the pass and free detail: every frame stays .partial. Safe to
 * call with NULL. */
void ft_abort_detail(ft_detail* detail);

#endif

/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_detail.h"

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_calib.h"
#include "ft_density.h"
#include "ft_detect.h"
#include "ft_geom.h"
#include "ft_icc.h"
#include "ft_stream.h"
#include "ft_tiff.h"

/* room for a path plus ".partial", ".cal" or ".txt" */
#define PATH_WITH_SUFFIX_BYTES (FT_DETAIL_PATH_BYTES + 20)

struct ft_detail {
  ft_detail_options options;
  /* the line developer */
  ft_stream* stream;
  /* the RGB row offset, and the thumbnail's box factor */
  int32_t row_offset;
  int32_t thumbnail_factor;
  uint32_t calibration_lines;
  /* raw lines received, and developed (aligned) lines */
  int64_t raw_lines;
  int64_t aligned_lines;
  /* one developed line, pixel_count x 3 */
  float* line;
  /* that line as stored transmittance, and encoded, pixel_count x 3 */
  uint16_t* stored;
  uint16_t* encoded;
  /* POSITIVE: the density encoder and the mono weights */
  ft_density_encoder* encoder;
  double weights[3];
  /* the thumbnail: lines and pixels; lines summed into the accumulator */
  int32_t thumbnail_lines;
  int32_t thumbnail_pixels;
  int32_t lines_summed;
  float* thumbnail;
  double* accumulator;
  /* per frame: its ranges in the pass (ft_find_scan_ranges), its writer,
   * its file */
  int32_t ranges[FT_MAX_FRAMES][4];
  ft_tiff_writer* writers[FT_MAX_FRAMES];
  char paths[FT_MAX_FRAMES][FT_DETAIL_PATH_BYTES];
  /* keep_raw: the raw file */
  FILE* raw_file;
  char raw_path[FT_DETAIL_PATH_BYTES];
  /* negative TIFFs: the linear scanner RGB profile */
  uint8_t icc_profile[FT_ICC_MAX_BYTES];
  size_t icc_profile_length;
};

int32_t ft_make_detail_path(const char* base_path, uint32_t index, bool is_tiff,
                            char* path, size_t capacity) {
  int written = snprintf(path, capacity, "%s_frame%02u%s", base_path,
                         (unsigned)index, is_tiff ? ".tif" : ".dng");
  if(written < 0 || (size_t)written + sizeof ".partial" > capacity) return -1;
  return 0;
}

static void free_detail(ft_detail* detail) {
  for(uint32_t index = 0; index < detail->options.frame_count; index++) {
    ft_abandon_tiff_writer(detail->writers[index]);
  }
  ft_close_stream(detail->stream);
  if(detail->raw_file != NULL) fclose(detail->raw_file);
  free(detail->line);
  free(detail->stored);
  free(detail->encoded);
  free(detail->encoder);
  free(detail->thumbnail);
  free(detail->accumulator);
  free(detail);
}

void ft_abort_detail(ft_detail* detail) {
  if(detail != NULL) free_detail(detail);
}

/* Write bytes to a new file. Returns false on any error. */
static bool write_file(const char* path, const void* bytes, size_t length) {
  FILE* file = fopen(path, "wb");
  if(file == NULL) return false;
  bool is_written = fwrite(bytes, 1, length, file) == length;
  if(fclose(file) != 0) is_written = false;
  return is_written;
}

ft_tiff_kind ft_get_detail_kind(ft_detail_output output,
                                ft_detail_format format, ft_detail_bw bw) {
  if(output == FT_DETAIL_NEGATIVE) {
    return format == FT_DETAIL_FORMAT_DNG ? FT_TIFF_DNG : FT_TIFF_RGB;
  }
  return bw == FT_BW_MONO ? FT_TIFF_GRAY : FT_TIFF_RGB;
}

static ft_tiff_kind get_kind(const ft_detail_options* options) {
  return ft_get_detail_kind(options->output, options->format, options->bw);
}

static const char* get_bw_word(ft_detail_bw bw) {
  switch(bw) {
    case FT_BW_MONO:
      return "mono";
    case FT_BW_RGB:
      return "rgb";
    case FT_BW_OFF:
      return "off";
  }
  return "off";
}

static bool are_options_valid(const ft_detail_options* options) {
  const ft_detail_scan* scan = &options->scan;
  bool is_known_output = options->output == FT_DETAIL_NEGATIVE ||
                         options->output == FT_DETAIL_POSITIVE;
  return options->frame_count >= 1 &&
         options->frame_count <= FT_MAX_FRAMES &&
         scan->pixel_count >= 1 &&
         scan->line_count >= 1 &&
         scan->bin >= 1 &&
         options->holder != NULL &&
         options->metadata != NULL &&
         options->base_path != NULL &&
         is_known_output;
}

/* Open one frame's .partial file. Returns 0, or -1 with error filled. */
static int32_t open_frame_file(ft_detail* detail, uint32_t index,
                               ft_err* error) {
  const ft_detail_options* options = &detail->options;
  const ft_detail_scan* scan = &options->scan;
  const ft_detail_box* box = &options->frames[index];
  double margin = options->margin_mm;
  if(ft_find_scan_ranges(scan, box->start_mm - margin, box->end_mm + margin,
                         box->across_start_mm - margin,
                         box->across_end_mm + margin,
                         detail->ranges[index]) != 0) {
    return ft_fail(error, "detail: frame %u is not in the pass scan",
                   (unsigned)box->index);
  }
  if(ft_make_detail_path(options->base_path, box->index,
                         get_kind(options) != FT_TIFF_DNG, detail->paths[index],
                         sizeof detail->paths[index]) != 0) {
    return ft_fail(error, "detail: output name too long");
  }

  ft_image metadata = *options->metadata;
  metadata.ppi = scan->ppi;
  if(options->output == FT_DETAIL_NEGATIVE) {
    snprintf(metadata.description, sizeof metadata.description,
             "flextight digital negative: transmittance x 65535; "
             "frame %u, pass %d, %g ppi, bin %d",
             (unsigned)box->index, (int)options->pass, scan->ppi,
             (int)scan->bin);
  } else {
    const double* base = detail->encoder->base_density;
    snprintf(metadata.description, sizeof metadata.description,
             "ft-density v1 base=%.4f %.4f %.4f offset=%.1f "
             "range=%.1f bw=%s w=%.4f %.4f %.4f; frame %u, pass %d, "
             "%g ppi",
             base[0], base[1], base[2], FT_DENSITY_OFFSET, FT_DENSITY_RANGE,
             get_bw_word(options->bw), detail->weights[0], detail->weights[1],
             detail->weights[2], (unsigned)box->index, (int)options->pass,
             scan->ppi);
  }
  ft_tiff_options tiff_options;
  memset(&tiff_options, 0, sizeof tiff_options);
  tiff_options.kind = get_kind(options);
  /* the file holds the frame turned: lines along become columns */
  tiff_options.width =
    (uint32_t)(detail->ranges[index][1] - detail->ranges[index][0]);
  tiff_options.height =
    (uint32_t)(detail->ranges[index][3] - detail->ranges[index][2]);
  tiff_options.metadata = &metadata;
  if(detail->icc_profile_length > 0) {
    tiff_options.icc_profile = detail->icc_profile;
    tiff_options.icc_profile_length = (uint32_t)detail->icc_profile_length;
  }
  char partial[PATH_WITH_SUFFIX_BYTES];
  snprintf(partial, sizeof partial, "%s.partial", detail->paths[index]);
  detail->writers[index] = ft_open_tiff_writer(partial, &tiff_options, error);
  return detail->writers[index] != NULL ? 0 : -1;
}

/* keep_raw: write the .cal and .faul files and open the .raw file.
 * Returns 0, or -1 with error filled. */
static int32_t open_raw_files(ft_detail* detail, const uint16_t* calibration,
                              uint32_t calibration_lines,
                              const char* faulty_dump,
                              size_t faulty_dump_length, ft_err* error) {
  const ft_detail_options* options = &detail->options;
  size_t line_values = (size_t)options->scan.pixel_count * 3;
  int written =
    snprintf(detail->raw_path, sizeof detail->raw_path, "%s_pass%02d.raw",
             options->base_path, (int)options->pass);
  /* + 8: room for the longest suffix added below */
  if(written < 0 || (size_t)written + 8 > sizeof detail->raw_path) {
    return ft_fail(error, "detail: output name too long");
  }
  char path[PATH_WITH_SUFFIX_BYTES];
  snprintf(path, sizeof path, "%s.cal", detail->raw_path);
  if(write_file(path, calibration,
                (size_t)calibration_lines * line_values * 2) == false) {
    return ft_fail(error, "detail: cannot write %s", path);
  }
  snprintf(path, sizeof path, "%s.faul", detail->raw_path);
  if(faulty_dump != NULL &&
     faulty_dump_length > 0 &&
     write_file(path, faulty_dump, faulty_dump_length) == false) {
    return ft_fail(error, "detail: cannot write %s", path);
  }
  detail->raw_file = fopen(detail->raw_path, "wb");
  if(detail->raw_file == NULL) {
    return ft_fail(error, "detail: cannot write %s", detail->raw_path);
  }
  return 0;
}

ft_detail* ft_open_detail(const ft_detail_options* options,
                          const uint16_t* calibration,
                          uint32_t calibration_lines, const char* faulty_dump,
                          size_t faulty_dump_length, ft_err* error) {
  const ft_detail_scan* scan = &options->scan;
  if(are_options_valid(options) == false) {
    ft_fail(
      error, "detail: bad pass (%u frames, %" PRId64 " px, %" PRId64 " lines)",
      (unsigned)options->frame_count, scan->pixel_count, scan->line_count);
    return NULL;
  }
  if(options->output == FT_DETAIL_POSITIVE &&
     options->film != FT_FILM_NEGATIVE) {
    ft_fail(error, "detail: the archive positive is for negative film");
    return NULL;
  }
  ft_detail* detail = calloc(1, sizeof *detail);
  if(detail == NULL) {
    ft_fail(error, "detail: out of memory");
    return NULL;
  }
  size_t line_values = (size_t)scan->pixel_count * 3;
  detail->options = *options;
  detail->calibration_lines = calibration_lines;

  ft_develop_options develop;
  memset(&develop, 0, sizeof develop);
  develop.first_pixel = (int32_t)scan->first_pixel;
  develop.bin = scan->bin;
  develop.defects = options->defects;
  detail->stream =
    ft_open_stream((uint32_t)scan->pixel_count, calibration, calibration_lines,
                   faulty_dump, faulty_dump_length, &develop, error);
  if(detail->stream == NULL) goto fail;
  detail->row_offset = (int32_t)ft_get_stream_row_offset(detail->stream);
  detail->thumbnail_factor = ft_get_thumbnail_factor(scan);
  detail->thumbnail_lines =
    (int32_t)((scan->line_count - 2 * detail->row_offset) /
              detail->thumbnail_factor);
  detail->thumbnail_pixels =
    (int32_t)(scan->pixel_count / detail->thumbnail_factor);
  detail->line = malloc(line_values * sizeof *detail->line);
  detail->stored = malloc(line_values * sizeof *detail->stored);
  detail->encoded = malloc(line_values * sizeof *detail->encoded);
  size_t accumulator_pixels =
    detail->thumbnail_pixels > 0 ? (size_t)detail->thumbnail_pixels : 1;
  detail->accumulator =
    calloc(accumulator_pixels * 3, sizeof *detail->accumulator);
  bool has_thumbnail =
    detail->thumbnail_lines > 0 && detail->thumbnail_pixels > 0;
  if(has_thumbnail) {
    detail->thumbnail = malloc((size_t)detail->thumbnail_lines *
                               detail->thumbnail_pixels *
                               3 *
                               sizeof *detail->thumbnail);
  }
  if(detail->line == NULL ||
     detail->stored == NULL ||
     detail->encoded == NULL ||
     detail->accumulator == NULL ||
     (has_thumbnail && detail->thumbnail == NULL)) {
    ft_fail(error, "detail: out of memory");
    goto fail;
  }

  if(options->output == FT_DETAIL_POSITIVE) {
    double variance[3];
    detail->encoder = malloc(sizeof *detail->encoder);
    if(detail->encoder == NULL) {
      ft_fail(error, "detail: out of memory");
      goto fail;
    }
    ft_init_density_encoder(detail->encoder, options->film_base_density);
    if(ft_measure_white_variance(calibration, calibration_lines,
                                 (uint32_t)scan->pixel_count, variance,
                                 error) != 0) {
      goto fail;
    }
    ft_compute_noise_weights(variance, detail->weights);
  }

  if(get_kind(options) == FT_TIFF_RGB &&
     options->output == FT_DETAIL_NEGATIVE) {
    detail->icc_profile_length = ft_build_linear_icc_profile(
      options->metadata->xyz_to_rgb,
      "Flextight scanner RGB, linear (flextight)", detail->icc_profile,
      sizeof detail->icc_profile);
    if(detail->icc_profile_length == 0) {
      ft_fail(error, "detail: the scanner colour matrix is singular");
      goto fail;
    }
  }
  for(uint32_t index = 0; index < options->frame_count; index++) {
    if(open_frame_file(detail, index, error) != 0) goto fail;
  }
  if(options->keep_raw &&
     open_raw_files(detail, calibration, calibration_lines, faulty_dump,
                    faulty_dump_length, error) != 0) {
    goto fail;
  }
  return detail;

fail:
  free_detail(detail);
  return NULL;
}

/* Box-average the developed line into the thumbnail: add it to the
 * accumulator, and every thumbnail_factor lines write one thumbnail
 * row. */
static void add_to_thumbnail(ft_detail* detail) {
  int32_t factor = detail->thumbnail_factor;
  int64_t row = detail->aligned_lines / factor;
  /* leftover lines past the last whole row are dropped */
  if(row >= detail->thumbnail_lines) return;
  for(int32_t pixel = 0; pixel < detail->thumbnail_pixels; pixel++) {
    for(int32_t step = 0; step < factor; step++) {
      for(uint32_t channel = 0; channel < 3; channel++) {
        detail->accumulator[pixel * 3 + channel] +=
          detail->line[(pixel * factor + step) * 3 + channel];
      }
    }
  }
  detail->lines_summed++;
  if(detail->lines_summed < factor) return;
  float* target =
    detail->thumbnail + (size_t)row * detail->thumbnail_pixels * 3;
  for(int32_t index = 0; index < detail->thumbnail_pixels * 3; index++) {
    target[index] =
      (float)(detail->accumulator[index] / ((double)factor * factor));
    detail->accumulator[index] = 0;
  }
  detail->lines_summed = 0;
}

int32_t ft_add_detail_line(ft_detail* detail, const uint16_t* raw,
                           ft_err* error) {
  const ft_detail_options* options = &detail->options;
  size_t line_values = (size_t)options->scan.pixel_count * 3;
  if(detail->raw_file != NULL &&
     fwrite(raw, 2, line_values, detail->raw_file) != line_values) {
    return ft_fail(error, "detail: write error on %s", detail->raw_path);
  }
  detail->raw_lines++;
  /* 1 = a developed line is ready; 0 = not yet; -1 = error */
  int32_t result =
    ft_push_stream_line(detail->stream, raw, detail->line, error);
  if(result <= 0) return result;

  int64_t line = detail->aligned_lines;
  if(detail->thumbnail != NULL) add_to_thumbnail(detail);
  ft_quantise_transmittance16(detail->line, detail->stored, line_values);
  for(uint32_t index = 0; index < options->frame_count; index++) {
    const int32_t* ranges = detail->ranges[index];
    /* this frame takes lines [ranges[0], ranges[1]) and pixels
     * [ranges[2], ranges[3]) */
    if(line < ranges[0] || line >= ranges[1]) continue;
    const uint16_t* column = detail->stored + (size_t)ranges[2] * 3;
    size_t pixel_count = (size_t)(ranges[3] - ranges[2]);
    if(options->output == FT_DETAIL_POSITIVE) {
      if(options->bw == FT_BW_MONO) {
        ft_encode_density_mono(detail->encoder, detail->weights, column,
                               detail->encoded, pixel_count);
      } else {
        ft_encode_density_rgb(detail->encoder, column, detail->encoded,
                              pixel_count);
      }
      column = detail->encoded;
    }
    if(ft_write_tiff_column(detail->writers[index], column, error) != 0) {
      return -1;
    }
  }
  detail->aligned_lines++;
  return 0;
}

/* The detected frame overlapping the preview box most (at least half its
 * length, centre within margin_mm), or NULL. */
static const ft_detected_frame* match_frame(const ft_layout* layout,
                                            const ft_detail_box* box,
                                            double margin_mm) {
  const ft_detected_frame* best = NULL;
  double best_overlap = 0.5 * (box->end_mm - box->start_mm);
  for(uint32_t index = 0; index < layout->frame_count; index++) {
    const ft_detected_frame* found = &layout->frames[index];
    double overlap_end =
      found->end_mm < box->end_mm ? found->end_mm : box->end_mm;
    double overlap_start =
      found->start_mm > box->start_mm ? found->start_mm : box->start_mm;
    double overlap = overlap_end - overlap_start;
    double shift =
      ((found->start_mm + found->end_mm) - (box->start_mm + box->end_mm)) / 2;
    if(found->type == FT_FRAME_PARTIAL || !(fabs(shift) <= margin_mm)) {
      continue;
    }
    if(overlap >= best_overlap) {
      best_overlap = overlap;
      best = found;
    }
  }
  return best;
}

/* qsort's comparison: ascending doubles */
static int compare_doubles(const void* first, const void* second) {
  double a = *(const double*)first;
  double b = *(const double*)second;
  if(a < b) return -1;
  return a > b ? 1 : 0;
}

/* Write a frame's sidecar <path>.txt. Returns false on an error. */
static bool write_sidecar(const ft_detail* detail, uint32_t index,
                          const ft_detail_frame_result* frame) {
  const ft_detail_options* options = &detail->options;
  const ft_detail_scan* scan = &options->scan;
  const int32_t* ranges = detail->ranges[index];
  char path[PATH_WITH_SUFFIX_BYTES];
  snprintf(path, sizeof path, "%s.txt", detail->paths[index]);
  FILE* file = fopen(path, "w");
  if(file == NULL) return false;
  fprintf(file,
          "frame=%u\npass=%d\nout=%s\nppi=%g\nbin=%d\nfocus_offset=%" PRId64
          "\nw=%d\nh=%d\nframe_box=%d %d %d %d\nmargin_mm=%.2f\n"
          "shift_mm=%.3f\nestimated=%d\nedge=%d\n"
          "scan=%" PRId64 " %" PRId64 " %" PRId64 " %" PRId64
          "\nscan_range=%d %d %d %d\n",
          (unsigned)frame->index, (int)options->pass,
          options->output == FT_DETAIL_NEGATIVE ? "neg" : "pos", scan->ppi,
          (int)scan->bin, options->focus_offset_steps, (int)frame->width,
          (int)frame->height, (int)frame->box[0], (int)frame->box[1],
          (int)frame->box[2], (int)frame->box[3], options->margin_mm,
          frame->shift_mm, frame->is_estimated ? 1 : 0,
          frame->touches_edge ? 1 : 0, scan->first_pixel, scan->first_line,
          scan->pixel_count, scan->line_count, (int)ranges[0], (int)ranges[1],
          (int)ranges[2], (int)ranges[3]);
  if(options->output == FT_DETAIL_POSITIVE) {
    const double* base = detail->encoder->base_density;
    fprintf(file,
            "film_base=%.4f %.4f %.4f\nencoding=ft-density v1\n"
            "offset=%.1f\nrange=%.1f\nbw=%s\nweights=%.4f %.4f %.4f\n",
            base[0], base[1], base[2], FT_DENSITY_OFFSET, FT_DENSITY_RANGE,
            get_bw_word(options->bw), detail->weights[0], detail->weights[1],
            detail->weights[2]);
  } else {
    fprintf(file, "encoding=transmittance x 65535\nfmt=%s\n",
            options->format == FT_DETAIL_FORMAT_DNG ? "dng" : "tiff");
  }
  return fclose(file) == 0;
}

/* Fill a frame's result: its file size and the frame box inside it
 * (detected, else the preview's shifted by median_shift_mm). */
static void place_frame(const ft_detail* detail, uint32_t index,
                        const ft_detected_frame* found, double median_shift_mm,
                        ft_detail_frame_result* frame) {
  const ft_detail_scan* scan = &detail->options.scan;
  const ft_detail_box* box = &detail->options.frames[index];
  const int32_t* ranges = detail->ranges[index];
  frame->index = box->index;
  snprintf(frame->path, sizeof frame->path, "%s", detail->paths[index]);
  frame->width = ranges[1] - ranges[0];
  frame->height = ranges[3] - ranges[2];

  double start_mm;
  double end_mm;
  double across_start_mm;
  double across_end_mm;
  if(found != NULL) {
    start_mm = found->start_mm;
    end_mm = found->end_mm;
    across_start_mm = found->across_start_mm;
    across_end_mm = found->across_end_mm;
  } else {
    start_mm = box->start_mm + median_shift_mm;
    end_mm = box->end_mm + median_shift_mm;
    across_start_mm = box->across_start_mm;
    across_end_mm = box->across_end_mm;
    frame->is_estimated = true;
  }
  frame->shift_mm = ((start_mm + end_mm) - (box->start_mm + box->end_mm)) / 2;

  /* the file is turned: x runs along the strip, y across */
  int64_t box_x = ft_get_scan_line(scan, start_mm) - ranges[0];
  int64_t box_width =
    ft_get_scan_line(scan, end_mm) - ft_get_scan_line(scan, start_mm);
  int64_t box_y = ft_get_scan_pixel(scan, across_start_mm) - ranges[2];
  int64_t box_height = ft_get_scan_pixel(scan, across_end_mm) -
                       ft_get_scan_pixel(scan, across_start_mm);
  if(box_x <= 0 ||
     box_y <= 0 ||
     box_x + box_width >= frame->width ||
     box_y + box_height >= frame->height) {
    frame->touches_edge = true;
  }
  /* clamp the box into the file */
  if(box_x < 0) {
    box_width += box_x;
    box_x = 0;
  }
  if(box_y < 0) {
    box_height += box_y;
    box_y = 0;
  }
  if(box_width > frame->width - box_x) box_width = frame->width - box_x;
  if(box_height > frame->height - box_y) box_height = frame->height - box_y;
  if(box_width < 1) box_width = 1;
  if(box_height < 1) box_height = 1;
  /* safe: all four are within the file, far below 2^31 */
  frame->box[0] = (int32_t)box_x;
  frame->box[1] = (int32_t)box_y;
  frame->box[2] = (int32_t)box_width;
  frame->box[3] = (int32_t)box_height;
}

/* keep_raw: close the .raw file and write its .txt. Returns false on an
 * error. */
static bool finish_raw_files(ft_detail* detail) {
  const ft_detail_options* options = &detail->options;
  const ft_detail_scan* scan = &options->scan;
  bool is_done = fclose(detail->raw_file) == 0;
  detail->raw_file = NULL;
  char path[PATH_WITH_SUFFIX_BYTES];
  snprintf(path, sizeof path, "%s.txt", detail->raw_path);
  FILE* file = fopen(path, "w");
  if(file == NULL) return false;
  fprintf(file,
          "preset=detail\nppi=%g\npx=%" PRId64 "\nx=%" PRId64 "\nlines=%" PRId64
          "\ny=%" PRId64 "\nbin=%d\nkept=%" PRId64
          "\ncal_lines=%u\npass=%d\nfocus_offset=%" PRId64 "\n",
          scan->ppi, scan->pixel_count, scan->first_pixel, scan->line_count,
          scan->first_line, (int)scan->bin, detail->raw_lines,
          (unsigned)detail->calibration_lines, (int)options->pass,
          options->focus_offset_steps);
  if(fclose(file) != 0) is_done = false;
  return is_done;
}

int32_t ft_close_detail(ft_detail* detail, ft_detail_result* result,
                        ft_err* error) {
  const ft_detail_options* options = &detail->options;
  const ft_detail_scan* scan = &options->scan;
  /* static: ft_layout is large, keep it off the stack */
  static ft_layout layout;
  /* errors after the first go here, so the first one is reported */
  ft_err later_error;
  int32_t status = 0;

  memset(result, 0, sizeof *result);
  result->lines = detail->raw_lines;
  result->frame_count = options->frame_count;
  /* the first frame the scan stopped short of, if any */
  int32_t short_frame = -1;
  for(uint32_t index = 0; index < options->frame_count; index++) {
    if(detail->aligned_lines < detail->ranges[index][1] && short_frame < 0) {
      short_frame = (int32_t)index;
    }
  }
  if(short_frame >= 0) {
    int64_t needed =
      detail->ranges[short_frame][1] + 2 * (int64_t)detail->row_offset;
    ft_fail(error,
            "pass %d: the scan ended after %" PRId64
            " lines; frame %u needs "
            "%" PRId64 ": unfinished frames stay .partial",
            (int)options->pass, detail->raw_lines,
            (unsigned)options->frames[short_frame].index, needed);
    status = -1;
  }

  /* creep: the frames found on the thumbnail, matched to the preview
   * boxes */
  if(detail->thumbnail != NULL &&
     detail->aligned_lines >=
       (int64_t)detail->thumbnail_lines * detail->thumbnail_factor) {
    ft_axes axes;
    ft_get_detail_axes(scan, detail->thumbnail_factor, &axes);
    result->was_detected =
      ft_detect_frames(detail->thumbnail, (uint32_t)detail->thumbnail_lines,
                       (uint32_t)detail->thumbnail_pixels, &axes, options->film,
                       options->holder, &layout, &later_error) == 0;
  }
  const ft_detected_frame* matches[FT_MAX_FRAMES];
  double shifts[FT_MAX_FRAMES];
  uint32_t match_count = 0;
  for(uint32_t index = 0; index < options->frame_count; index++) {
    const ft_detail_box* box = &options->frames[index];
    matches[index] = result->was_detected
                       ? match_frame(&layout, box, options->margin_mm)
                       : NULL;
    if(matches[index] != NULL) {
      shifts[match_count] =
        ((matches[index]->start_mm + matches[index]->end_mm) -
         (box->start_mm + box->end_mm)) /
        2;
      match_count++;
    }
  }
  double median_shift_mm = 0;
  if(match_count > 0) {
    qsort(shifts, match_count, sizeof shifts[0], compare_doubles);
    if(match_count % 2 == 1) {
      median_shift_mm = shifts[match_count / 2];
    } else {
      median_shift_mm =
        (shifts[match_count / 2 - 1] + shifts[match_count / 2]) / 2;
    }
  }

  for(uint32_t index = 0; index < options->frame_count; index++) {
    ft_detail_frame_result* frame = &result->frames[index];
    place_frame(detail, index, matches[index], median_shift_mm, frame);
    /* not all its lines arrived: it stays .partial */
    if(detail->aligned_lines < detail->ranges[index][1]) continue;
    if(get_kind(options) == FT_TIFF_DNG) {
      ft_set_tiff_crop(detail->writers[index], frame->box[0], frame->box[1],
                       frame->box[2], frame->box[3]);
    }
    ft_tiff_writer* writer = detail->writers[index];
    detail->writers[index] = NULL;
    if(ft_close_tiff_writer(writer, status != 0 ? &later_error : error) != 0) {
      status = -1;
      continue;
    }
    char partial[PATH_WITH_SUFFIX_BYTES];
    snprintf(partial, sizeof partial, "%s.partial", detail->paths[index]);
    /* rename never replaces a file on Windows: remove it first */
    remove(detail->paths[index]);
    if(write_sidecar(detail, index, frame) == false ||
       rename(partial, detail->paths[index]) != 0) {
      if(status == 0) {
        ft_fail(error, "detail: cannot finish %s", detail->paths[index]);
      }
      status = -1;
    }
  }

  if(detail->raw_file != NULL &&
     finish_raw_files(detail) == false &&
     status == 0) {
    ft_fail(error, "detail: cannot finish %s", detail->raw_path);
    status = -1;
  }
  free_detail(detail);
  return status;
}

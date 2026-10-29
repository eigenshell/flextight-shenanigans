/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_stream.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ft_calib.h"
#include "ft_geom.h"

/* the largest bin */
#define MAX_BIN 12

struct ft_stream {
  /* pixels per line */
  uint32_t pixels;
  /* the RGB row offset k = 12 / bin */
  uint32_t row_offset;
  /* lines in the ring: 2k + 1 */
  uint32_t ring_lines;
  /* raw lines pushed so far */
  uint64_t lines_pushed;
  /* the dark and white reference, pixels x 3 each */
  float* dark;
  float* white;
  /* the last ring_lines flat-fielded lines, pixels x 3 each; line n
   * sits at n % ring_lines */
  float* ring;
  /* a copy of the Faul Pixs dump; NULL = none */
  char* faulty_dump;
  size_t faulty_dump_length;
  /* the user's defects, copied; used when has_defects */
  ft_defects defects;
  bool has_defects;
  /* the scan's first output pixel across, and its bin */
  int32_t first_pixel;
  int32_t bin;
};

void ft_close_stream(ft_stream* stream) {
  if(stream == NULL) return;
  free(stream->dark);
  free(stream->white);
  free(stream->ring);
  free(stream->faulty_dump);
  free(stream);
}

ft_stream* ft_open_stream(uint32_t pixels, const uint16_t* calibration,
                          uint32_t calibration_lines, const char* faulty_dump,
                          size_t faulty_dump_length,
                          const ft_develop_options* options, ft_err* error) {
  if(pixels < 1 || options->bin < 1 || options->bin > MAX_BIN) {
    ft_fail(error, "stream: bad geometry (%u px, bin %d)", (unsigned)pixels,
            (int)options->bin);
    return NULL;
  }
  ft_stream* stream = calloc(1, sizeof *stream);
  if(stream == NULL) {
    ft_fail(error, "stream: out of memory");
    return NULL;
  }
  size_t line_values = (size_t)pixels * 3;
  stream->pixels = pixels;
  /* safe: bin is 1..12 here */
  stream->row_offset = FT_CCD_ROW_SPACING_LINES / (uint32_t)options->bin;
  stream->ring_lines = 2 * stream->row_offset + 1;
  stream->first_pixel = options->first_pixel;
  stream->bin = options->bin;
  if(options->defects != NULL) {
    stream->defects = *options->defects;
    stream->has_defects = true;
  }
  stream->dark = malloc(line_values * sizeof *stream->dark);
  stream->white = malloc(line_values * sizeof *stream->white);
  stream->ring =
    malloc(line_values * stream->ring_lines * sizeof *stream->ring);
  bool wants_dump = faulty_dump != NULL && faulty_dump_length > 0;
  if(wants_dump) {
    stream->faulty_dump = malloc(faulty_dump_length);
    if(stream->faulty_dump != NULL) {
      memcpy(stream->faulty_dump, faulty_dump, faulty_dump_length);
      stream->faulty_dump_length = faulty_dump_length;
    }
  }
  if(stream->dark == NULL ||
     stream->white == NULL ||
     stream->ring == NULL ||
     (wants_dump && stream->faulty_dump == NULL)) {
    ft_fail(error, "stream: out of memory");
    goto fail;
  }
  if(ft_split_calibration(calibration, calibration_lines, pixels, stream->dark,
                          stream->white, error) != 0) {
    goto fail;
  }
  /* check the defect lists once here, on a scratch line */
  if(ft_repair_defects(stream->ring, 1, pixels, stream->faulty_dump,
                       stream->faulty_dump_length,
                       stream->has_defects ? &stream->defects : NULL,
                       stream->first_pixel, stream->bin, error) != 0) {
    goto fail;
  }
  return stream;

fail:
  ft_close_stream(stream);
  return NULL;
}

uint32_t ft_get_stream_row_offset(const ft_stream* stream) {
  return stream->row_offset;
}

int32_t ft_push_stream_line(ft_stream* stream, const uint16_t* raw,
                            float* output, ft_err* error) {
  size_t line_values = (size_t)stream->pixels * 3;
  uint64_t line = stream->lines_pushed;
  uint64_t offset = stream->row_offset;

  /* flat-field and repair the new line into its place in the ring */
  float* newest =
    stream->ring + (size_t)(line % stream->ring_lines) * line_values;
  ft_apply_flat_field(raw, 1, stream->pixels, stream->dark, stream->white,
                      newest);
  if(ft_repair_defects(newest, 1, stream->pixels, stream->faulty_dump,
                       stream->faulty_dump_length,
                       stream->has_defects ? &stream->defects : NULL,
                       stream->first_pixel, stream->bin, error) != 0) {
    return -1;
  }
  stream->lines_pushed++;
  if(line < 2 * offset) return 0;

  /* as ft_align_rgb_rows: output line l = red of raw line l + 2k, green
   * of l + k, blue of l. The newest line is l + 2k. */
  const float* red = newest;
  const float* green =
    stream->ring + (size_t)((line - offset) % stream->ring_lines) * line_values;
  const float* blue =
    stream->ring +
    (size_t)((line - 2 * offset) % stream->ring_lines) * line_values;
  for(size_t pixel = 0; pixel < stream->pixels; pixel++) {
    output[pixel * 3 + 0] = red[pixel * 3 + 0];
    output[pixel * 3 + 1] = green[pixel * 3 + 1];
    output[pixel * 3 + 2] = blue[pixel * 3 + 2];
  }
  return 1;
}

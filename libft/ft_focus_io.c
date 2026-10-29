/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The focus curve file (focus.txt) through a stream. ft_focus.c (with
 * ft_parse_lens) also runs in the browser; these two are for the command
 * line tools. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_focus.h"

/* the read buffer's first size; it doubles when full */
#define FIRST_CAPACITY_BYTES 8192
/* read at most this much at a time, growing first when less is left */
#define READ_CHUNK_BYTES 4096

int32_t ft_write_lens(const ft_lens* lens, FILE* file) {
  fprintf(file,
          "# ftpoc focus curve (thin lens, motor steps)\n"
          "film = %.1f\nw = %.1f\nfl = %.1f\np = %.6e\n",
          lens->film_steps, lens->gap_offset_steps, lens->focal_length_steps,
          lens->pixel_pitch_inches);
  for(uint32_t index = 0;
      index < lens->correction_count && index < FT_LENS_CORRECTION_MAX;
      index++) {
    fprintf(file, "correct = %.0f %.0f\n", lens->correction_scale_ppi[index],
            lens->correction_offset_steps[index]);
  }
  return ferror(file) ? -1 : 0;
}

int32_t ft_read_lens(ft_lens* lens, FILE* file, ft_err* error) {
  char* text = NULL;
  size_t length = 0;
  size_t capacity = 0;
  size_t bytes_read;
  do {
    if(length + READ_CHUNK_BYTES > capacity) {
      capacity = capacity > 0 ? capacity * 2 : FIRST_CAPACITY_BYTES;
      char* bigger = realloc(text, capacity);
      if(bigger == NULL) {
        free(text);
        return ft_fail(error, "focus file: out of memory");
      }
      text = bigger;
    }
    bytes_read = fread(text + length, 1, capacity - length, file);
    length += bytes_read;
  } while(bytes_read > 0);
  int32_t result = ft_parse_lens(lens, text != NULL ? text : "", length, error);
  free(text);
  return result;
}

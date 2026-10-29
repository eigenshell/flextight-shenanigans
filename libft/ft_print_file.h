/* SPDX-License-Identifier: GPL-2.0-or-later */
/* <roll>.print, next to the job file: the roll's emulsion and each
 * frame's C/M/Y/D clicks (ft_print.h) and turn. What Quixote and
 * ftdev print read and write; nothing else about a print is stored.
 *
 *   emulsion = generic-neg
 *   [frame 3]
 *   name = beach_03
 *   cmyd = 0 2 0 -1
 *   rotate = 90
 *
 * emulsion comes first, before any [frame N]. A frame without a section
 * prints at auto (0 0 0 0), as scanned; rotate (degrees clockwise: 90,
 * 180, 270; 0 = as scanned) turns the print. The writer leaves out
 * cmyd = 0 0 0 0 beside a rotate or a name, and rotate = 0 always. name
 * (optional, the frame's file name without extension: 1..64 of A-Z a-z
 * 0-9 . _ -, not starting with '.') comes first in its section. Syntax
 * as the job file: '#' starts a comment. The writer is byte-stable
 * (parse -> write -> parse -> write gives the same bytes) and sorts
 * frames by number.
 * Pure C99 (buffers only): runs in the browser too. */
#ifndef FT_PRINT_FILE_H
#define FT_PRINT_FILE_H

#include <stddef.h>
#include <stdint.h>

#include "ft.h"
#include "ft_print.h"

/* an emulsion id, with its NUL */
#define FT_PRINT_ID_BYTES 32
/* a frame name, with its NUL */
#define FT_PRINT_NAME_BYTES 65

/* One [frame N] section. */
typedef struct ft_print_file_frame {
  /* the job's frame number, 0..9999 */
  int32_t index;
  /* the line [frame N] was read from; 0 = set, not read */
  uint32_t source_line;
  ft_cmyd clicks;
  /* degrees clockwise: 0 90 180 270 */
  int32_t rotation_degrees;
  /* "" = none (Quixote's auto name) */
  char name[FT_PRINT_NAME_BYTES];
} ft_print_file_frame;

/* A <roll>.print. */
typedef struct ft_print_file {
  char emulsion[FT_PRINT_ID_BYTES];
  uint32_t frame_count;
  /* sorted by index */
  ft_print_file_frame frames[FT_MAX_FRAMES];
} ft_print_file;

/* Start a print file with no frames; emulsion must be an id
 * (ft_emulsion.h). */
void ft_init_print_file(ft_print_file* print_file, const char* emulsion);

/* Read a .print. Errors name the line: "line 4: cmyd: 'x' is not four
 * whole numbers (c m y d)". Returns 0, or -1 with error filled. */
int32_t ft_parse_print_file(const char* text, size_t text_length,
                            ft_print_file* print_file, ft_err* error);

/* Write a .print as text (NUL-terminated when capacity > 0).
 * Returns the length the whole text needs, without the NUL: >= capacity
 * means it was cut short. */
size_t ft_format_print_file(const ft_print_file* print_file, char* text,
                            size_t capacity);

/* The frame's clicks (0 0 0 0 without a section). */
ft_cmyd ft_get_print_clicks(const ft_print_file* print_file, int32_t frame);

/* Set a frame's clicks (all 0: its section goes).
 * Returns 0, or -1 with error filled on bad clicks for the emulsion, a
 * bad frame number or too many frames. */
int32_t ft_set_print_clicks(ft_print_file* print_file, int32_t frame,
                            ft_cmyd clicks, ft_err* error);

/* The frame's turn, degrees clockwise (0 without a section). */
int32_t ft_get_print_rotation(const ft_print_file* print_file, int32_t frame);

/* Set a frame's turn (0 90 180 270; a frame at 0 with auto clicks loses
 * its section). Returns 0, or -1 with error filled on another angle, a
 * bad frame number or too many frames. */
int32_t ft_set_print_rotation(ft_print_file* print_file, int32_t frame,
                              int32_t degrees, ft_err* error);

/* The frame's name ("" without one). */
const char* ft_get_print_name(const ft_print_file* print_file, int32_t frame);

/* Set a frame's name ("" clears it; a frame with nothing else loses its
 * section). Returns 0, or -1 with error filled on a name that is not
 * file-safe, a bad frame number or too many frames. */
int32_t ft_set_print_name(ft_print_file* print_file, int32_t frame,
                          const char* name, ft_err* error);

/* Count the [frame N] sections whose N is not in job_frames[0..count);
 * with any, warning receives "[frame 7] is not in the job: ignored"
 * ("[frame 7] and 2 more are ..."). */
uint32_t ft_count_unknown_print_frames(const ft_print_file* print_file,
                                       const int32_t* job_frames,
                                       uint32_t count, ft_err* warning);

#endif

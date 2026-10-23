/* SPDX-License-Identifier: GPL-2.0-or-later */
/* <roll>.print, next to the job file: the roll's emulsion and each
 * frame's C/M/Y/D clicks (ft_print.h) and turn. What the web UI and
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
 * as the job file: '#' starts a comment. The writer is byte-stable (parse ->
 * write
 * -> parse -> write gives the same bytes) and sorts frames by number.
 * Pure C99 (buffers only): runs in the browser too. */
#ifndef FT_PRINT_FILE_H
#define FT_PRINT_FILE_H
#include "ft.h"
#include "ft_print.h"
#include <stddef.h>

#define FT_PRINT_ID 32   /* emulsion id, with its NUL */
#define FT_PRINT_NAME 65 /* frame name, with its NUL */

typedef struct {
  int index; /* the job's frame number, 0..9999 */
  int line;  /* where [frame N] was read; 0 = set */
  ft_cmyd k;
  int rotate;               /* degrees clockwise: 0 90 180 270 */
  char name[FT_PRINT_NAME]; /* "" = none (the page's auto name) */
} ft_print_file_frame;

typedef struct {
  char emulsion[FT_PRINT_ID];
  int nframes;
  ft_print_file_frame f[FT_MAX_FRAMES]; /* sorted by index */
} ft_print_file;

/* no frames; emulsion must be an id (ft_emulsion.h) */
void ft_print_file_init(ft_print_file *pf, const char *emulsion);
/* text -> pf; errors name the line: "line 4: cmyd: 'x' is not four whole
 * numbers (c m y d)". -1 with e filled. */
int ft_print_file_parse(
    const char *text, size_t n, ft_print_file *pf, ft_err *e
);
/* pf -> text in buf (NUL-terminated if cap > 0); returns the length the
 * whole text needs, without the NUL: >= cap means truncated */
size_t ft_print_file_write(const ft_print_file *pf, char *buf, size_t cap);
/* the frame's clicks (0 0 0 0 without a section) */
ft_cmyd ft_print_file_clicks(const ft_print_file *pf, int frame);
/* set a frame's clicks (all 0: its section goes); -1 with e filled on bad
 * clicks for the emulsion, a bad frame number or too many frames */
int ft_print_file_set(ft_print_file *pf, int frame, ft_cmyd k, ft_err *e);
/* the frame's turn, degrees clockwise (0 without a section) */
int ft_print_file_rotation(const ft_print_file *pf, int frame);
/* set a frame's turn (0 90 180 270; a frame at 0 with auto clicks loses
 * its section); -1 with e filled on another angle, a bad frame number or
 * too many frames */
int ft_print_file_set_rotation(ft_print_file *pf, int frame, int deg, ft_err *e);
/* the frame's name ("" without one) */
const char *ft_print_file_name(const ft_print_file *pf, int frame);
/* set a frame's name ("" clears it; a frame with nothing else loses its
 * section); -1 with e filled on a name that is not file-safe, a bad frame
 * number or too many frames */
int ft_print_file_set_name(
    ft_print_file *pf, int frame, const char *name, ft_err *e
);
/* the [frame N] sections whose N is not in frames[0..n): their count,
 * and with any, a warning in w: "[frame 7] is not in the job: ignored"
 * ("[frame 7] and 2 more are ...") */
int ft_print_file_unknown(
    const ft_print_file *pf, const int *frames, int n, ft_err *w
);

#endif

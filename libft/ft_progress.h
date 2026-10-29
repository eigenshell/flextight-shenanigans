/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Sancho --progress: one machine-readable line per event, for the web
 * server and page (the human output stays as it is around them):
 *
 *   progress begin window 3        a run of 3 units (preview, window,
 *                                  frame or pass)
 *   progress begin upload 1        the scanner was in its loader: ftpoc
 *                                  uploads the microcode first (before
 *                                  the command's own run; no units)
 *   progress unit window 2 5      unit 2 of the run: the job's window 5
 *   progress lines 812/1890        lines read of the current unit
 *   progress focus 2               focus crop 2 of the current unit
 *                                  (before its own scan; no lines)
 *   progress focus 2 frame 4 at -500   the same, on frame 4's band at
 *                                  focus offset -500 (tandem steps)
 *   progress sharp -500 0.0412     that crop's sharpness
 *   progress focused +620 measured the unit's focus offset: measured (a
 *                                  bracket), checked (a short one at
 *                                  higher magnification), carried (an
 *                                  earlier unit's, no crops) or none
 *                                  (no peak: the offset used anyway)
 *   progress file C:\scans\a.raw   a finished file (the rest of the line)
 *   progress end ok                ok, stopped, failed or refused
 *
 * Formatting and parsing into buffers only, so it runs in the browser. */
#ifndef FT_PROGRESS_H
#define FT_PROGRESS_H

#include <stddef.h>
#include <stdint.h>

/* The event a line reports: the word after "progress". */
typedef enum ft_progress_type {
  FT_PROGRESS_BEGIN,
  FT_PROGRESS_UNIT,
  FT_PROGRESS_LINES,
  FT_PROGRESS_FILE,
  FT_PROGRESS_END,
  FT_PROGRESS_FOCUS,
  FT_PROGRESS_SHARP,
  FT_PROGRESS_FOCUSED
} ft_progress_type;

/* What one unit of a run is. */
typedef enum ft_progress_kind {
  FT_PROGRESS_PREVIEW,
  FT_PROGRESS_WINDOW,
  FT_PROGRESS_FRAME,
  FT_PROGRESS_PASS,
  FT_PROGRESS_UPLOAD
} ft_progress_kind;

/* How a run ended. */
typedef enum ft_progress_status {
  FT_PROGRESS_OK,
  FT_PROGRESS_STOPPED,
  FT_PROGRESS_FAILED,
  FT_PROGRESS_REFUSED
} ft_progress_status;

/* Where a unit's focus offset came from. */
typedef enum ft_progress_how {
  FT_PROGRESS_MEASURED,
  FT_PROGRESS_CHECKED,
  FT_PROGRESS_CARRIED,
  FT_PROGRESS_NONE
} ft_progress_how;

/* One progress line as values. Which fields count depends on type. */
typedef struct ft_progress {
  /* the event */
  ft_progress_type type;
  /* begin, unit: what the units are */
  ft_progress_kind kind;
  /* begin: units in the run (0..); unit: this unit (1..) */
  int32_t number;
  /* unit: its number in the job (0..); focus: the frame (0: none given) */
  int32_t label;
  /* lines: lines read so far (0..total); focus: the crop (1..) */
  int64_t step;
  /* lines: lines in the unit */
  int64_t total;
  /* focus (with a frame), sharp, focused: the focus offset */
  int64_t offset;
  /* sharp: the sharpness, >= 0, 4 decimals on the line */
  double sharpness;
  /* focused: where the offset came from */
  ft_progress_how how;
  /* end: how the run ended */
  ft_progress_status status;
  /* file: the file's path; not empty, no '\n' or '\r' */
  char path[260];
} ft_progress;

/* ft_format_progress's answer for a line the format cannot carry */
#define FT_PROGRESS_BAD ((size_t)-1)

/* Format one progress line, '\n' included, into line.
 *
 * progress       the event
 * line           receives the line, NUL-terminated when capacity > 0
 * line_capacity  bytes available in line
 *
 * Returns the length the whole line needs (>= line_capacity: it was cut
 * short), or FT_PROGRESS_BAD (line empty) when a field is out of range. */
size_t ft_format_progress(const ft_progress* progress, char* line,
                          size_t line_capacity);

/* Parse one line of output.
 *
 * line         the line; a '\n' or "\r\n" at the end is fine
 * line_length  its length in bytes
 * progress     receives the event
 *
 * Returns 0, or -1 when the line is not a well-formed progress line
 * (other output). */
int32_t ft_parse_progress(const char* line, size_t line_length,
                          ft_progress* progress);

#endif

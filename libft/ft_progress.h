/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ftpoc --progress: one machine-readable line per event, for the web
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
 * Formatting and parsing into buffers only (the browser tier). */
#ifndef FT_PROGRESS_H
#define FT_PROGRESS_H
#include <stddef.h>

typedef enum {
  FT_PG_BEGIN,
  FT_PG_UNIT,
  FT_PG_LINES,
  FT_PG_FILE,
  FT_PG_END,
  FT_PG_FOCUS,
  FT_PG_SHARP,
  FT_PG_FOCUSED
} ft_pg_type;
typedef enum {
  FT_PG_PREVIEW,
  FT_PG_WINDOW,
  FT_PG_FRAME,
  FT_PG_PASS,
  FT_PG_UPLOAD
} ft_pg_kind;
typedef enum {
  FT_PG_OK,
  FT_PG_STOPPED,
  FT_PG_FAILED,
  FT_PG_REFUSED
} ft_pg_status;
typedef enum {
  FT_PG_MEASURED,
  FT_PG_CHECKED,
  FT_PG_CARRIED,
  FT_PG_NONE
} ft_pg_how;

typedef struct {
  ft_pg_type type;
  ft_pg_kind kind;     /* begin, unit */
  int n;               /* begin: units in the run (0..);
                          unit: this one (1..) */
  int label;           /* unit: its number in the job (0..);
                          focus: the frame (0: none given) */
  long k, total;       /* lines: 0 <= k <= total;
                          focus: k, the crop (1..) */
  long off;            /* focus (with a frame), sharp, focused:
                          the focus offset */
  double sharp;        /* sharp: >= 0, 4 decimals on the line */
  ft_pg_how how;       /* focused */
  ft_pg_status status; /* end */
  char path[260];      /* file: no '\n' or '\r', not empty */
} ft_progress;

#define FT_PG_BAD ((size_t)-1)
/* p -> one line with its '\n' in buf (NUL-terminated if cap > 0); the
 * length the whole line needs: >= cap means truncated. FT_PG_BAD (buf
 * empty) for fields the format cannot carry. */
size_t ft_progress_fmt(const ft_progress* p, char* buf, size_t cap);
/* line[0..n) (a '\n' or "\r\n" at the end is fine) -> p; 0, or -1 when
 * it is not a well-formed progress line (other output) */
int ft_progress_parse(const char* line, size_t n, ft_progress* p);

#endif

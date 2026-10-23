/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Looks and roll edits: what the web UI edits and ftdev export renders.
 *
 *   look   <name>.look: [stock] steers the auto-estimate for a film (hints
 *          learned from the user's own rolls, ft_stock_learn), [taste] is
 *          the portable rendering choice (grade, paper, curves, exposure
 *          and tint offsets, NR and sharpening).
 *   edit   <roll>.edit, next to the job file: the roll's look, copied in
 *          when applied (later changes to the library never reach it),
 *          plus [frame N] sections that override any taste key for one
 *          frame. A .look is a valid .edit with no frames.
 *
 * A frame's recipe is resolved, never stored: the roll's taste with the
 * frame's overrides (a frame key replaces the roll's), absolute fields
 * into the recipe, the film base, ft_render_auto, the stock's range ratio,
 * then exposure (offset from the auto value) and tint (offset from 0).
 *
 * Text format: the job file's (ft_job.h): `key = value`, `[section]`, '#'
 * starts a comment. Numbers are written with at most 6 decimals, so
 * parse -> write is byte-stable. Pure C99 (no I/O, no allocation): it runs
 * in the browser too. An ft_edit is ~100 KB: static or heap, not stack. */
#ifndef FT_LOOK_H
#define FT_LOOK_H
#include "ft.h"
#include "ft_image.h"
#include "ft_render.h"
#include <stddef.h>

#define FT_STOCK_VERSION 1    /* the newest [stock] this code reads */
#define FT_LOOK_CURVE_TXT 320 /* 16 points of "0.123456,0.123456 " */
#define FT_LOOK_NAME 64
#define FT_EDIT_ROLL (-1) /* scope of an edit operation: the roll */

/* ft_taste.set: which keys are present (in table order) */
enum {
  FT_T_EXPOSURE = 1u << 0,
  FT_T_TINT = 1u << 1,
  FT_T_GRADE = 1u << 2,
  FT_T_PAPER_BLACK = 1u << 3,
  FT_T_PAPER_WHITE = 1u << 4,
  FT_T_CURVE = 1u << 5,
  FT_T_CURVE_R = 1u << 6,
  FT_T_CURVE_G = 1u << 7,
  FT_T_CURVE_B = 1u << 8,
  FT_T_NR_SENSOR = 1u << 9,
  FT_T_NR_CHROMA = 1u << 10,
  FT_T_NR_GRAIN = 1u << 11,
  FT_T_SHARPEN = 1u << 12
};

typedef struct {
  unsigned set;
  double exposure; /* stops, offset from the auto exposure */
  double tint[3];  /* CC, offset from the auto tint (0) */
  double grade, paper_black, paper_white;
  char curve[4][FT_LOOK_CURVE_TXT]; /* master, r, g, b: a preset name
                                       or "x,y x,y ..." */
  double nr_sensor, nr_chroma, nr_grain, sharpen;
} ft_taste;

typedef struct {
  int present; /* the text has a [stock] section */
  int version;
  int has_ratio;
  double range_ratio[3]; /* range[c] / range[green]; green = 1 */
  int has_bw;
  double bw_weights[3];
  long n_frames; /* frames the ratio was learned from */
} ft_stock;

typedef struct {
  int index;
  ft_taste o;
} ft_edit_frame;

typedef struct {
  char from[FT_LOOK_NAME]; /* the look it was copied from ("" none) */
  ft_stock stock;
  ft_taste taste;
  int nframes;
  ft_edit_frame f[FT_MAX_FRAMES];
} ft_edit;

typedef enum { FT_LOOK_FILE, FT_EDIT_FILE } ft_look_kind;

/* export settings carried by the taste (defaults: ftdev export's) */
typedef struct {
  double nr_sensor, nr_chroma, nr_grain, sharpen;
} ft_look_detail;

void ft_edit_init(ft_edit *ed);
/* the i-th taste key's name and FT_T_* bit (table order); NULL past the
 * last */
const char *ft_taste_key(int i, unsigned *bit);
/* one taste key from its text value, validated; -1 with e filled */
int ft_taste_set(ft_taste *t, const char *key, const char *value, ft_err *e);
/* text -> ed (FT_LOOK_FILE: no `from`, no [frame N]); errors name the
 * line: "line 7: grade: 9 is out of range (0.2..5)" */
int ft_edit_parse(
    const char *text, size_t n, ft_look_kind k, ft_edit *ed, ft_err *e
);
/* ed -> text in buf (always NUL-terminated if cap > 0); returns the length
 * the whole text needs, without the NUL: a result >= cap means truncated */
size_t ft_edit_write(const ft_edit *ed, ft_look_kind k, char *buf, size_t cap);

/* edit operations (the web UI's); frame = a frame index or FT_EDIT_ROLL */
int ft_edit_set(
    ft_edit *ed, int frame, const char *key, const char *value, ft_err *e
);
/* key NULL: every key of that scope */
int ft_edit_clear(ft_edit *ed, int frame, const char *key, ft_err *e);
/* dst's overrides become a copy of src's (none if src has none) */
int ft_edit_copy(ft_edit *ed, int src, int dst, ft_err *e);
/* the look's [stock] and [taste] replace the roll's; frames are kept */
void ft_edit_apply_look(ft_edit *ed, const ft_edit *look, const char *name);

/* the frame's effective taste: the roll's, frame keys replacing them;
 * from_frame (NULL ok) gets the bits that came from the frame */
void ft_edit_taste(
    const ft_edit *ed, int frame, ft_taste *out, unsigned *from_frame
);
void ft_look_detail_of(const ft_taste *t, ft_look_detail *d);
/* the recipe for frames im[0..n) (pooled statistics; usually one):
 * stock and taste NULL = none; base NULL = estimated from the frames.
 * Exposure after the offset is clamped to +-6 stops. The stock's
 * range_ratio applies to negatives only, the taste's tint not to b&w (both
 * ignored there). -1 with e filled */
int ft_look_recipe(
    const ft_stock *s, const ft_taste *t, ft_render_mode m, const float *base,
    const ft_image *const *im, int n, ft_recipe *r, ft_err *e
);

/* stock hints from n frames' measured ratios (range[c] / range[green] of
 * the stock-free auto-estimate): the median per channel, pooled with old
 * (NULL: none) weighted by frame count; old's bw weights are kept.
 * spread (NULL ok) = the largest max - min of the frames' r or b ratios */
int ft_stock_learn(
    const double (*ratio)[3], int n, const ft_stock *old, ft_stock *out,
    double *spread, ft_err *e
);

#endif

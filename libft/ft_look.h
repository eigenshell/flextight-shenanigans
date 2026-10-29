/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Looks and roll edits: what the web UI edits and ftdev export renders.
 *
 *   look   <name>.look: [stock] steers the auto-estimate for a film (hints
 *          learned from the user's own rolls, ft_learn_stock), [taste] is
 *          the portable rendering choice (grade, paper, curves, exposure
 *          and tint offsets, NR and sharpening).
 *   edit   <roll>.edit, next to the job file: the roll's look, copied in
 *          when applied (later changes to the library never reach it),
 *          plus [frame N] sections that override any taste key for one
 *          frame. A .look is a valid .edit with no frames.
 *
 * A frame's recipe is resolved, never stored: the roll's taste with the
 * frame's overrides (a frame key replaces the roll's), absolute fields
 * into the recipe, the film base, ft_estimate_recipe, the stock's range ratio,
 * then exposure (offset from the auto value) and tint (offset from 0).
 *
 * Text format: the job file's (ft_job.h): `key = value`, `[section]`, '#'
 * starts a comment. Numbers are written with at most 6 decimals, so
 * parse -> write is byte-stable. Pure C99 (no I/O, no allocation): it runs
 * in the browser too. An ft_edit is ~100 KB: static or heap, not stack. */
#ifndef FT_LOOK_H
#define FT_LOOK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ft.h"
#include "ft_image.h"
#include "ft_render.h"

/* the newest [stock] this code reads */
#define FT_STOCK_VERSION 1
/* room for a curve's text: 16 points of "0.123456,0.123456 " */
#define FT_LOOK_CURVE_TEXT_BYTES 320
/* room for a look's name, NUL included */
#define FT_LOOK_NAME_BYTES 64
/* the scope of an edit operation: the roll, not one frame */
#define FT_EDIT_ROLL (-1)

/* The bits of ft_taste's set_keys: which keys are present, in table
 * order. */
enum {
  FT_TASTE_EXPOSURE = 1u << 0,
  FT_TASTE_TINT = 1u << 1,
  FT_TASTE_GRADE = 1u << 2,
  FT_TASTE_PAPER_BLACK = 1u << 3,
  FT_TASTE_PAPER_WHITE = 1u << 4,
  FT_TASTE_CURVE = 1u << 5,
  FT_TASTE_CURVE_R = 1u << 6,
  FT_TASTE_CURVE_G = 1u << 7,
  FT_TASTE_CURVE_B = 1u << 8,
  FT_TASTE_NR_SENSOR = 1u << 9,
  FT_TASTE_NR_CHROMA = 1u << 10,
  FT_TASTE_NR_GRAIN = 1u << 11,
  FT_TASTE_SHARPEN = 1u << 12
};

/* The portable rendering choice: [taste], or a [frame N]'s overrides. */
typedef struct ft_taste {
  /* FT_TASTE_* bits of the keys present */
  uint32_t set_keys;
  /* stops, an offset from the auto exposure */
  double exposure;
  /* CC, an offset from the auto tint (0) */
  double tint[3];
  double grade;
  double paper_black;
  double paper_white;
  /* master, r, g, b: a preset name or "x,y x,y ..." */
  char curves[4][FT_LOOK_CURVE_TEXT_BYTES];
  double nr_sensor;
  double nr_chroma;
  double nr_grain;
  double sharpen;
} ft_taste;

/* Hints for one film stock, learned from the user's rolls. */
typedef struct ft_stock {
  /* the text has a [stock] section */
  bool is_present;
  int32_t version;
  bool has_ratio;
  /* range[c] / range[green]; green = 1 */
  double range_ratio[3];
  bool has_bw;
  double bw_weights[3];
  /* the frames the ratio was learned from */
  int64_t frame_count;
} ft_stock;

/* One frame's overrides in an edit. */
typedef struct ft_edit_frame {
  int32_t index;
  ft_taste overrides;
} ft_edit_frame;

/* A .look or a .edit. */
typedef struct ft_edit {
  /* the look it was copied from; "" = none */
  char from[FT_LOOK_NAME_BYTES];
  ft_stock stock;
  ft_taste taste;
  uint32_t frame_count;
  ft_edit_frame frames[FT_MAX_FRAMES];
} ft_edit;

/* Which text: a .look (no from, no [frame N]) or a .edit. */
typedef enum ft_look_kind { FT_LOOK_FILE, FT_EDIT_FILE } ft_look_kind;

/* The export settings a taste carries (defaults: ftdev export's). */
typedef struct ft_look_detail {
  double nr_sensor;
  double nr_chroma;
  double nr_grain;
  double sharpen;
} ft_look_detail;

/* Start an empty edit. */
void ft_init_edit(ft_edit* edit);

/* The index-th taste key's name and FT_TASTE_* bit (table order); NULL
 * past the last. bit may be NULL. */
const char* ft_get_taste_key(uint32_t index, uint32_t* bit);

/* Set one taste key from its text value, validated.
 * Returns 0, or -1 with error filled. */
int32_t ft_set_taste_key(ft_taste* taste, const char* key, const char* value,
                         ft_err* error);

/* Read a .look or .edit (FT_LOOK_FILE: no from, no [frame N]). Errors
 * name the line: "line 7: grade: 9 is out of range (0.2..5)".
 * Returns 0, or -1 with error filled. */
int32_t ft_parse_edit(const char* text, size_t text_length, ft_look_kind kind,
                      ft_edit* edit, ft_err* error);

/* Write an edit as text (always NUL-terminated when capacity > 0).
 * Returns the length the whole text needs, without the NUL: a result >=
 * capacity means it was cut short. */
size_t ft_format_edit(const ft_edit* edit, ft_look_kind kind, char* text,
                      size_t capacity);

/* The edit operations (Quixote's); frame = a frame index or
 * FT_EDIT_ROLL. Set one key. Returns 0, or -1 with error filled. */
int32_t ft_set_edit_key(ft_edit* edit, int32_t frame, const char* key,
                        const char* value, ft_err* error);

/* Clear one key; key NULL: every key of that scope.
 * Returns 0, or -1 with error filled. */
int32_t ft_clear_edit_key(ft_edit* edit, int32_t frame, const char* key,
                          ft_err* error);

/* destination's overrides become a copy of source's (none if source has
 * none). Returns 0, or -1 with error filled. */
int32_t ft_copy_edit_frame(ft_edit* edit, int32_t source, int32_t destination,
                           ft_err* error);

/* The look's [stock] and [taste] replace the roll's; frames are kept. */
void ft_apply_look(ft_edit* edit, const ft_edit* look, const char* name);

/* The frame's effective taste: the roll's, the frame's keys replacing
 * them. from_frame (may be NULL) receives the bits that came from the
 * frame. */
void ft_get_frame_taste(const ft_edit* edit, int32_t frame, ft_taste* taste,
                        uint32_t* from_frame);

/* The export settings of a taste (taste may be NULL: the defaults). */
void ft_get_look_detail(const ft_taste* taste, ft_look_detail* detail);

/* Resolve the recipe for images[0..image_count) (pooled statistics;
 * usually one). stock and taste NULL = none; base NULL = estimated from
 * the frames. The exposure after the offset is clamped to +-6 stops. The
 * stock's range_ratio applies to negatives only, the taste's tint not to
 * b&w (both ignored there). Returns 0, or -1 with error filled. */
int32_t ft_resolve_recipe(const ft_stock* stock, const ft_taste* taste,
                          ft_render_mode mode, const float* base,
                          const ft_image* const* images, uint32_t image_count,
                          ft_recipe* recipe, ft_err* error);

/* Learn stock hints from frame_count frames' measured ratios (range[c] /
 * range[green] of the stock-free auto-estimate): the median per channel,
 * pooled with old (NULL: none) weighted by frame count; old's bw weights
 * are kept. spread (may be NULL) receives the largest max - min of the
 * frames' r or b ratios. Returns 0, or -1 with error filled. */
int32_t ft_learn_stock(const double (*ratio)[3], uint32_t frame_count,
                       const ft_stock* old, ft_stock* learned, double* spread,
                       ft_err* error);

#endif

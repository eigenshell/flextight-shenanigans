/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_look.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ft_curve.h"
#include "ft_render_auto.h"

/* What kind of value a taste key takes. */
typedef enum key_kind {
  /* one number */
  KIND_NUMBER,
  /* three numbers, r g b */
  KIND_TRIPLE,
  /* a curve: a preset name or points */
  KIND_CURVE
} key_kind;

/* One taste key. */
typedef struct taste_key {
  const char* name;
  uint32_t bit;
  key_kind kind;
  /* the allowed range of a number */
  double low;
  double high;
} taste_key;

/* table order = write order = the FT_TASTE_* bit order */
static const taste_key TASTE_KEYS[] = {
  {"exposure", FT_TASTE_EXPOSURE, KIND_NUMBER, -6, 6},
  {"tint", FT_TASTE_TINT, KIND_TRIPLE, -50, 50},
  {"grade", FT_TASTE_GRADE, KIND_NUMBER, 0.2, 5},
  {"paper_black", FT_TASTE_PAPER_BLACK, KIND_NUMBER, 0, 4},
  {"paper_white", FT_TASTE_PAPER_WHITE, KIND_NUMBER, 0, 4},
  {"curve", FT_TASTE_CURVE, KIND_CURVE, 0, 0},
  {"curve_r", FT_TASTE_CURVE_R, KIND_CURVE, 0, 0},
  {"curve_g", FT_TASTE_CURVE_G, KIND_CURVE, 0, 0},
  {"curve_b", FT_TASTE_CURVE_B, KIND_CURVE, 0, 0},
  {"nr_sensor", FT_TASTE_NR_SENSOR, KIND_NUMBER, 0, 4},
  {"nr_chroma", FT_TASTE_NR_CHROMA, KIND_NUMBER, 0, 8},
  {"nr_grain", FT_TASTE_NR_GRAIN, KIND_NUMBER, 0, 4},
  {"sharpen", FT_TASTE_SHARPEN, KIND_NUMBER, 0, 3},
};
#define TASTE_KEY_COUNT (sizeof TASTE_KEYS / sizeof TASTE_KEYS[0])
/* the first curve key's place in TASTE_KEYS */
#define FIRST_CURVE_KEY 5

/* The [stock] keys, in the order of their stock_key values. */
typedef enum stock_key {
  STOCK_VERSION_KEY,
  STOCK_RANGE_RATIO,
  STOCK_BW_WEIGHTS,
  STOCK_FRAME_COUNT
} stock_key;
static const char* const STOCK_KEYS[] = {"stock_version", "range_ratio",
                                         "bw_weights", "n_frames"};
#define STOCK_KEY_COUNT (sizeof STOCK_KEYS / sizeof STOCK_KEYS[0])

/* the export defaults (ftdev export) */
#define DEFAULT_NR_SENSOR 1.0
#define DEFAULT_NR_CHROMA 2.0
#define DEFAULT_NR_GRAIN 0.0
#define DEFAULT_SHARPEN 0.5
/* frame numbers run 0..MAX_FRAME_NUMBER */
#define MAX_FRAME_NUMBER 9999
/* the longest line a .look or .edit has */
#define MAX_LINE_BYTES 512

/* the same white space as isspace in the C locale, without the locale */
static bool is_space(char character) {
  return character == ' ' ||
         character == '\t' ||
         character == '\r' ||
         character == '\n' ||
         character == '\f' ||
         character == '\v';
}

/* Cut white space from both ends of text, in place. */
static char* trim(char* text) {
  while(is_space(*text)) text++;
  size_t length = strlen(text);
  while(length > 0 && is_space(text[length - 1])) {
    length--;
    text[length] = '\0';
  }
  return text;
}

/* the first character in text, or NULL (strchr is not on the purity
 * list, libft/test/purity.sh) */
static char* find_character(char* text, char character) {
  for(; *text != '\0'; text++) {
    if(*text == character) return text;
  }
  return NULL;
}

static bool is_rest_blank(const char* text) {
  while(is_space(*text)) text++;
  return *text == '\0';
}

/* one finite number and nothing else */
static bool parse_number(const char* text, double* value) {
  char* end;
  *value = strtod(text, &end);
  return end != text && is_rest_blank(end) && isfinite(*value);
}

/* three finite numbers separated by spaces and nothing else */
static bool parse_triple(const char* text, double values[3]) {
  for(uint32_t index = 0; index < 3; index++) {
    char* end;
    values[index] = strtod(text, &end);
    if(end == text || isfinite(values[index]) == false) return false;
    text = end;
  }
  return is_rest_blank(text);
}

/* a whole number in low..high */
static bool parse_whole(const char* text, double low, double high,
                        int64_t* out) {
  double value;
  if(parse_number(text, &value) == false) return false;
  if(value != floor(value) || value < low || value > high) return false;
  *out = (int64_t)value;
  return true;
}

/* the index of a taste key; -1 when it is none */
static int32_t find_taste_key(const char* key) {
  for(size_t index = 0; index < TASTE_KEY_COUNT; index++) {
    if(strcmp(TASTE_KEYS[index].name, key) == 0) return (int32_t)index;
  }
  return -1;
}

/* the index of a [stock] key; -1 when it is none */
static int32_t find_stock_key(const char* key) {
  for(size_t index = 0; index < STOCK_KEY_COUNT; index++) {
    if(strcmp(STOCK_KEYS[index], key) == 0) return (int32_t)index;
  }
  return -1;
}

/* the field of a number key's bit */
static double* get_number_field(ft_taste* taste, uint32_t bit) {
  switch(bit) {
    case FT_TASTE_EXPOSURE:
      return &taste->exposure;
    case FT_TASTE_GRADE:
      return &taste->grade;
    case FT_TASTE_PAPER_BLACK:
      return &taste->paper_black;
    case FT_TASTE_PAPER_WHITE:
      return &taste->paper_white;
    case FT_TASTE_NR_SENSOR:
      return &taste->nr_sensor;
    case FT_TASTE_NR_CHROMA:
      return &taste->nr_chroma;
    case FT_TASTE_NR_GRAIN:
      return &taste->nr_grain;
    default:
      return &taste->sharpen;
  }
}

/* the index into ft_taste's curves of a curve key's bit */
static uint32_t get_curve_slot(uint32_t bit) {
  switch(bit) {
    case FT_TASTE_CURVE:
      return 0;
    case FT_TASTE_CURVE_R:
      return 1;
    case FT_TASTE_CURVE_G:
      return 2;
    default:
      return 3;
  }
}

/* Text to a curve: a preset name or points.
 * Returns 0, or -1 with error naming the presets. */
static int32_t parse_curve_text(const char* key, const char* text,
                                ft_curve* curve, ft_err* error) {
  ft_err inner_error;
  if(ft_find_curve_preset(text, curve) == 0) return 0;
  if(ft_parse_curve(text, curve, &inner_error) == 0) return 0;
  /* drop the "curve: " the inner message starts with */
  const char* message = inner_error.message;
  if(memcmp(message, "curve: ", 7) == 0) message += 7;
  return ft_fail(error,
                 "%s: not a preset (linear soft medium strong fade) "
                 "and not points: %.100s",
                 key, message);
}

void ft_init_edit(ft_edit* edit) { memset(edit, 0, sizeof *edit); }

const char* ft_get_taste_key(uint32_t index, uint32_t* bit) {
  if(index >= TASTE_KEY_COUNT) return NULL;
  if(bit != NULL) *bit = TASTE_KEYS[index].bit;
  return TASTE_KEYS[index].name;
}

/* Normalise a curve's white space to single spaces, so it writes back
 * the same. Returns 0, or -1 with error filled when it is too long. */
static int32_t normalise_curve(const char* key, const char* value,
                               char normalised[FT_LOOK_CURVE_TEXT_BYTES],
                               ft_err* error) {
  size_t length = 0;
  const char* cursor = value;
  while(*cursor != '\0') {
    if(is_space(*cursor)) {
      while(is_space(*cursor)) cursor++;
      if(*cursor != '\0' && length > 0) {
        if(length + 1 >= FT_LOOK_CURVE_TEXT_BYTES) {
          return ft_fail(error, "%s: too long", key);
        }
        normalised[length] = ' ';
        length++;
      }
      continue;
    }
    if(length + 1 >= FT_LOOK_CURVE_TEXT_BYTES) {
      return ft_fail(error, "%s: too long", key);
    }
    normalised[length] = *cursor;
    length++;
    cursor++;
  }
  normalised[length] = '\0';
  return 0;
}

int32_t ft_set_taste_key(ft_taste* taste, const char* key, const char* value,
                         ft_err* error) {
  int32_t index = find_taste_key(key);
  if(index < 0) {
    if(find_stock_key(key) >= 0) {
      return ft_fail(
        error, "%.40s: a [stock] key (roll-wide): put it in [stock]", key);
    }
    return ft_fail(error, "%.40s: unknown key", key);
  }
  const taste_key* found = &TASTE_KEYS[index];
  double values[3];
  switch(found->kind) {
    case KIND_NUMBER:
      if(parse_number(value, &values[0]) == false) {
        return ft_fail(error, "%s: '%.40s' is not a number", found->name,
                       value);
      }
      if(!(values[0] >= found->low && values[0] <= found->high)) {
        return ft_fail(error, "%s: %.40s is out of range (%g..%g)", found->name,
                       value, found->low, found->high);
      }
      *get_number_field(taste, found->bit) = values[0];
      break;
    case KIND_TRIPLE:
      if(parse_triple(value, values) == false) {
        return ft_fail(error, "%s: '%.40s' is not three numbers (r g b)",
                       found->name, value);
      }
      for(uint32_t channel = 0; channel < 3; channel++) {
        if(!(values[channel] >= found->low && values[channel] <= found->high)) {
          return ft_fail(error, "%s: %g is out of range (%g..%g)", found->name,
                         values[channel], found->low, found->high);
        }
      }
      memcpy(taste->tint, values, sizeof values);
      break;
    case KIND_CURVE: {
      char normalised[FT_LOOK_CURVE_TEXT_BYTES];
      ft_curve curve;
      if(normalise_curve(found->name, value, normalised, error) != 0) {
        return -1;
      }
      if(parse_curve_text(found->name, normalised, &curve, error) != 0) {
        return -1;
      }
      memcpy(taste->curves[get_curve_slot(found->bit)], normalised,
             strlen(normalised) + 1);
      break;
    }
  }
  taste->set_keys |= found->bit;
  return 0;
}

/* Set one [stock] key. seen has a bit per stock key already set.
 * Returns 0, or -1 with error filled. */
static int32_t set_stock_key(ft_stock* stock, uint32_t* seen, const char* key,
                             const char* value, ft_err* error) {
  int32_t index = find_stock_key(key);
  if(index < 0) {
    if(find_taste_key(key) >= 0) {
      return ft_fail(error, "%.40s: a taste key: put it in [taste]", key);
    }
    return ft_fail(error, "%.40s: unknown key", key);
  }
  if((*seen & (1u << index)) != 0) return ft_fail(error, "%s: set twice", key);
  *seen |= 1u << index;
  double values[3];
  int64_t number;
  switch((stock_key)index) {
    case STOCK_VERSION_KEY:
      if(parse_whole(value, 1, 1e6, &number) == false) {
        return ft_fail(error, "stock_version: '%.40s' is not a version", value);
      }
      if(number > FT_STOCK_VERSION) {
        return ft_fail(error,
                       "stock_version: %ld needs a newer libft (this one "
                       "reads %d)",
                       (long)number, FT_STOCK_VERSION);
      }
      stock->version = (int32_t)number;
      break;
    case STOCK_RANGE_RATIO:
      if(parse_triple(value, values) == false) {
        return ft_fail(
          error, "range_ratio: '%.40s' is not three numbers (r g b)", value);
      }
      for(uint32_t channel = 0; channel < 3; channel++) {
        if(!(values[channel] >= 0.2 && values[channel] <= 5)) {
          return ft_fail(error, "range_ratio: %g is out of range (0.2..5)",
                         values[channel]);
        }
      }
      if(values[1] != 1) {
        return ft_fail(error,
                       "range_ratio: green must be 1 (the ratios are "
                       "relative to green)");
      }
      memcpy(stock->range_ratio, values, sizeof values);
      stock->has_ratio = true;
      break;
    case STOCK_BW_WEIGHTS:
      if(parse_triple(value, values) == false) {
        return ft_fail(
          error, "bw_weights: '%.40s' is not three numbers (r g b)", value);
      }
      for(uint32_t channel = 0; channel < 3; channel++) {
        if(!(values[channel] >= 0 && values[channel] <= 1)) {
          return ft_fail(error, "bw_weights: %g is out of range (0..1)",
                         values[channel]);
        }
      }
      if(fabs(values[0] + values[1] + values[2] - 1) > 1e-3) {
        return ft_fail(error, "bw_weights: sum to %g, not 1",
                       values[0] + values[1] + values[2]);
      }
      memcpy(stock->bw_weights, values, sizeof values);
      stock->has_bw = true;
      break;
    case STOCK_FRAME_COUNT:
      if(parse_whole(value, 0, 1e9, &number) == false) {
        return ft_fail(error, "n_frames: '%.40s' is not a count", value);
      }
      stock->frame_count = number;
      break;
  }
  return 0;
}

/* the slot of a frame index; -1 when it has none */
static int32_t find_frame(const ft_edit* edit, int32_t index) {
  for(uint32_t slot = 0; slot < edit->frame_count; slot++) {
    if(edit->frames[slot].index == index) return (int32_t)slot;
  }
  return -1;
}

/* The slot of a frame index, created (in index order) when missing.
 * Returns -1 when the edit is full. */
static int32_t get_frame_slot(ft_edit* edit, int32_t index) {
  int32_t found = find_frame(edit, index);
  if(found >= 0) return found;
  if(edit->frame_count == FT_MAX_FRAMES) return -1;
  uint32_t slot = edit->frame_count;
  while(slot > 0 && edit->frames[slot - 1].index > index) {
    edit->frames[slot] = edit->frames[slot - 1];
    slot--;
  }
  memset(&edit->frames[slot], 0, sizeof edit->frames[slot]);
  edit->frames[slot].index = index;
  edit->frame_count++;
  return (int32_t)slot;
}

/* the line's error: "line N: " + the inner message */
static int32_t fail_at_line(ft_err* error, uint32_t line_number,
                            const ft_err* inner_error) {
  return ft_fail(error, "line %u: %.180s", (unsigned)line_number,
                 inner_error->message);
}

/* Where the parser is. */
typedef enum section {
  SECTION_TOP,
  SECTION_STOCK,
  SECTION_TASTE,
  SECTION_FRAME
} section;

/* The parser's state across lines. */
typedef struct parser {
  ft_look_kind kind;
  ft_edit* edit;
  section current;
  /* the taste the keys go into: the roll's or a frame's */
  ft_taste* taste;
  /* a bit per [stock] key set */
  uint32_t stock_seen;
  uint32_t line_number;
  uint32_t stock_line;
  bool has_taste;
  bool has_from;
} parser;

/* Start a [section] (the brackets cut off). Returns 0, or -1 with error
 * filled. */
static int32_t start_section(parser* state, char* name, ft_err* error) {
  ft_edit* edit = state->edit;
  unsigned line_number = state->line_number;
  if(strcmp(name, "stock") == 0) {
    if(edit->stock.is_present) {
      return ft_fail(error, "line %u: [stock] twice", line_number);
    }
    edit->stock.is_present = true;
    state->stock_line = state->line_number;
    state->current = SECTION_STOCK;
    return 0;
  }
  if(strcmp(name, "taste") == 0) {
    if(state->has_taste) {
      return ft_fail(error, "line %u: [taste] twice", line_number);
    }
    state->has_taste = true;
    state->taste = &edit->taste;
    state->current = SECTION_TASTE;
    return 0;
  }
  bool is_frame =
    strlen(name) > 5 && memcmp(name, "frame", 5) == 0 && is_space(name[5]);
  if(is_frame == false) {
    return ft_fail(error, "line %u: unknown section [%.40s]", line_number,
                   name);
  }
  if(state->kind == FT_LOOK_FILE) {
    return ft_fail(error, "line %u: [frame N] belongs in a .edit, not a .look",
                   line_number);
  }
  int64_t index;
  if(parse_whole(name + 6, 0, MAX_FRAME_NUMBER, &index) == false) {
    return ft_fail(error, "line %u: '[%.40s]': the frame is a number 0..9999",
                   line_number, name);
  }
  if(find_frame(edit, (int32_t)index) >= 0) {
    return ft_fail(error, "line %u: [frame %ld] twice", line_number,
                   (long)index);
  }
  if(edit->frame_count == FT_MAX_FRAMES) {
    return ft_fail(error, "line %u: more than %d frames", line_number,
                   FT_MAX_FRAMES);
  }
  /* in file order, so a parsed text writes back the same */
  uint32_t slot = edit->frame_count;
  edit->frame_count++;
  edit->frames[slot].index = (int32_t)index;
  state->taste = &edit->frames[slot].overrides;
  state->current = SECTION_FRAME;
  return 0;
}

/* Read one key = value line. Returns 0, or -1 with error filled. */
static int32_t read_key_value(parser* state, char* key, char* value,
                              ft_err* error) {
  unsigned line_number = state->line_number;
  ft_err inner_error;
  if(state->current == SECTION_TOP) {
    if(state->kind == FT_EDIT_FILE && strcmp(key, "from") == 0) {
      size_t length = strlen(value);
      if(state->has_from) {
        return ft_fail(error, "line %u: from: set twice", line_number);
      }
      if(length == 0 || length >= FT_LOOK_NAME_BYTES) {
        return ft_fail(error, "line %u: from: a name of 1..%d characters",
                       line_number, FT_LOOK_NAME_BYTES - 1);
      }
      memcpy(state->edit->from, value, length + 1);
      state->has_from = true;
      return 0;
    }
    return ft_fail(
      error, "line %u: %.40s: outside a section ([stock], [taste]%s)",
      line_number, key, state->kind == FT_EDIT_FILE ? ", [frame N]" : "");
  }
  if(state->current == SECTION_STOCK) {
    if(set_stock_key(&state->edit->stock, &state->stock_seen, key, value,
                     &inner_error) != 0) {
      return fail_at_line(error, state->line_number, &inner_error);
    }
    return 0;
  }
  int32_t index = find_taste_key(key);
  if(index >= 0 && (state->taste->set_keys & TASTE_KEYS[index].bit) != 0) {
    return ft_fail(error, "line %u: %s: set twice", line_number, key);
  }
  if(ft_set_taste_key(state->taste, key, value, &inner_error) != 0) {
    return fail_at_line(error, state->line_number, &inner_error);
  }
  return 0;
}

int32_t ft_parse_edit(const char* text, size_t text_length, ft_look_kind kind,
                      ft_edit* edit, ft_err* error) {
  parser state;
  memset(&state, 0, sizeof state);
  state.kind = kind;
  state.edit = edit;
  state.current = SECTION_TOP;
  ft_init_edit(edit);
  size_t position = 0;
  while(position < text_length && text[position] != '\0') {
    char line[MAX_LINE_BYTES];
    size_t length = 0;
    state.line_number++;
    while(position < text_length &&
          text[position] != '\0' &&
          text[position] != '\n') {
      if(length + 1 >= sizeof line) {
        return ft_fail(error, "line %u: too long", (unsigned)state.line_number);
      }
      line[length] = text[position];
      length++;
      position++;
    }
    if(position < text_length && text[position] == '\n') position++;
    line[length] = '\0';
    /* '#' always starts a comment */
    char* hash = find_character(line, '#');
    if(hash != NULL) *hash = '\0';
    char* content = trim(line);
    if(content[0] == '\0') continue;

    if(content[0] == '[') {
      size_t content_length = strlen(content);
      if(state.current == SECTION_STOCK &&
         (state.stock_seen & (1u << STOCK_VERSION_KEY)) == 0) {
        return ft_fail(error, "line %u: [stock] has no stock_version",
                       (unsigned)state.stock_line);
      }
      if(content[content_length - 1] != ']') {
        return ft_fail(error, "line %u: '%.40s' is not a [section]",
                       (unsigned)state.line_number, content);
      }
      content[content_length - 1] = '\0';
      if(start_section(&state, trim(content + 1), error) != 0) return -1;
      continue;
    }
    char* equals = find_character(content, '=');
    if(equals == NULL) {
      return ft_fail(error, "line %u: expected key = value",
                     (unsigned)state.line_number);
    }
    *equals = '\0';
    if(read_key_value(&state, trim(content), trim(equals + 1), error) != 0) {
      return -1;
    }
  }
  if(state.current == SECTION_STOCK &&
     (state.stock_seen & (1u << STOCK_VERSION_KEY)) == 0) {
    return ft_fail(error, "line %u: [stock] has no stock_version",
                   (unsigned)state.stock_line);
  }
  return 0;
}

/* --- writing --- */

/* Text going into a caller's buffer: length counts every byte asked
 * for; the buffer keeps what fits and stays NUL-terminated. */
typedef struct text_buffer {
  char* text;
  size_t capacity;
  size_t length;
} text_buffer;

static void append(text_buffer* buffer, const char* text) {
  for(; *text != '\0'; text++, buffer->length++) {
    if(buffer->length + 1 < buffer->capacity) {
      buffer->text[buffer->length] = *text;
    }
  }
  if(buffer->capacity > 0) {
    size_t end =
      buffer->length < buffer->capacity ? buffer->length : buffer->capacity - 1;
    buffer->text[end] = '\0';
  }
}

/* A number rounded to 6 decimals, trailing zeros dropped: "0.9",
 * "-1.25", "3". Written by hand, not printf, so it is the same in every
 * locale and libc. */
static void append_number(text_buffer* buffer, double value) {
  char out[48];
  uint32_t length = 0;
  double millionths = value * 1e6;
  bool is_negative = millionths < 0;
  millionths = floor(fabs(millionths) + 0.5);
  uint64_t rounded = (uint64_t)millionths;
  /* no "-0" */
  if(rounded == 0) is_negative = false;
  uint64_t whole = rounded / 1000000u;
  uint32_t fraction = (uint32_t)(rounded % 1000000u);
  if(is_negative) {
    out[length] = '-';
    length++;
  }
  /* the whole part's digits come out last first: reverse them */
  char digits[24];
  uint32_t digit_count = 0;
  do {
    digits[digit_count] = (char)('0' + whole % 10);
    digit_count++;
    whole /= 10;
  } while(whole != 0);
  while(digit_count > 0) {
    digit_count--;
    out[length] = digits[digit_count];
    length++;
  }
  if(fraction != 0) {
    out[length] = '.';
    length++;
    /* the fraction's digits from the first, until only zeros remain */
    for(uint32_t place = 100000; place >= 1 && fraction != 0; place /= 10) {
      out[length] = (char)('0' + fraction / place);
      length++;
      fraction %= place;
    }
  }
  out[length] = '\0';
  append(buffer, out);
}

static void append_numbers(text_buffer* buffer, const double* values,
                           uint32_t count) {
  for(uint32_t index = 0; index < count; index++) {
    if(index > 0) append(buffer, " ");
    append_number(buffer, values[index]);
  }
}

static void append_taste(text_buffer* buffer, const ft_taste* taste) {
  for(size_t index = 0; index < TASTE_KEY_COUNT; index++) {
    const taste_key* key = &TASTE_KEYS[index];
    if((taste->set_keys & key->bit) == 0) continue;
    append(buffer, key->name);
    append(buffer, " = ");
    switch(key->kind) {
      case KIND_NUMBER:
        /* the cast drops const: get_number_field only reads here */
        append_number(buffer, *get_number_field((ft_taste*)taste, key->bit));
        break;
      case KIND_TRIPLE:
        append_numbers(buffer, taste->tint, 3);
        break;
      case KIND_CURVE:
        append(buffer, taste->curves[get_curve_slot(key->bit)]);
        break;
    }
    append(buffer, "\n");
  }
}

/* a section header, with a blank line before it unless it is first */
static void append_section(text_buffer* buffer, const char* header) {
  if(buffer->length > 0) append(buffer, "\n");
  append(buffer, header);
}

static void append_stock(text_buffer* buffer, const ft_stock* stock) {
  append_section(buffer, "[stock]\nstock_version = ");
  append_number(buffer, stock->version);
  append(buffer, "\n");
  if(stock->has_ratio) {
    append(buffer, "range_ratio = ");
    append_numbers(buffer, stock->range_ratio, 3);
    append(buffer, "\n");
  }
  if(stock->has_bw) {
    append(buffer, "bw_weights = ");
    append_numbers(buffer, stock->bw_weights, 3);
    append(buffer, "\n");
  }
  if(stock->frame_count != 0) {
    append(buffer, "n_frames = ");
    append_number(buffer, (double)stock->frame_count);
    append(buffer, "\n");
  }
}

size_t ft_format_edit(const ft_edit* edit, ft_look_kind kind, char* text,
                      size_t capacity) {
  text_buffer buffer = {.text = text, .capacity = capacity, .length = 0};
  if(capacity > 0) text[0] = '\0';
  if(kind == FT_EDIT_FILE && edit->from[0] != '\0') {
    append(&buffer, "from = ");
    append(&buffer, edit->from);
    append(&buffer, "\n");
  }
  if(edit->stock.is_present) append_stock(&buffer, &edit->stock);
  append_section(&buffer, "[taste]\n");
  append_taste(&buffer, &edit->taste);
  if(kind == FT_EDIT_FILE) {
    for(uint32_t slot = 0; slot < edit->frame_count; slot++) {
      char header[32];
      text_buffer header_buffer = {
        .text = header, .capacity = sizeof header, .length = 0};
      append(&header_buffer, "[frame ");
      append_number(&header_buffer, edit->frames[slot].index);
      append(&header_buffer, "]\n");
      append_section(&buffer, header);
      append_taste(&buffer, &edit->frames[slot].overrides);
    }
  }
  return buffer.length;
}

/* --- edit operations --- */

static bool is_frame_number(int32_t frame) {
  return frame >= 0 && frame <= MAX_FRAME_NUMBER;
}

/* The taste of a scope. should_create adds a missing frame. Returns
 * NULL when the frame has none (or cannot be added: error filled). */
static ft_taste* get_scope(ft_edit* edit, int32_t frame, bool should_create,
                           ft_err* error) {
  if(frame == FT_EDIT_ROLL) return &edit->taste;
  if(is_frame_number(frame) == false) {
    ft_fail(error, "frame %d: not a frame number (0..9999)", (int)frame);
    return NULL;
  }
  int32_t slot =
    should_create ? get_frame_slot(edit, frame) : find_frame(edit, frame);
  if(slot < 0) {
    if(should_create) {
      ft_fail(error, "frame %d: more than %d frames", (int)frame,
              FT_MAX_FRAMES);
    }
    return NULL;
  }
  return &edit->frames[slot].overrides;
}

int32_t ft_set_edit_key(ft_edit* edit, int32_t frame, const char* key,
                        const char* value, ft_err* error) {
  if(frame != FT_EDIT_ROLL && is_frame_number(frame) == false) {
    return ft_fail(error, "frame %d: not a frame number (0..9999)", (int)frame);
  }
  /* validate before a frame slot is created */
  ft_taste check;
  memset(&check, 0, sizeof check);
  if(ft_set_taste_key(&check, key, value, error) != 0) return -1;
  ft_taste* taste = get_scope(edit, frame, true, error);
  if(taste == NULL) return -1;
  return ft_set_taste_key(taste, key, value, error);
}

static void drop_frame(ft_edit* edit, uint32_t slot) {
  for(uint32_t index = slot; index + 1 < edit->frame_count; index++) {
    edit->frames[index] = edit->frames[index + 1];
  }
  edit->frame_count--;
}

int32_t ft_clear_edit_key(ft_edit* edit, int32_t frame, const char* key,
                          ft_err* error) {
  /* every key, unless one is named */
  uint32_t bits = ~0u;
  if(key != NULL) {
    int32_t index = find_taste_key(key);
    if(index < 0) return ft_fail(error, "%.40s: unknown key", key);
    bits = TASTE_KEYS[index].bit;
  }
  if(frame != FT_EDIT_ROLL && is_frame_number(frame) == false) {
    return ft_fail(error, "frame %d: not a frame number (0..9999)", (int)frame);
  }
  ft_taste* taste = get_scope(edit, frame, false, error);
  /* a frame with nothing to clear */
  if(taste == NULL) return 0;
  taste->set_keys &= ~bits;
  if(frame != FT_EDIT_ROLL && taste->set_keys == 0) {
    drop_frame(edit, (uint32_t)find_frame(edit, frame));
  }
  return 0;
}

int32_t ft_copy_edit_frame(ft_edit* edit, int32_t source, int32_t destination,
                           ft_err* error) {
  if(is_frame_number(source) == false ||
     is_frame_number(destination) == false) {
    return ft_fail(error, "copy: frames are numbers 0..9999");
  }
  if(source == destination) return 0;
  int32_t source_slot = find_frame(edit, source);
  if(source_slot < 0) {
    /* the source has no overrides: neither will the destination */
    int32_t destination_slot = find_frame(edit, destination);
    if(destination_slot >= 0) drop_frame(edit, (uint32_t)destination_slot);
    return 0;
  }
  int32_t destination_slot = get_frame_slot(edit, destination);
  if(destination_slot < 0) {
    return ft_fail(error, "frame %d: more than %d frames", (int)destination,
                   FT_MAX_FRAMES);
  }
  /* the insert may have moved the source */
  source_slot = find_frame(edit, source);
  edit->frames[destination_slot].overrides =
    edit->frames[source_slot].overrides;
  return 0;
}

void ft_apply_look(ft_edit* edit, const ft_edit* look, const char* name) {
  size_t length = name != NULL ? strlen(name) : 0;
  if(length >= FT_LOOK_NAME_BYTES) length = FT_LOOK_NAME_BYTES - 1;
  edit->stock = look->stock;
  edit->taste = look->taste;
  memcpy(edit->from, name != NULL ? name : "", length);
  edit->from[length] = '\0';
}

/* --- resolving --- */

void ft_get_frame_taste(const ft_edit* edit, int32_t frame, ft_taste* taste,
                        uint32_t* from_frame) {
  int32_t slot = frame == FT_EDIT_ROLL ? -1 : find_frame(edit, frame);
  *taste = edit->taste;
  if(from_frame != NULL) *from_frame = 0;
  if(slot < 0) return;
  const ft_taste* overrides = &edit->frames[slot].overrides;
  for(size_t index = 0; index < TASTE_KEY_COUNT; index++) {
    uint32_t bit = TASTE_KEYS[index].bit;
    if((overrides->set_keys & bit) == 0) continue;
    switch(TASTE_KEYS[index].kind) {
      case KIND_NUMBER:
        *get_number_field(taste, bit) =
          *get_number_field((ft_taste*)overrides, bit);
        break;
      case KIND_TRIPLE:
        memcpy(taste->tint, overrides->tint, sizeof taste->tint);
        break;
      case KIND_CURVE:
        memcpy(taste->curves[get_curve_slot(bit)],
               overrides->curves[get_curve_slot(bit)],
               FT_LOOK_CURVE_TEXT_BYTES);
        break;
    }
    taste->set_keys |= bit;
    if(from_frame != NULL) *from_frame |= bit;
  }
}

/* the taste's value of a number key, or the default */
static double get_or_default(const ft_taste* taste, uint32_t bit, double value,
                             double default_value) {
  if(taste != NULL && (taste->set_keys & bit) != 0) return value;
  return default_value;
}

void ft_get_look_detail(const ft_taste* taste, ft_look_detail* detail) {
  bool has_taste = taste != NULL;
  detail->nr_sensor =
    get_or_default(taste, FT_TASTE_NR_SENSOR, has_taste ? taste->nr_sensor : 0,
                   DEFAULT_NR_SENSOR);
  detail->nr_chroma =
    get_or_default(taste, FT_TASTE_NR_CHROMA, has_taste ? taste->nr_chroma : 0,
                   DEFAULT_NR_CHROMA);
  detail->nr_grain =
    get_or_default(taste, FT_TASTE_NR_GRAIN, has_taste ? taste->nr_grain : 0,
                   DEFAULT_NR_GRAIN);
  detail->sharpen = get_or_default(
    taste, FT_TASTE_SHARPEN, has_taste ? taste->sharpen : 0, DEFAULT_SHARPEN);
}

static float clamp_to_float(double value, double low, double high) {
  if(value < low) return (float)low;
  if(value > high) return (float)high;
  return (float)value;
}

int32_t ft_resolve_recipe(const ft_stock* stock, const ft_taste* taste,
                          ft_render_mode mode, const float* base,
                          const ft_image* const* images, uint32_t image_count,
                          ft_recipe* recipe, ft_err* error) {
  ft_init_recipe(recipe, mode);
  if(stock != NULL && stock->has_bw && mode == FT_RENDER_BW) {
    for(uint32_t channel = 0; channel < 3; channel++) {
      recipe->bw_weights[channel] = (float)stock->bw_weights[channel];
    }
  }
  /* absolute fields first: the auto exposure depends on grade and paper */
  if(taste != NULL) {
    uint32_t keys = taste->set_keys;
    if((keys & FT_TASTE_GRADE) != 0) recipe->grade = (float)taste->grade;
    if((keys & FT_TASTE_PAPER_BLACK) != 0) {
      recipe->paper_black = (float)taste->paper_black;
    }
    if((keys & FT_TASTE_PAPER_WHITE) != 0) {
      recipe->paper_white = (float)taste->paper_white;
    }
    /* curves 0..3: master, r, g, b, in bit order from FT_TASTE_CURVE */
    for(uint32_t slot = 0; slot < 4; slot++) {
      if((keys & (FT_TASTE_CURVE << slot)) == 0) continue;
      ft_curve* curve =
        slot > 0 ? &recipe->channels[slot - 1] : &recipe->master;
      if(parse_curve_text(TASTE_KEYS[FIRST_CURVE_KEY + slot].name,
                          taste->curves[slot], curve, error) != 0) {
        return -1;
      }
    }
  }
  float density[3];
  if(base != NULL) {
    memcpy(density, base, sizeof density);
  } else if(ft_estimate_base_from_frames(images, image_count, mode, density,
                                         error) != 0) {
    return -1;
  }
  ft_set_recipe_base(recipe, density);
  if(ft_estimate_recipe(images, image_count, recipe, error) != 0) return -1;
  /* the stock's channel balance; green (and so the exposure) stays */
  if(stock != NULL && stock->has_ratio && mode == FT_RENDER_NEGATIVE) {
    recipe->range_density[0] = clamp_to_float(
      recipe->range_density[1] * stock->range_ratio[0], 0.05, 4.5);
    recipe->range_density[2] = clamp_to_float(
      recipe->range_density[1] * stock->range_ratio[2], 0.05, 4.5);
  }
  if(taste != NULL && (taste->set_keys & FT_TASTE_EXPOSURE) != 0) {
    recipe->exposure =
      clamp_to_float(recipe->exposure + taste->exposure, -6, 6);
  }
  /* b&w has one channel: a look's tint is ignored, not an error */
  if(taste != NULL &&
     (taste->set_keys & FT_TASTE_TINT) != 0 &&
     mode != FT_RENDER_BW) {
    for(uint32_t channel = 0; channel < 3; channel++) {
      recipe->tint[channel] = (float)taste->tint[channel];
    }
  }
  return ft_validate_recipe(recipe, error);
}

/* --- learning --- */

/* the median of values[0..count); sorts values */
static double get_median(double* values, uint32_t count) {
  /* insertion sort: the counts are small */
  for(uint32_t index = 1; index < count; index++) {
    double value = values[index];
    uint32_t position = index;
    while(position > 0 && values[position - 1] > value) {
      values[position] = values[position - 1];
      position--;
    }
    values[position] = value;
  }
  if(count % 2 == 1) return values[count / 2];
  return (values[count / 2 - 1] + values[count / 2]) / 2;
}

int32_t ft_learn_stock(const double (*ratio)[3], uint32_t frame_count,
                       const ft_stock* old, ft_stock* learned, double* spread,
                       ft_err* error) {
  if(frame_count < 1) return ft_fail(error, "learn: no frames");
  if(frame_count > FT_MAX_FRAMES) {
    return ft_fail(error, "learn: more than %d frames", FT_MAX_FRAMES);
  }
  /* red (0) and blue (2): green is always 1 */
  for(uint32_t frame = 0; frame < frame_count; frame++) {
    for(uint32_t channel = 0; channel < 3; channel += 2) {
      if(!(ratio[frame][channel] >= 0.2 && ratio[frame][channel] <= 5)) {
        return ft_fail(error, "learn: frame %u: ratio %g out of range (0.2..5)",
                       (unsigned)frame + 1, ratio[frame][channel]);
      }
    }
  }
  double values[FT_MAX_FRAMES];
  double medians[3] = {1, 1, 1};
  double largest_spread = 0;
  for(uint32_t channel = 0; channel < 3; channel += 2) {
    for(uint32_t frame = 0; frame < frame_count; frame++) {
      values[frame] = ratio[frame][channel];
    }
    medians[channel] = get_median(values, frame_count);
    /* sorted now: the spread is the last less the first */
    double channel_spread = values[frame_count - 1] - values[0];
    if(channel_spread > largest_spread) largest_spread = channel_spread;
  }
  if(old != NULL) {
    *learned = *old;
  } else {
    memset(learned, 0, sizeof *learned);
  }
  learned->is_present = true;
  learned->version = FT_STOCK_VERSION;
  if(old != NULL && old->has_ratio && old->frame_count > 0) {
    double old_count = (double)old->frame_count;
    for(uint32_t channel = 0; channel < 3; channel += 2) {
      learned->range_ratio[channel] = (old_count * old->range_ratio[channel] +
                                       frame_count * medians[channel]) /
                                      (old_count + frame_count);
    }
    learned->frame_count = old->frame_count + frame_count;
  } else {
    learned->range_ratio[0] = medians[0];
    learned->range_ratio[2] = medians[2];
    learned->frame_count = frame_count;
  }
  learned->range_ratio[1] = 1;
  learned->has_ratio = true;
  if(spread != NULL) *spread = largest_spread;
  return 0;
}

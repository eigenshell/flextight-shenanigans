/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_print_file.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ft_emulsion.h"

/* frame numbers run 0..MAX_INDEX */
#define MAX_INDEX 9999
/* the longest line a .print has */
#define MAX_LINE_BYTES 256
/* the largest rotation */
#define MAX_ROTATION 270
#define NAME_RULE "is not 1..64 of A-Z a-z 0-9 . _ - (not starting with .)"

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

/* count whole numbers in -limit..limit separated by spaces, nothing
 * else. Returns true and fills out. */
static bool parse_wholes(const char* text, uint32_t count, double limit,
                         int32_t* out) {
  for(uint32_t index = 0; index < count; index++) {
    char* end;
    double value = strtod(text, &end);
    if(end == text ||
       !(value == floor(value)) ||
       value < -limit ||
       value > limit) {
      return false;
    }
    out[index] = (int32_t)value;
    text = end;
  }
  while(is_space(*text)) text++;
  return *text == '\0';
}

void ft_init_print_file(ft_print_file* print_file, const char* emulsion) {
  memset(print_file, 0, sizeof *print_file);
  size_t length = strlen(emulsion);
  if(length < sizeof print_file->emulsion) {
    memcpy(print_file->emulsion, emulsion, length + 1);
  }
}

/* the slot of a frame, or -1 */
static int32_t find_frame(const ft_print_file* print_file, int32_t index) {
  for(uint32_t slot = 0; slot < print_file->frame_count; slot++) {
    if(print_file->frames[slot].index == index) return (int32_t)slot;
  }
  return -1;
}

/* a new section, in index order; -1 when full */
static int32_t insert_frame(ft_print_file* print_file, int32_t index) {
  if(print_file->frame_count >= FT_MAX_FRAMES) return -1;
  uint32_t slot = 0;
  while(slot < print_file->frame_count &&
        print_file->frames[slot].index < index) {
    slot++;
  }
  memmove(&print_file->frames[slot + 1], &print_file->frames[slot],
          (print_file->frame_count - slot) * sizeof print_file->frames[0]);
  memset(&print_file->frames[slot], 0, sizeof print_file->frames[slot]);
  print_file->frames[slot].index = index;
  print_file->frame_count++;
  return (int32_t)slot;
}

static bool is_rotation_valid(int32_t degrees) {
  return degrees == 0 || degrees == 90 || degrees == 180 || degrees == 270;
}

static bool is_frame_number(int32_t frame) {
  return frame >= 0 && frame <= MAX_INDEX;
}

/* A frame name: [A-Za-z0-9._-], 1..64, no leading '.' (ftwebd's file
 * names). */
static bool is_name_valid(const char* name) {
  size_t length = strlen(name);
  if(length < 1 || length >= FT_PRINT_NAME_BYTES || name[0] == '.') {
    return false;
  }
  for(size_t index = 0; index < length; index++) {
    char character = name[index];
    bool is_allowed = (character >= 'a' && character <= 'z') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') ||
                      character == '.' ||
                      character == '_' ||
                      character == '-';
    if(is_allowed == false) return false;
  }
  return true;
}

static bool are_clicks_auto(ft_cmyd clicks) {
  return clicks.cyan == 0 &&
         clicks.magenta == 0 &&
         clicks.yellow == 0 &&
         clicks.density == 0;
}

/* A frame's section goes when it says nothing. */
static void drop_if_empty(ft_print_file* print_file, int32_t slot) {
  if(slot < 0) return;
  const ft_print_file_frame* frame = &print_file->frames[slot];
  if(are_clicks_auto(frame->clicks) == false ||
     frame->rotation_degrees != 0 ||
     frame->name[0] != '\0') {
    return;
  }
  memmove(&print_file->frames[slot], &print_file->frames[slot + 1],
          (print_file->frame_count - (uint32_t)slot - 1) *
            sizeof print_file->frames[0]);
  print_file->frame_count--;
}

/* An unknown emulsion: the error lists the known ids. */
static int32_t fail_unknown_emulsion(ft_err* error, uint32_t line_number,
                                     const char* value) {
  char ids[160];
  size_t length = 0;
  ids[0] = '\0';
  const ft_emulsion* emulsion;
  for(uint32_t index = 0; (emulsion = ft_get_emulsion_at(index)) != NULL;
      index++) {
    size_t id_length = strlen(emulsion->id);
    if(length + id_length + 2 >= sizeof ids) break;
    if(length > 0) {
      ids[length] = ' ';
      length++;
    }
    memcpy(ids + length, emulsion->id, id_length + 1);
    length += id_length;
  }
  return ft_fail(error, "line %u: emulsion: '%.40s' is not an emulsion (%s)",
                 (unsigned)line_number, value, ids);
}

/* The parser's state across lines. */
typedef struct parser {
  ft_print_file* print_file;
  const ft_emulsion* emulsion;
  /* the frame section being read; -1 = before any */
  int32_t slot;
  uint32_t line_number;
  bool has_clicks;
  bool has_rotation;
  bool has_name;
} parser;

/* Start "[frame N]" (brackets cut off). Returns 0, or -1 with error
 * filled. */
static int32_t start_frame(parser* state, char* name, ft_err* error) {
  unsigned line_number = state->line_number;
  bool is_frame =
    strlen(name) >= 6 && memcmp(name, "frame", 5) == 0 && is_space(name[5]);
  if(is_frame == false) {
    return ft_fail(error, "line %u: unknown section [%.40s]", line_number,
                   name);
  }
  int32_t index;
  if(parse_wholes(name + 5, 1, MAX_INDEX, &index) == false || index < 0) {
    return ft_fail(error, "line %u: '[%.40s]': the frame is a number (0..%d)",
                   line_number, name, MAX_INDEX);
  }
  if(find_frame(state->print_file, index) >= 0) {
    return ft_fail(error, "line %u: [frame %d] twice", line_number, (int)index);
  }
  state->slot = insert_frame(state->print_file, index);
  if(state->slot < 0) {
    return ft_fail(error, "line %u: more than %d frames", line_number,
                   FT_MAX_FRAMES);
  }
  state->print_file->frames[state->slot].source_line = state->line_number;
  state->has_clicks = false;
  state->has_rotation = false;
  state->has_name = false;
  return 0;
}

/* Read one key = value line. Returns 0, or -1 with error filled. */
static int32_t read_key_value(parser* state, const char* key, const char* value,
                              ft_err* error) {
  unsigned line_number = state->line_number;
  if(state->slot < 0) {
    if(strcmp(key, "emulsion") != 0) {
      return ft_fail(error, "line %u: unknown key '%.40s'", line_number, key);
    }
    if(state->emulsion != NULL) {
      return ft_fail(error, "line %u: emulsion: set twice", line_number);
    }
    state->emulsion = ft_find_emulsion(value);
    if(state->emulsion == NULL) {
      return fail_unknown_emulsion(error, state->line_number, value);
    }
    memcpy(state->print_file->emulsion, state->emulsion->id,
           strlen(state->emulsion->id) + 1);
    return 0;
  }
  ft_print_file_frame* frame = &state->print_file->frames[state->slot];
  if(strcmp(key, "name") == 0) {
    if(state->has_name) {
      return ft_fail(error, "line %u: name: set twice", line_number);
    }
    if(is_name_valid(value) == false) {
      return ft_fail(error, "line %u: name: '%.40s' " NAME_RULE, line_number,
                     value);
    }
    memcpy(frame->name, value, strlen(value) + 1);
    state->has_name = true;
    return 0;
  }
  if(strcmp(key, "rotate") == 0) {
    int32_t degrees;
    if(state->has_rotation) {
      return ft_fail(error, "line %u: rotate: set twice", line_number);
    }
    if(parse_wholes(value, 1, MAX_ROTATION, &degrees) == false ||
       is_rotation_valid(degrees) == false) {
      return ft_fail(error, "line %u: rotate: '%.40s' is not 0, 90, 180 or 270",
                     line_number, value);
    }
    frame->rotation_degrees = degrees;
    state->has_rotation = true;
    return 0;
  }
  if(strcmp(key, "cmyd") != 0) {
    return ft_fail(error, "line %u: unknown key '%.40s' in [frame %d]",
                   line_number, key, (int)frame->index);
  }
  if(state->has_clicks) {
    return ft_fail(error, "line %u: cmyd: set twice", line_number);
  }
  int32_t values[4];
  if(parse_wholes(value, 4, FT_PRINT_CLICKS_MAX, values) == false) {
    return ft_fail(error,
                   "line %u: cmyd: '%.40s' is not four whole numbers "
                   "(c m y d, -%d..%d)",
                   line_number, value, FT_PRINT_CLICKS_MAX,
                   FT_PRINT_CLICKS_MAX);
  }
  frame->clicks.cyan = values[0];
  frame->clicks.magenta = values[1];
  frame->clicks.yellow = values[2];
  frame->clicks.density = values[3];
  state->has_clicks = true;
  return 0;
}

int32_t ft_parse_print_file(const char* text, size_t text_length,
                            ft_print_file* print_file, ft_err* error) {
  parser state;
  memset(&state, 0, sizeof state);
  state.print_file = print_file;
  state.slot = -1;
  memset(print_file, 0, sizeof *print_file);
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
      if(content[content_length - 1] != ']') {
        return ft_fail(error, "line %u: '%.40s' is not a [section]",
                       (unsigned)state.line_number, content);
      }
      content[content_length - 1] = '\0';
      if(start_frame(&state, trim(content + 1), error) != 0) return -1;
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
  if(state.emulsion == NULL) {
    return ft_fail(error, "emulsion: missing (emulsion = <id>)");
  }
  for(uint32_t slot = 0; slot < print_file->frame_count; slot++) {
    ft_err inner_error;
    if(ft_check_print_clicks(state.emulsion, print_file->frames[slot].clicks,
                             &inner_error) != 0) {
      return ft_fail(error, "line %u: %.180s",
                     (unsigned)print_file->frames[slot].source_line,
                     inner_error.message);
    }
  }
  return 0;
}

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

/* a whole number, written by hand (no snprintf in pure code) */
static void append_integer(text_buffer* buffer, int32_t value) {
  char digits[16];
  char out[16];
  uint32_t digit_count = 0;
  uint32_t length = 0;
  /* the magnitude as unsigned: safe even for the most negative value */
  uint32_t magnitude = value < 0 ? 0u - (uint32_t)value : (uint32_t)value;
  if(value < 0) {
    out[length] = '-';
    length++;
  }
  /* the digits come out last first: reverse them */
  do {
    digits[digit_count] = (char)('0' + magnitude % 10);
    digit_count++;
    magnitude /= 10;
  } while(magnitude != 0);
  while(digit_count > 0) {
    digit_count--;
    out[length] = digits[digit_count];
    length++;
  }
  out[length] = '\0';
  append(buffer, out);
}

size_t ft_format_print_file(const ft_print_file* print_file, char* text,
                            size_t capacity) {
  text_buffer buffer = {.text = text, .capacity = capacity, .length = 0};
  if(capacity > 0) text[0] = '\0';
  append(&buffer, "emulsion = ");
  append(&buffer, print_file->emulsion);
  append(&buffer, "\n");
  for(uint32_t slot = 0; slot < print_file->frame_count; slot++) {
    const ft_print_file_frame* frame = &print_file->frames[slot];
    const ft_cmyd* clicks = &frame->clicks;
    append(&buffer, "\n[frame ");
    append_integer(&buffer, frame->index);
    append(&buffer, "]\n");
    if(frame->name[0] != '\0') {
      append(&buffer, "name = ");
      append(&buffer, frame->name);
      append(&buffer, "\n");
    }
    /* cmyd = 0 0 0 0 only when nothing else is in the section */
    bool is_alone = frame->rotation_degrees == 0 && frame->name[0] == '\0';
    if(are_clicks_auto(*clicks) == false || is_alone) {
      append(&buffer, "cmyd = ");
      append_integer(&buffer, clicks->cyan);
      append(&buffer, " ");
      append_integer(&buffer, clicks->magenta);
      append(&buffer, " ");
      append_integer(&buffer, clicks->yellow);
      append(&buffer, " ");
      append_integer(&buffer, clicks->density);
      append(&buffer, "\n");
    }
    if(frame->rotation_degrees != 0) {
      append(&buffer, "rotate = ");
      append_integer(&buffer, frame->rotation_degrees);
      append(&buffer, "\n");
    }
  }
  return buffer.length;
}

ft_cmyd ft_get_print_clicks(const ft_print_file* print_file, int32_t frame) {
  static const ft_cmyd NONE = {0, 0, 0, 0};
  int32_t slot = find_frame(print_file, frame);
  return slot < 0 ? NONE : print_file->frames[slot].clicks;
}

/* The frame's slot, created when missing (unless should_create is
 * false). Returns -1 when there is none, or with error filled when the
 * file is full. */
static int32_t get_slot(ft_print_file* print_file, int32_t frame,
                        bool should_create, int32_t* slot, ft_err* error) {
  *slot = find_frame(print_file, frame);
  if(*slot >= 0 || should_create == false) return 0;
  *slot = insert_frame(print_file, frame);
  if(*slot < 0) return ft_fail(error, "more than %d frames", FT_MAX_FRAMES);
  return 0;
}

int32_t ft_set_print_clicks(ft_print_file* print_file, int32_t frame,
                            ft_cmyd clicks, ft_err* error) {
  const ft_emulsion* emulsion = ft_find_emulsion(print_file->emulsion);
  if(emulsion == NULL) {
    return ft_fail(error, "emulsion: '%.40s' is not an emulsion",
                   print_file->emulsion);
  }
  if(is_frame_number(frame) == false) {
    return ft_fail(error, "frame %d (0..%d)", (int)frame, MAX_INDEX);
  }
  if(ft_check_print_clicks(emulsion, clicks, error) != 0) return -1;
  int32_t slot;
  /* auto clicks on a frame without a section: nothing to store */
  if(get_slot(print_file, frame, are_clicks_auto(clicks) == false, &slot,
              error) != 0) {
    return -1;
  }
  if(slot < 0) return 0;
  print_file->frames[slot].clicks = clicks;
  drop_if_empty(print_file, slot);
  return 0;
}

int32_t ft_get_print_rotation(const ft_print_file* print_file, int32_t frame) {
  int32_t slot = find_frame(print_file, frame);
  return slot < 0 ? 0 : print_file->frames[slot].rotation_degrees;
}

int32_t ft_set_print_rotation(ft_print_file* print_file, int32_t frame,
                              int32_t degrees, ft_err* error) {
  if(is_frame_number(frame) == false) {
    return ft_fail(error, "frame %d (0..%d)", (int)frame, MAX_INDEX);
  }
  if(is_rotation_valid(degrees) == false) {
    return ft_fail(error, "rotate: %d is not 0, 90, 180 or 270", (int)degrees);
  }
  int32_t slot;
  if(get_slot(print_file, frame, degrees != 0, &slot, error) != 0) return -1;
  if(slot < 0) return 0;
  print_file->frames[slot].rotation_degrees = degrees;
  drop_if_empty(print_file, slot);
  return 0;
}

const char* ft_get_print_name(const ft_print_file* print_file, int32_t frame) {
  int32_t slot = find_frame(print_file, frame);
  return slot < 0 ? "" : print_file->frames[slot].name;
}

int32_t ft_set_print_name(ft_print_file* print_file, int32_t frame,
                          const char* name, ft_err* error) {
  if(is_frame_number(frame) == false) {
    return ft_fail(error, "frame %d (0..%d)", (int)frame, MAX_INDEX);
  }
  if(name[0] != '\0' && is_name_valid(name) == false) {
    return ft_fail(error, "name: '%.40s' " NAME_RULE, name);
  }
  int32_t slot;
  if(get_slot(print_file, frame, name[0] != '\0', &slot, error) != 0) {
    return -1;
  }
  if(slot < 0) return 0;
  memcpy(print_file->frames[slot].name, name, strlen(name) + 1);
  drop_if_empty(print_file, slot);
  return 0;
}

uint32_t ft_count_unknown_print_frames(const ft_print_file* print_file,
                                       const int32_t* job_frames,
                                       uint32_t count, ft_err* warning) {
  uint32_t unknown_count = 0;
  int32_t first = -1;
  for(uint32_t slot = 0; slot < print_file->frame_count; slot++) {
    int32_t index = print_file->frames[slot].index;
    bool is_in_job = false;
    for(uint32_t job_index = 0; job_index < count; job_index++) {
      if(job_frames[job_index] == index) is_in_job = true;
    }
    if(is_in_job) continue;
    if(first < 0) first = index;
    unknown_count++;
  }
  if(unknown_count == 1) {
    ft_fail(warning, "[frame %d] is not in the job: ignored", (int)first);
  } else if(unknown_count > 1) {
    ft_fail(warning, "[frame %d] and %u more are not in the job: ignored",
            (int)first, (unsigned)unknown_count - 1);
  }
  return unknown_count;
}

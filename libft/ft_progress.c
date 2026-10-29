/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_progress.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* the words on the line, in the order of their enums */
static const char* const TYPE_WORDS[] = {"begin", "unit",  "lines", "file",
                                         "end",   "focus", "sharp", "focused"};
static const char* const KIND_WORDS[] = {"preview", "window", "frame", "pass",
                                         "upload"};
static const char* const STATUS_WORDS[] = {"ok", "stopped", "failed",
                                           "refused"};
static const char* const HOW_WORDS[] = {"measured", "checked", "carried",
                                        "none"};
#define WORD_COUNT(words) (sizeof words / sizeof words[0])

/* the largest sharpness the line can carry */
#define SHARPNESS_LIMIT 1e9

static bool has_no_line_break(const char* text, size_t length) {
  return memchr(text, '\n', length) == NULL &&
         memchr(text, '\r', length) == NULL;
}

size_t ft_format_progress(const ft_progress* progress, char* line,
                          size_t line_capacity) {
  const ft_progress* p = progress;
  /* snprintf's answer; stays -1 when a field is out of range */
  int written = -1;
  if(line_capacity > 0) line[0] = '\0';

  /* the casts to unsigned also catch enum values below 0 */
  bool is_kind_valid = (unsigned)p->kind <= FT_PROGRESS_UPLOAD;
  switch(p->type) {
    case FT_PROGRESS_BEGIN:
      if(is_kind_valid && p->number >= 0) {
        written =
          snprintf(line, line_capacity, "progress begin %s %" PRId32 "\n",
                   KIND_WORDS[p->kind], p->number);
      }
      break;
    case FT_PROGRESS_UNIT:
      if(is_kind_valid && p->number >= 1 && p->label >= 0) {
        written = snprintf(line, line_capacity,
                           "progress unit %s %" PRId32 " %" PRId32 "\n",
                           KIND_WORDS[p->kind], p->number, p->label);
      }
      break;
    case FT_PROGRESS_LINES:
      if(p->step >= 0 && p->step <= p->total) {
        written = snprintf(line, line_capacity,
                           "progress lines %" PRId64 "/%" PRId64 "\n", p->step,
                           p->total);
      }
      break;
    case FT_PROGRESS_FILE: {
      /* the path's length; sizeof p->path when it has no NUL */
      size_t path_length = 0;
      while(path_length < sizeof p->path && p->path[path_length] != '\0') {
        path_length++;
      }
      if(path_length > 0 &&
         path_length < sizeof p->path &&
         has_no_line_break(p->path, path_length)) {
        written = snprintf(line, line_capacity, "progress file %s\n", p->path);
      }
      break;
    }
    case FT_PROGRESS_END:
      if((unsigned)p->status <= FT_PROGRESS_REFUSED) {
        written = snprintf(line, line_capacity, "progress end %s\n",
                           STATUS_WORDS[p->status]);
      }
      break;
    case FT_PROGRESS_FOCUS:
      if(p->step >= 1 && p->label > 0) {
        written = snprintf(line, line_capacity,
                           "progress focus %" PRId64 " frame %" PRId32
                           " at %+" PRId64 "\n",
                           p->step, p->label, p->offset);
      }
      if(p->step >= 1 && p->label == 0) {
        written = snprintf(line, line_capacity, "progress focus %" PRId64 "\n",
                           p->step);
      }
      break;
    case FT_PROGRESS_SHARP:
      if(p->sharpness >= 0 && p->sharpness < SHARPNESS_LIMIT) {
        written =
          snprintf(line, line_capacity, "progress sharp %+" PRId64 " %.4f\n",
                   p->offset, p->sharpness);
      }
      break;
    case FT_PROGRESS_FOCUSED:
      if((unsigned)p->how <= FT_PROGRESS_NONE) {
        written =
          snprintf(line, line_capacity, "progress focused %+" PRId64 " %s\n",
                   p->offset, HOW_WORDS[p->how]);
      }
      break;
  }
  if(written < 0) {
    if(line_capacity > 0) line[0] = '\0';
    return FT_PROGRESS_BAD;
  }
  /* safe: written is not negative here */
  return (size_t)written;
}

/* Read the word at *cursor, up to a space or end, and look it up in
 * words. On a match, *cursor moves past the word and one space.
 * Returns the word's index, or -1 when it is not in words or a lone
 * space ends the line. */
static int32_t read_word(const char** cursor, const char* end,
                         const char* const* words, size_t word_count) {
  const char* word_start = *cursor;
  const char* word_end = word_start;
  while(word_end < end && *word_end != ' ') word_end++;
  /* safe: word_end never moves before word_start */
  size_t word_length = (size_t)(word_end - word_start);

  for(size_t index = 0; index < word_count; index++) {
    bool is_match = word_length == strlen(words[index]) &&
                    memcmp(word_start, words[index], word_length) == 0;
    if(is_match) {
      bool has_space = word_end < end;
      *cursor = has_space ? word_end + 1 : word_end;
      /* a space as the line's last character is not allowed */
      if(has_space && word_end + 1 == end) return -1;
      return (int32_t)index;
    }
  }
  return -1;
}

/* Read decimal digits at *cursor (at least one), up to stop or end;
 * *cursor moves past stop. Returns the number, or -1 when there are no
 * digits, another character follows, or the number is too big. */
static int64_t read_digits(const char** cursor, const char* end, char stop) {
  const char* digits_start = *cursor;
  int64_t value = 0;
  while(*cursor < end && **cursor >= '0' && **cursor <= '9') {
    if(value > (INT64_MAX - 9) / 10) return -1;
    value = value * 10 + (**cursor - '0');
    (*cursor)++;
  }
  if(*cursor == digits_start) return -1;
  if(*cursor < end && **cursor != stop) return -1;
  if(*cursor < end) (*cursor)++;
  return value;
}

/* Read an optional sign ('+' or '-') and digits up to stop or end.
 * Returns true and fills *value, or false when the digits are bad. */
static bool read_signed(const char** cursor, const char* end, char stop,
                        int64_t* value) {
  bool is_negative = false;
  if(*cursor < end && (**cursor == '+' || **cursor == '-')) {
    is_negative = **cursor == '-';
    (*cursor)++;
  }
  int64_t magnitude = read_digits(cursor, end, stop);
  if(magnitude < 0) return false;
  *value = is_negative ? -magnitude : magnitude;
  return true;
}

/* Read digits '.' digits up to end: a sharpness, no sign, no exponent.
 * Returns true and fills *value, or false when it is not well formed. */
static bool read_decimal(const char* cursor, const char* end, double* value) {
  const char* digits_start = cursor;
  double number = 0;
  while(cursor < end && *cursor >= '0' && *cursor <= '9') {
    number = number * 10 + (*cursor - '0');
    cursor++;
  }
  if(cursor == digits_start || cursor == end || *cursor != '.') return false;
  cursor++;
  if(cursor == end) return false;
  /* the place value of the next fraction digit: 0.1, 0.01, ... */
  double place = 1;
  while(cursor < end && *cursor >= '0' && *cursor <= '9') {
    place /= 10;
    number += (*cursor - '0') * place;
    cursor++;
  }
  if(cursor != end) return false;
  *value = number;
  return true;
}

int32_t ft_parse_progress(const char* line, size_t line_length,
                          ft_progress* progress) {
  static const char PREFIX[] = "progress ";
  /* without the NUL */
  const size_t prefix_length = sizeof PREFIX - 1;
  ft_progress* p = progress;
  const char* cursor = line;
  const char* end = line + line_length;

  if(line_length > 0 && end[-1] == '\n') end--;
  if(end > line && end[-1] == '\r') end--;
  if((size_t)(end - cursor) < prefix_length) return -1;
  if(memcmp(cursor, PREFIX, prefix_length) != 0) return -1;
  cursor += prefix_length;
  memset(p, 0, sizeof *p);

  int32_t type = read_word(&cursor, end, TYPE_WORDS, WORD_COUNT(TYPE_WORDS));
  if(type < 0 || cursor >= end) return -1;
  p->type = (ft_progress_type)type;

  switch(p->type) {
    case FT_PROGRESS_BEGIN:
    case FT_PROGRESS_UNIT: {
      int32_t kind =
        read_word(&cursor, end, KIND_WORDS, WORD_COUNT(KIND_WORDS));
      if(kind < 0) return -1;
      p->kind = (ft_progress_kind)kind;
      int64_t number = read_digits(&cursor, end, ' ');
      if(number < 0 || number > INT32_MAX) return -1;
      p->number = (int32_t)number;
      if(p->type == FT_PROGRESS_BEGIN) {
        if(cursor == end && end[-1] != ' ') return 0;
        return -1;
      }
      int64_t label = read_digits(&cursor, end, ' ');
      if(number < 1 || label < 0 || label > INT32_MAX) return -1;
      if(cursor != end || end[-1] == ' ') return -1;
      p->label = (int32_t)label;
      return 0;
    }
    case FT_PROGRESS_LINES: {
      int64_t step = read_digits(&cursor, end, '/');
      if(step < 0 || cursor >= end || cursor[-1] != '/') return -1;
      int64_t total = read_digits(&cursor, end, ' ');
      if(total < 0 || cursor != end || step > total) return -1;
      p->step = step;
      p->total = total;
      return 0;
    }
    case FT_PROGRESS_FILE: {
      /* safe: cursor never moves past end */
      size_t path_length = (size_t)(end - cursor);
      if(path_length >= sizeof p->path) return -1;
      if(memchr(cursor, '\r', path_length) != NULL) return -1;
      if(memchr(cursor, '\0', path_length) != NULL) return -1;
      memcpy(p->path, cursor, path_length);
      return 0;
    }
    case FT_PROGRESS_END: {
      int32_t status =
        read_word(&cursor, end, STATUS_WORDS, WORD_COUNT(STATUS_WORDS));
      if(status < 0 || cursor != end) return -1;
      p->status = (ft_progress_status)status;
      return 0;
    }
    case FT_PROGRESS_FOCUS: {
      int64_t step = read_digits(&cursor, end, ' ');
      if(step < 1) return -1;
      p->step = step;
      if(cursor == end) return end[-1] == ' ' ? -1 : 0;
      if(end - cursor < 6 || memcmp(cursor, "frame ", 6) != 0) return -1;
      cursor += 6;
      int64_t label = read_digits(&cursor, end, ' ');
      if(label < 1 || label > INT32_MAX) return -1;
      if(end - cursor < 4 || memcmp(cursor, "at ", 3) != 0) return -1;
      cursor += 3;
      p->label = (int32_t)label;
      if(read_signed(&cursor, end, ' ', &p->offset) == false) return -1;
      return cursor == end ? 0 : -1;
    }
    case FT_PROGRESS_SHARP:
      if(read_signed(&cursor, end, ' ', &p->offset) == false) return -1;
      if(cursor >= end || cursor[-1] != ' ') return -1;
      return read_decimal(cursor, end, &p->sharpness) ? 0 : -1;
    case FT_PROGRESS_FOCUSED: {
      if(read_signed(&cursor, end, ' ', &p->offset) == false) return -1;
      if(cursor >= end || cursor[-1] != ' ') return -1;
      int32_t how = read_word(&cursor, end, HOW_WORDS, WORD_COUNT(HOW_WORDS));
      if(how < 0 || cursor != end) return -1;
      p->how = (ft_progress_how)how;
      return 0;
    }
  }
  return -1;
}

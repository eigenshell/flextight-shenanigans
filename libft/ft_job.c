/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_job.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_holder.h"
#include "ft_plan.h"

/* the words in the file, in the order of their enums */
static const char* const FRAME_TYPE_WORDS[] = {"full", "half", "panorama",
                                               "partial"};
static const char* const DETAIL_WORDS[] = {"none", "neg", "pos"};
static const char* const DETAIL_FORMAT_WORDS[] = {"dng", "tiff"};
static const char* const BW_WORDS[] = {"off", "mono", "rgb"};
#define WORD_COUNT(words) (sizeof words / sizeof words[0])

/* the longest line the parser reads */
#define MAX_LINE_BYTES 256
/* the highest frame number a [frame N] header may have */
#define MAX_FRAME_NUMBER 1000000
/* the share target range, pixels on the long side */
#define MIN_SHARE_PIXELS 100
#define MAX_SHARE_PIXELS 20000
/* the largest film base density */
#define MAX_FILM_BASE_DENSITY 4.5
/* the largest frame tilt, degrees */
#define MAX_TILT_DEGREES 45
/* the largest bin */
#define MAX_BIN 12
/* room for "line 123 (frame 45)" */
#define WHERE_BYTES 48

/* The keys of the [strip] section, in the order of STRIP_KEYS. */
typedef enum strip_key {
  STRIP_HOLDER,
  STRIP_FILM,
  STRIP_FOCUS,
  STRIP_FOCUS_OFFSET,
  STRIP_FILM_BASE,
  STRIP_DETAIL,
  STRIP_DETAIL_FORMAT,
  STRIP_DETAIL_BW,
  STRIP_KEEP_RAW,
  STRIP_RING_MEGABYTES,
  STRIP_SHARE,
  STRIP_HALF_FRAMES
} strip_key;
static const char* const STRIP_KEYS[] = {
  "holder",    "film",    "focus",      "focus_offset",
  "film_base", "detail",  "detail_fmt", "detail_bw",
  "keep_raw",  "ring_mb", "share",      "hf_half_res"};

/* The keys of a [frame N] section, in the order of FRAME_KEYS. */
typedef enum frame_key {
  FRAME_TYPE,
  FRAME_INCLUDE,
  FRAME_PPI,
  FRAME_WINDOW,
  FRAME_TILT,
  FRAME_SPROCKETS,
  FRAME_STRAIGHTEN,
  FRAME_CONFIDENCE,
  FRAME_FOCUS_OFFSET,
  FRAME_DONE,
  FRAME_BIN,
  FRAME_PASS,
  FRAME_GROUP,
  FRAME_SHARE_PIXELS
} frame_key;
static const char* const FRAME_KEYS[] = {
  "type",      "include",    "ppi",        "window",       "tilt",
  "sprockets", "straighten", "confidence", "focus_offset", "done",
  "bin",       "pass",       "group",      "share_px"};

static const char* yes_or_no(bool value) { return value ? "yes" : "no"; }

int32_t ft_get_frame_bin(const ft_job_frame* frame) {
  if(frame->bin != 0) return frame->bin;
  return ft_get_bin_for_ppi(frame->ppi);
}

double ft_get_frame_share_pixels(const ft_job* job, const ft_job_frame* frame) {
  if(frame->share_pixels > 0) return frame->share_pixels;
  return job->share_pixels;
}

uint32_t ft_count_pass_frames_to_do(const ft_job* job, int32_t pass) {
  uint32_t count = 0;
  for(uint32_t index = 0; index < job->frame_count; index++) {
    const ft_job_frame* frame = &job->frames[index];
    if(frame->is_included && frame->is_done == false && frame->pass == pass) {
      count++;
    }
  }
  return count;
}

/* Text going into a caller's buffer. length counts every byte asked
 * for; the buffer keeps what fits and stays NUL-terminated. */
typedef struct text_buffer {
  /* the caller's buffer; may be NULL when capacity is 0 */
  char* text;
  /* bytes available in text */
  size_t capacity;
  /* bytes the whole text needs so far */
  size_t length;
} text_buffer;

/* printf onto the end of the buffer */
#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
static void append(text_buffer* buffer, const char* format, ...) {
  bool has_room = buffer->length < buffer->capacity;
  char* end = has_room ? buffer->text + buffer->length : NULL;
  size_t room = has_room ? buffer->capacity - buffer->length : 0;
  va_list values;
  va_start(values, format);
  int written = vsnprintf(end, room, format, values);
  va_end(values);
  /* safe: only added when not negative */
  if(written > 0) buffer->length += (size_t)written;
}

/* A comment listing the frame's bin options:
 * "# bin options at P ppi: b: time, light, Dmax | ..." */
static void append_bin_options(text_buffer* out, const ft_job_frame* frame) {
  uint32_t bins[6];
  uint32_t bin_count = ft_find_bins_for_ppi(frame->ppi, bins);
  if(bin_count < 2) return;
  append(out, "# bin options at %g ppi (time, light, Dmax~):", frame->ppi);
  for(uint32_t index = 0; index < bin_count; index++) {
    double light =
      ft_estimate_relative_light(&ft_lens_default, frame->ppi * bins[index]);
    double seconds =
      ft_estimate_scan_seconds(frame->ppi, bins[index], frame->length_mm);
    append(out, "%s %u: %.0f s %.1fx %.2f", index > 0 ? " |" : "",
           (unsigned)bins[index], seconds, light,
           ft_estimate_dmax_for_light(light));
  }
  append(out, "\n");
}

/* every enum inside its word list; the casts to unsigned also catch
 * values below 0 */
static bool is_job_in_range(const ft_job* job) {
  for(uint32_t index = 0; index < job->frame_count; index++) {
    if((unsigned)job->frames[index].type >= WORD_COUNT(FRAME_TYPE_WORDS)) {
      return false;
    }
  }
  return (unsigned)job->detail < WORD_COUNT(DETAIL_WORDS) &&
         (unsigned)job->detail_format < WORD_COUNT(DETAIL_FORMAT_WORDS) &&
         (unsigned)job->detail_bw < WORD_COUNT(BW_WORDS);
}

static void append_frame(text_buffer* out, const ft_job* job,
                         const ft_job_frame* frame) {
  append(out,
         "\n[frame %u]\ntype = %s\ninclude = %s\nppi = %g\n"
         "bin = %d   # 0 = fastest; smaller = more light, slower\n",
         (unsigned)frame->index, FRAME_TYPE_WORDS[frame->type],
         yes_or_no(frame->is_included), frame->ppi, (int)frame->bin);
  append_bin_options(out, frame);
  append(out,
         "window = %.3f %.3f %.3f %.3f   # y0 x0 length width, mm\n"
         "tilt = %.2f\nsprockets = %s\nstraighten = %s\n"
         "confidence = %s\nfocus_offset = %" PRId64 "\ndone = %s\n",
         frame->along_start_mm, frame->across_start_mm, frame->length_mm,
         frame->width_mm, frame->tilt_degrees, yes_or_no(frame->has_sprockets),
         yes_or_no(frame->should_straighten),
         frame->is_confidence_low ? "low" : "high", frame->focus_offset_steps,
         yes_or_no(frame->is_done));
  if(job->detail != FT_DETAIL_NONE) {
    append(out, "pass = %d\n", (int)frame->pass);
  }
  if(job->share_pixels > 0) {
    append(out, "group = %d   # window: frames of one group scan together\n",
           (int)frame->group);
  }
  if(frame->share_pixels > 0) {
    append(out, "share_px = %g   # this frame's target, px on its long side\n",
           frame->share_pixels);
  }
}

size_t ft_format_job(const ft_job* job, char* text, size_t text_capacity) {
  text_buffer buffer = {.text = text, .capacity = text_capacity, .length = 0};
  text_buffer* out = &buffer;
  if(text_capacity > 0) text[0] = '\0';
  if(is_job_in_range(job) == false) return FT_JOB_BAD;

  append(out,
         "[strip]\nholder = %s\nfilm = %s\nfocus = %s\n"
         "focus_offset = %" PRId64 "\n",
         job->holder, job->film == FT_FILM_SLIDE ? "slide" : "negative",
         job->focus_mode == FT_FOCUS_PER_FRAME ? "per-frame" : "shifted",
         job->focus_offset_steps);
  if(job->has_film_base) {
    append(out, "film_base = %.4f %.4f %.4f   # D r g b: unexposed film\n",
           job->film_base_density[0], job->film_base_density[1],
           job->film_base_density[2]);
  }
  if(job->detail != FT_DETAIL_NONE) {
    append(out,
           "detail = %s   # neg: digital negative, pos: archive "
           "positive\ndetail_fmt = %s\ndetail_bw = %s\n"
           "keep_raw = %s\nring_mb = %d\n",
           DETAIL_WORDS[job->detail], DETAIL_FORMAT_WORDS[job->detail_format],
           BW_WORDS[job->detail_bw], yes_or_no(job->keep_raw),
           (int)job->ring_megabytes);
  }
  if(job->share_pixels > 0) {
    append(out,
           "share = %g   # scan to share: px on each frame's long "
           "side\nhf_half_res = %s\n",
           job->share_pixels, yes_or_no(job->half_frames_at_full_frame_ppi));
  }
  for(uint32_t index = 0; index < job->frame_count; index++) {
    append_frame(out, job, &job->frames[index]);
  }
  return buffer.length;
}

/* Cut white space from both ends of text, in place. */
static char* trim(char* text) {
  while(isspace((unsigned char)*text)) text++;
  char* end = text + strlen(text);
  while(end > text && isspace((unsigned char)end[-1])) {
    end--;
    *end = '\0';
  }
  return text;
}

/* The parsers below return true when value was well formed and fill
 * *out; false leaves *out as it may be. */

static bool parse_yes_no(const char* value, bool* out) {
  if(strcmp(value, "yes") == 0) {
    *out = true;
    return true;
  }
  if(strcmp(value, "no") == 0) {
    *out = false;
    return true;
  }
  return false;
}

/* a finite decimal number, nothing after it but white space */
static bool parse_number(const char* value, double* out) {
  char* end;
  *out = strtod(value, &end);
  return end != value && *trim(end) == '\0' && isfinite(*out);
}

/* a whole number, nothing after it but white space */
static bool parse_integer(const char* value, int64_t* out) {
  char* end;
  errno = 0;
  /* strtoll: long long is at least 64 bits on every platform */
  long long number = strtoll(value, &end, 10);
  *out = number;
  return end != value && *trim(end) == '\0' && errno != ERANGE;
}

/* a whole number within minimum..maximum */
static bool parse_integer_in(const char* value, int64_t minimum,
                             int64_t maximum, int32_t* out) {
  int64_t number;
  if(parse_integer(value, &number) == false) return false;
  if(number < minimum || number > maximum) return false;
  /* safe: the range checked above fits 32 bits */
  *out = (int32_t)number;
  return true;
}

/* value as an index into words */
static bool parse_word(const char* const* words, size_t word_count,
                       const char* value, uint32_t* out) {
  for(size_t index = 0; index < word_count; index++) {
    if(strcmp(words[index], value) == 0) {
      *out = (uint32_t)index;
      return true;
    }
  }
  return false;
}

/* key as an index into keys; -1 when it is none of them */
static int32_t find_key(const char* const* keys, size_t key_count,
                        const char* key) {
  for(size_t index = 0; index < key_count; index++) {
    if(strcmp(keys[index], key) == 0) return (int32_t)index;
  }
  return -1;
}

static bool is_share_in_range(double share_pixels) {
  return share_pixels >= MIN_SHARE_PIXELS && share_pixels <= MAX_SHARE_PIXELS;
}

/* Read one [strip] value. Returns true when it was well formed. */
static bool parse_strip_value(ft_job* job, strip_key key, const char* value) {
  uint32_t word;
  switch(key) {
    case STRIP_HOLDER:
      if(value[0] == '\0' || strlen(value) >= sizeof job->holder) return false;
      strcpy(job->holder, value);
      return true;
    case STRIP_FILM:
      if(strcmp(value, "negative") == 0) {
        job->film = FT_FILM_NEGATIVE;
        return true;
      }
      if(strcmp(value, "slide") == 0) {
        job->film = FT_FILM_SLIDE;
        return true;
      }
      return false;
    case STRIP_FOCUS:
      if(strcmp(value, "shifted") == 0) {
        job->focus_mode = FT_FOCUS_SHIFTED;
        return true;
      }
      if(strcmp(value, "per-frame") == 0) {
        job->focus_mode = FT_FOCUS_PER_FRAME;
        return true;
      }
      return false;
    case STRIP_FOCUS_OFFSET:
      return parse_integer(value, &job->focus_offset_steps);
    case STRIP_FILM_BASE: {
      double* density = job->film_base_density;
      /* a 4th match means something follows the three numbers */
      char extra;
      bool is_valid = sscanf(value, "%lf %lf %lf %c", &density[0], &density[1],
                             &density[2], &extra) == 3;
      for(uint32_t channel = 0; channel < 3 && is_valid; channel++) {
        is_valid =
          density[channel] >= 0 && density[channel] <= MAX_FILM_BASE_DENSITY;
      }
      job->has_film_base = is_valid;
      return is_valid;
    }
    case STRIP_DETAIL:
      /* "none" is not written in a file: detail starts at "neg" */
      if(parse_word(DETAIL_WORDS + 1, WORD_COUNT(DETAIL_WORDS) - 1, value,
                    &word) == false) {
        return false;
      }
      job->detail = (ft_detail_output)(word + 1);
      return true;
    case STRIP_DETAIL_FORMAT:
      if(parse_word(DETAIL_FORMAT_WORDS, WORD_COUNT(DETAIL_FORMAT_WORDS), value,
                    &word) == false) {
        return false;
      }
      job->detail_format = (ft_detail_format)word;
      return true;
    case STRIP_DETAIL_BW:
      if(parse_word(BW_WORDS, WORD_COUNT(BW_WORDS), value, &word) == false) {
        return false;
      }
      job->detail_bw = (ft_detail_bw)word;
      return true;
    case STRIP_KEEP_RAW:
      return parse_yes_no(value, &job->keep_raw);
    case STRIP_RING_MEGABYTES:
      return parse_integer_in(value, FT_RING_MEGABYTES_MIN,
                              FT_RING_MEGABYTES_MAX, &job->ring_megabytes);
    case STRIP_SHARE:
      return parse_number(value, &job->share_pixels) &&
             is_share_in_range(job->share_pixels);
    case STRIP_HALF_FRAMES:
      return parse_yes_no(value, &job->half_frames_at_full_frame_ppi);
  }
  return false;
}

/* Read one [frame N] value. Returns true when it was well formed. */
static bool parse_frame_value(ft_job_frame* frame, frame_key key,
                              const char* value) {
  uint32_t word;
  switch(key) {
    case FRAME_TYPE:
      if(parse_word(FRAME_TYPE_WORDS, WORD_COUNT(FRAME_TYPE_WORDS), value,
                    &word) == false) {
        return false;
      }
      frame->type = (ft_frame_type)word;
      return true;
    case FRAME_INCLUDE:
      return parse_yes_no(value, &frame->is_included);
    case FRAME_PPI:
      return parse_number(value, &frame->ppi);
    case FRAME_WINDOW: {
      char extra;
      bool is_valid = sscanf(value, "%lf %lf %lf %lf %c",
                             &frame->along_start_mm, &frame->across_start_mm,
                             &frame->length_mm, &frame->width_mm, &extra) == 4;
      return is_valid &&
             isfinite(frame->along_start_mm) &&
             isfinite(frame->across_start_mm) &&
             isfinite(frame->length_mm) &&
             isfinite(frame->width_mm);
    }
    case FRAME_TILT:
      return parse_number(value, &frame->tilt_degrees);
    case FRAME_SPROCKETS:
      return parse_yes_no(value, &frame->has_sprockets);
    case FRAME_STRAIGHTEN:
      return parse_yes_no(value, &frame->should_straighten);
    case FRAME_CONFIDENCE:
      if(strcmp(value, "low") == 0) {
        frame->is_confidence_low = true;
        return true;
      }
      if(strcmp(value, "high") == 0) {
        frame->is_confidence_low = false;
        return true;
      }
      return false;
    case FRAME_FOCUS_OFFSET:
      return parse_integer(value, &frame->focus_offset_steps);
    case FRAME_DONE:
      return parse_yes_no(value, &frame->is_done);
    case FRAME_BIN:
      return parse_integer_in(value, 0, MAX_BIN, &frame->bin);
    case FRAME_PASS:
      return parse_integer_in(value, 0, FT_MAX_FRAMES, &frame->pass);
    case FRAME_GROUP:
      return parse_integer_in(value, 0, FT_MAX_FRAMES, &frame->group);
    case FRAME_SHARE_PIXELS:
      return parse_number(value, &frame->share_pixels) &&
             is_share_in_range(frame->share_pixels);
  }
  return false;
}

/* A frame section needs a ppi and a window. seen has bit k set for every
 * frame key k read. Returns 0, or -1 with error filled. */
static int32_t finish_frame(const ft_job_frame* frame, uint32_t seen,
                            ft_err* error) {
  if((seen & (1u << FRAME_PPI)) == 0) {
    return ft_fail(error, "line %u: frame %u has no ppi",
                   (unsigned)frame->source_line, (unsigned)frame->index);
  }
  if((seen & (1u << FRAME_WINDOW)) == 0) {
    return ft_fail(error, "line %u: frame %u has no window",
                   (unsigned)frame->source_line, (unsigned)frame->index);
  }
  return 0;
}

/* Read "[frame N]" (header points at the '['). Returns N, or -1 when
 * the header is malformed. */
static int64_t parse_frame_header(const char* header) {
  const char* number_start = header + strlen("[frame ");
  char* end;
  errno = 0;
  long long number = strtoll(number_start, &end, 10);
  bool is_valid = end != number_start &&
                  end[0] == ']' &&
                  end[1] == '\0' &&
                  errno != ERANGE &&
                  number >= 1 &&
                  number <= MAX_FRAME_NUMBER;
  return is_valid ? number : -1;
}

int32_t ft_parse_job(ft_job* job, const char* text, size_t text_length,
                     ft_err* error) {
  const char* cursor = text;
  const char* text_end = text + text_length;
  uint32_t line_number = 0;
  /* the line of [strip]; 0 = not seen yet */
  uint32_t strip_line = 0;
  bool is_in_strip = false;
  /* bit k set: key k of the current section was read */
  uint32_t seen = 0;
  /* the frame section being read; NULL outside one */
  ft_job_frame* frame = NULL;

  memset(job, 0, sizeof *job);
  job->ring_megabytes = FT_RING_MEGABYTES_DEFAULT;
  while(cursor < text_end) {
    const char* newline = memchr(cursor, '\n', (size_t)(text_end - cursor));
    const char* line_end = newline != NULL ? newline : text_end;
    size_t length = (size_t)(line_end - cursor);
    line_number++;
    if(memchr(cursor, '\0', length) != NULL) {
      return ft_fail(error, "line %u: NUL byte", (unsigned)line_number);
    }
    char line[MAX_LINE_BYTES];
    if(length >= sizeof line - 1) {
      return ft_fail(error, "line %u: too long", (unsigned)line_number);
    }
    memcpy(line, cursor, length);
    line[length] = '\0';
    cursor = newline != NULL ? newline + 1 : text_end;

    /* '#' starts a comment anywhere on the line */
    char* hash = strchr(line, '#');
    if(hash != NULL) *hash = '\0';
    char* content = trim(line);
    if(content[0] == '\0') continue;

    if(content[0] == '[') {
      if(strcmp(content, "[strip]") == 0) {
        if(frame != NULL && finish_frame(frame, seen, error) != 0) return -1;
        if(strip_line != 0) {
          return ft_fail(error,
                         "line %u: duplicate [strip] section "
                         "(first at line %u)",
                         (unsigned)line_number, (unsigned)strip_line);
        }
        strip_line = line_number;
        is_in_strip = true;
        frame = NULL;
        seen = 0;
        continue;
      }
      if(strncmp(content, "[frame ", 7) == 0) {
        int64_t number = parse_frame_header(content);
        if(number < 0) {
          return ft_fail(error, "line %u: bad frame header %s",
                         (unsigned)line_number, content);
        }
        if(frame != NULL && finish_frame(frame, seen, error) != 0) return -1;
        for(uint32_t index = 0; index < job->frame_count; index++) {
          if(job->frames[index].index == number) {
            return ft_fail(error,
                           "line %u: frame %d repeated (first at line %u)",
                           (unsigned)line_number, (int)number,
                           (unsigned)job->frames[index].source_line);
          }
        }
        if(job->frame_count == FT_MAX_FRAMES) {
          return ft_fail(error, "line %u: more than %d frames",
                         (unsigned)line_number, FT_MAX_FRAMES);
        }
        frame = &job->frames[job->frame_count];
        job->frame_count++;
        /* safe: parse_frame_header keeps it within 1..1000000 */
        frame->index = (uint32_t)number;
        frame->source_line = line_number;
        frame->is_included = true;
        is_in_strip = false;
        seen = 0;
        continue;
      }
      return ft_fail(error, "line %u: unknown section %s",
                     (unsigned)line_number, content);
    }

    char* value = strchr(content, '=');
    if(value == NULL) {
      return ft_fail(error, "line %u: expected key = value",
                     (unsigned)line_number);
    }
    /* split "key = value" at the '=' */
    *value = '\0';
    value++;
    char* key = trim(content);
    value = trim(value);

    int32_t key_index;
    if(is_in_strip) {
      key_index = find_key(STRIP_KEYS, WORD_COUNT(STRIP_KEYS), key);
    } else if(frame != NULL) {
      key_index = find_key(FRAME_KEYS, WORD_COUNT(FRAME_KEYS), key);
    } else {
      return ft_fail(error, "line %u: %s outside a section",
                     (unsigned)line_number, key);
    }
    if(key_index < 0) {
      return ft_fail(error, "line %u: unknown key %s", (unsigned)line_number,
                     key);
    }
    if((seen & (1u << key_index)) != 0) {
      return ft_fail(error, "line %u: duplicate key %s", (unsigned)line_number,
                     key);
    }
    seen |= 1u << key_index;

    bool is_valid;
    if(is_in_strip) {
      is_valid = parse_strip_value(job, (strip_key)key_index, value);
    } else {
      is_valid = parse_frame_value(frame, (frame_key)key_index, value);
    }
    if(is_valid == false) {
      return ft_fail(error, "line %u: bad value for %s: %s",
                     (unsigned)line_number, key, value);
    }
  }
  if(frame != NULL && finish_frame(frame, seen, error) != 0) return -1;
  if(strip_line == 0) return ft_fail(error, "no [strip] section");
  if(job->holder[0] == '\0') {
    return ft_fail(error, "line %u: [strip] has no holder",
                   (unsigned)strip_line);
  }
  return 0;
}

/* "line 12 (frame 3)", or "frame 3" for a frame not read from a file */
static void describe_frame(char* text, size_t capacity,
                           const ft_job_frame* frame) {
  if(frame->source_line > 0) {
    snprintf(text, capacity, "line %u (frame %u)", (unsigned)frame->source_line,
             (unsigned)frame->index);
  } else {
    snprintf(text, capacity, "frame %u", (unsigned)frame->index);
  }
}

/* An ordinary job has no groups and no share_px. */
static int32_t check_no_share(const ft_job* job, ft_err* error) {
  char where[WHERE_BYTES];
  for(uint32_t index = 0; index < job->frame_count; index++) {
    if(job->frames[index].group != 0) {
      describe_frame(where, sizeof where, &job->frames[index]);
      return ft_fail(error, "%s: group without share in [strip]", where);
    }
  }
  for(uint32_t index = 0; index < job->frame_count; index++) {
    if(job->frames[index].share_pixels > 0) {
      describe_frame(where, sizeof where, &job->frames[index]);
      return ft_fail(error, "%s: share_px without share in [strip]", where);
    }
  }
  return 0;
}

/* A share job scans each group of frames as one window: every included
 * frame is in a group, included frames follow the strip, a group's frames
 * share ppi, bin and target, and are not split by another group's. */
static int32_t check_share_groups(const ft_job* job, ft_err* error) {
  if(!(job->share_pixels > 0)) return check_no_share(job, error);

  /* the included frame before this one; NULL for the first */
  const ft_job_frame* previous = NULL;
  for(uint32_t index = 0; index < job->frame_count; index++) {
    const ft_job_frame* frame = &job->frames[index];
    if(frame->is_included == false) continue;
    char where[WHERE_BYTES];
    describe_frame(where, sizeof where, frame);
    if(frame->group < 1) {
      return ft_fail(error, "%s: share job frame without a group", where);
    }
    if(previous == NULL) {
      previous = frame;
      continue;
    }
    if(frame->along_start_mm < previous->along_start_mm) {
      return ft_fail(error, "%s: share job frames must follow the strip",
                     where);
    }
    if(frame->group != previous->group) {
      /* a new group starts: it must not have appeared before */
      for(const ft_job_frame* earlier = job->frames; earlier < previous;
          earlier++) {
        if(earlier->is_included && earlier->group == frame->group) {
          return ft_fail(error, "%s: group %d is split by group %d", where,
                         (int)frame->group, (int)previous->group);
        }
      }
      previous = frame;
      continue;
    }
    if(frame->ppi != previous->ppi || frame->bin != previous->bin) {
      return ft_fail(error, "%s: frames of group %d differ in ppi or bin",
                     where, (int)frame->group);
    }
    if(ft_get_frame_share_pixels(job, frame) !=
       ft_get_frame_share_pixels(job, previous)) {
      return ft_fail(error, "%s: frames of group %d differ in share_px", where,
                     (int)frame->group);
    }
    previous = frame;
  }
  return 0;
}

/* The detail settings that go together. */
static int32_t check_detail(const ft_job* job, ft_err* error) {
  if(job->detail == FT_DETAIL_POSITIVE && job->film != FT_FILM_NEGATIVE) {
    return ft_fail(error,
                   "detail = pos (archive positive) is for negative "
                   "film; slides: detail = neg");
  }
  if(job->detail == FT_DETAIL_POSITIVE &&
     job->detail_format != FT_DETAIL_FORMAT_TIFF) {
    return ft_fail(error, "detail = pos is always TIFF (detail_fmt = tiff)");
  }
  if(job->detail == FT_DETAIL_NEGATIVE && job->detail_bw != FT_BW_OFF) {
    return ft_fail(error, "detail_bw is for detail = pos only");
  }
  if(job->detail == FT_DETAIL_POSITIVE && job->has_film_base == false) {
    return ft_fail(error, "detail = pos needs film_base (from the preview)");
  }
  return 0;
}

static bool is_window_inside(const ft_job_frame* frame,
                             const ft_holder* holder) {
  return frame->length_mm > 0 &&
         frame->width_mm > 0 &&
         frame->along_start_mm >= 0 &&
         frame->across_start_mm >= 0 &&
         frame->along_start_mm + frame->length_mm <= holder->scan_length_mm &&
         frame->across_start_mm + frame->width_mm <= holder->scan_width_mm;
}

/* Check one included frame. Returns 0, or -1 with error filled. */
static int32_t check_frame(const ft_job* job, uint32_t frame_index,
                           const ft_holder* holder, const ft_lens* lens,
                           ft_err* error) {
  const ft_job_frame* frame = &job->frames[frame_index];
  char where[WHERE_BYTES];
  describe_frame(where, sizeof where, frame);

  if(isfinite(frame->ppi) == false ||
     ft_is_ppi_supported(frame->ppi) == false) {
    return ft_fail(error, "%s: ppi %g not supported (525..6300)", where,
                   frame->ppi);
  }
  if(frame->bin != 0 && ft_is_bin_usable(frame->ppi, frame->bin) == false) {
    return ft_fail(error,
                   "%s: bin %d not usable at %g ppi (the optics "
                   "ppi x bin must be 1500..6300)",
                   where, (int)frame->bin, frame->ppi);
  }
  /* written as "not inside" so that NaN is refused too */
  if(is_window_inside(frame, holder) == false) {
    return ft_fail(error, "%s: window outside the holder (%.0f x %.0f mm)",
                   where, holder->scan_length_mm, holder->scan_width_mm);
  }
  if(!(fabs(frame->tilt_degrees) <= MAX_TILT_DEGREES)) {
    return ft_fail(error, "%s: tilt %g out of range (+-45 degrees)", where,
                   frame->tilt_degrees);
  }
  if(job->detail != FT_DETAIL_NONE) {
    if(frame->pass < 1) {
      return ft_fail(error, "%s: detailed scan without a pass", where);
    }
    if(ft_get_frame_bin(frame) != 1) {
      return ft_fail(error, "%s: detailed scans are bin 1", where);
    }
    for(uint32_t index = 0; index < frame_index; index++) {
      const ft_job_frame* earlier = &job->frames[index];
      if(earlier->is_included &&
         earlier->pass == frame->pass &&
         earlier->ppi != frame->ppi) {
        return ft_fail(error, "%s: pass %d mixes %g and %g ppi", where,
                       (int)frame->pass, earlier->ppi, frame->ppi);
      }
    }
  }

  int64_t offset_steps = job->focus_mode == FT_FOCUS_PER_FRAME
                           ? frame->focus_offset_steps
                           : job->focus_offset_steps;
  int64_t ccd_steps;
  int64_t lens_steps;
  /* the range check first keeps the additions below from overflowing */
  bool is_safe = offset_steps >= -FT_CCD_MAX_STEPS &&
                 offset_steps <= FT_CCD_MAX_STEPS &&
                 ft_find_focused_optics_positions(
                   lens, frame->ppi * ft_get_frame_bin(frame), offset_steps,
                   &ccd_steps, &lens_steps) == 0 &&
                 ft_are_positions_safe(ccd_steps, lens_steps);
  if(is_safe == false) {
    return ft_fail(error,
                   "%s: focus offset %" PRId64
                   " puts the CCD/lens outside the safe range",
                   where, offset_steps);
  }
  return 0;
}

int32_t ft_validate_job(const ft_job* job, const ft_lens* lens, ft_err* error) {
  const ft_holder* holder = ft_find_holder(job->holder);
  if(holder == NULL) return ft_fail(error, "unknown holder '%s'", job->holder);
  if(job->frame_count == 0) return ft_fail(error, "no frames");
  if(check_detail(job, error) != 0) return -1;
  if(check_share_groups(job, error) != 0) return -1;
  for(uint32_t index = 0; index < job->frame_count; index++) {
    if(job->frames[index].is_included == false) continue;
    if(check_frame(job, index, holder, lens, error) != 0) return -1;
  }
  return 0;
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_share_cut.h"

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_detect.h"
#include "ft_geom.h"
#include "ft_holder.h"
#include "ft_noise_model.h"
#include "ft_preview.h"

#define MM_PER_INCH 25.4
/* the frame type words, in the order of ft_frame_type */
static const char* const TYPE_WORDS[] = {"full", "half", "panorama", "partial"};
#define TYPE_COUNT (sizeof TYPE_WORDS / sizeof TYPE_WORDS[0])
/* the detection grid: about this many ppi */
#define DETECTION_PPI 500
/* the fewest lines and pixels the detection takes */
#define MIN_DETECTION_SIDE 8
/* a cut frame needs at least this many lines and pixels */
#define MIN_FRAME_SIDE 8
/* the window's ppi, pixel and share ranges */
#define MIN_PPI 525
#define MAX_PPI 6300
#define MAX_PIXELS 20000
#define MAX_BIN 12
#define MIN_SHARE_PIXELS 100
#define MAX_SHARE_PIXELS 20000
/* the largest film base density */
#define MAX_FILM_BASE_DENSITY 4.5
/* key and value room on one line */
#define KEY_BYTES 32
#define VALUE_BYTES 160

static bool parse_type(const char* word, ft_frame_type* type) {
  for(size_t index = 0; index < TYPE_COUNT; index++) {
    if(strcmp(word, TYPE_WORDS[index]) == 0) {
      *type = (ft_frame_type)index;
      return true;
    }
  }
  return false;
}

/* Read the "key=value" line at position: key and value are copied (cut
 * to their buffers). Returns the position after the line; text_length at
 * the end. */
static size_t read_key_value(const char* text, size_t text_length,
                             size_t position, char* key, size_t key_capacity,
                             char* value, size_t value_capacity) {
  size_t line_end = position;
  while(line_end < text_length && text[line_end] != '\n') line_end++;
  size_t key_length = 0;
  size_t value_length = 0;
  size_t cursor = position;
  for(; cursor < line_end && text[cursor] != '='; cursor++) {
    if(key_length + 1 < key_capacity) {
      key[key_length] = text[cursor];
      key_length++;
    }
  }
  key[key_length] = '\0';
  if(cursor < line_end) {
    /* skip the '='; the value ends at the line's end or a '\r' */
    for(cursor++; cursor < line_end && text[cursor] != '\r'; cursor++) {
      if(value_length + 1 < value_capacity) {
        value[value_length] = text[cursor];
        value_length++;
      }
    }
  }
  value[value_length] = '\0';
  return line_end < text_length ? line_end + 1 : text_length;
}

/* a finite number, nothing after it */
static bool parse_number(const char* value, double* out) {
  char* end;
  *out = strtod(value, &end);
  return end != value && *end == '\0' && isfinite(*out);
}

/* a whole number, nothing after it */
static bool parse_integer(const char* value, int64_t* out) {
  char* end;
  long long number = strtoll(value, &end, 10);
  *out = number;
  return end != value && *end == '\0';
}

/* 0 or 1 as a bool */
static bool parse_flag(const char* value, bool* out) {
  int64_t number;
  if(parse_integer(value, &number) == false) return false;
  if(number != 0 && number != 1) return false;
  *out = number == 1;
  return true;
}

static bool parse_film(const char* value, ft_film* film) {
  if(strcmp(value, "negative") == 0) {
    *film = FT_FILM_NEGATIVE;
    return true;
  }
  if(strcmp(value, "slide") == 0) {
    *film = FT_FILM_SLIDE;
    return true;
  }
  return false;
}

/* three densities within 0..MAX_FILM_BASE_DENSITY */
static bool parse_film_base(const char* value, double density[3]) {
  /* a 4th match means something follows the three numbers */
  char extra;
  if(sscanf(value, "%lf %lf %lf %c", &density[0], &density[1], &density[2],
            &extra) != 3) {
    return false;
  }
  for(uint32_t channel = 0; channel < 3; channel++) {
    if(!(density[channel] >= 0 && density[channel] <= MAX_FILM_BASE_DENSITY)) {
      return false;
    }
  }
  return true;
}

/* the keys a window .txt must have, as bits */
enum {
  SEEN_PPI = 1,
  SEEN_PIXELS = 2,
  SEEN_X = 4,
  SEEN_Y = 8,
  SEEN_BIN = 16,
  SEEN_SHARE = 32,
  SEEN_ALL = 63
};

/* Read one share_frame value into the next frame. Returns 0, or -1 with
 * error filled (bad value, or a frame listed twice). */
static int32_t parse_share_frame(ft_share_metadata* metadata, const char* value,
                                 ft_err* error) {
  ft_share_frame* frame = &metadata->frames[metadata->frame_count];
  char type_word[16];
  char extra;
  int index;
  bool is_valid =
    sscanf(value, "%d %15s %lf %lf %lf %lf %c", &index, type_word,
           &frame->along_start_mm, &frame->length_mm, &frame->across_start_mm,
           &frame->width_mm, &extra) == 6 &&
    parse_type(type_word, &frame->type) &&
    index >= 1 &&
    frame->length_mm > 0 &&
    frame->width_mm > 0 &&
    isfinite(frame->along_start_mm) &&
    isfinite(frame->across_start_mm);
  if(is_valid == false) {
    return ft_fail(error, "share meta: bad share_frame=%s", value);
  }
  frame->index = (uint32_t)index;
  for(uint32_t other = 0; other < metadata->frame_count; other++) {
    if(metadata->frames[other].index == frame->index) {
      return ft_fail(error, "share meta: frame %d listed twice", index);
    }
  }
  metadata->frame_count++;
  return 0;
}

int32_t ft_parse_share_metadata(const char* text, size_t text_length,
                                ft_share_metadata* metadata, ft_err* error) {
  char key[KEY_BYTES];
  char value[VALUE_BYTES];
  size_t position = 0;
  uint32_t seen = 0;
  memset(metadata, 0, sizeof *metadata);
  metadata->film = FT_FILM_NEGATIVE;
  strcpy(metadata->holder, "24x217");
  while(position < text_length) {
    position = read_key_value(text, text_length, position, key, sizeof key,
                              value, sizeof value);
    if(key[0] == '\0') continue;
    int64_t number;
    bool is_valid = true;
    if(strcmp(key, "ppi") == 0) {
      is_valid = parse_number(value, &metadata->ppi) &&
                 metadata->ppi >= MIN_PPI &&
                 metadata->ppi <= MAX_PPI;
      seen |= SEEN_PPI;
    } else if(strcmp(key, "px") == 0) {
      is_valid =
        parse_integer(value, &number) && number >= 1 && number <= MAX_PIXELS;
      metadata->pixel_count = (int32_t)number;
      seen |= SEEN_PIXELS;
    } else if(strcmp(key, "x") == 0) {
      is_valid = parse_integer(value, &metadata->first_pixel) &&
                 metadata->first_pixel >= 0;
      seen |= SEEN_X;
    } else if(strcmp(key, "y") == 0) {
      is_valid = parse_integer(value, &metadata->first_line) &&
                 metadata->first_line >= 0;
      seen |= SEEN_Y;
    } else if(strcmp(key, "bin") == 0) {
      /* the bin must divide 12 */
      is_valid = parse_integer(value, &number) &&
                 number >= 1 &&
                 number <= MAX_BIN &&
                 MAX_BIN % number == 0;
      metadata->bin = (int32_t)number;
      seen |= SEEN_BIN;
    } else if(strcmp(key, "share") == 0) {
      is_valid = parse_number(value, &metadata->share_pixels) &&
                 metadata->share_pixels >= MIN_SHARE_PIXELS &&
                 metadata->share_pixels <= MAX_SHARE_PIXELS;
      seen |= SEEN_SHARE;
    } else if(strcmp(key, "hf_half_res") == 0) {
      is_valid = parse_flag(value, &metadata->half_frames_at_full_frame_ppi);
    } else if(strcmp(key, "film") == 0) {
      is_valid = parse_film(value, &metadata->film);
    } else if(strcmp(key, "holder") == 0) {
      is_valid = value[0] != '\0' && strlen(value) < sizeof metadata->holder;
      if(is_valid) strcpy(metadata->holder, value);
    } else if(strcmp(key, "film_base") == 0) {
      is_valid = parse_film_base(value, metadata->film_base_density);
      metadata->has_film_base = is_valid;
    } else if(strcmp(key, "share_frame") == 0) {
      if(metadata->frame_count == FT_MAX_FRAMES) {
        return ft_fail(error, "share meta: more than %d share_frame",
                       FT_MAX_FRAMES);
      }
      if(parse_share_frame(metadata, value, error) != 0) return -1;
    }
    if(is_valid == false) {
      return ft_fail(error, "share meta: bad %s=%s", key, value);
    }
  }
  if((seen & SEEN_ALL) != SEEN_ALL) {
    const char* missing = "share";
    if((seen & SEEN_PPI) == 0) {
      missing = "ppi";
    } else if((seen & SEEN_PIXELS) == 0) {
      missing = "px";
    } else if((seen & SEEN_X) == 0) {
      missing = "x";
    } else if((seen & SEEN_Y) == 0) {
      missing = "y";
    } else if((seen & SEEN_BIN) == 0) {
      missing = "bin";
    }
    return ft_fail(error, "share meta: missing %s", missing);
  }
  if(metadata->frame_count == 0) {
    return ft_fail(error, "share meta: no share_frame");
  }
  if(ft_find_holder(metadata->holder) == NULL) {
    return ft_fail(error, "share meta: unknown holder %s", metadata->holder);
  }
  return 0;
}

/* The detected frame overlapping the preview frame most along, if it is
 * that frame: an overlap of at least half the shorter one, the length
 * within 25 %, the shift within the margin (more: creep we did not scan
 * for). Returns its index, or -1. */
static int32_t match_frame(const ft_layout* layout,
                           const ft_share_frame* preview, const bool* is_used) {
  int32_t best = -1;
  double best_overlap = 0;
  double preview_end = preview->along_start_mm + preview->length_mm;
  for(uint32_t index = 0; index < layout->frame_count; index++) {
    const ft_detected_frame* found = &layout->frames[index];
    double length = found->end_mm - found->start_mm;
    double overlap_end =
      found->end_mm < preview_end ? found->end_mm : preview_end;
    double overlap_start = found->start_mm > preview->along_start_mm
                             ? found->start_mm
                             : preview->along_start_mm;
    double overlap = overlap_end - overlap_start;
    if(is_used[index] || overlap <= best_overlap) continue;
    double shorter = length < preview->length_mm ? length : preview->length_mm;
    if(overlap < 0.5 * shorter ||
       fabs(length - preview->length_mm) > 0.25 * preview->length_mm ||
       fabs(found->start_mm - preview->along_start_mm) > FT_SHARE_MARGIN_MM) {
      continue;
    }
    best_overlap = overlap;
    best = (int32_t)index;
  }
  return best;
}

/* qsort's comparison: ascending doubles */
static int compare_doubles(const void* first, const void* second) {
  double a = *(const double*)first;
  double b = *(const double*)second;
  if(a < b) return -1;
  return a > b ? 1 : 0;
}

/* round to the nearest integer, halves up */
static int64_t round_to_int64(double value) {
  return (int64_t)floor(value + 0.5);
}

/* the aligned row at holder mm along, as Sancho's write_crop maps it */
static int64_t get_row(const ft_share_metadata* metadata, double along_mm,
                       int32_t row_offset) {
  return round_to_int64(along_mm / MM_PER_INCH * metadata->ppi) -
         metadata->first_line -
         row_offset;
}

/* the pixel at holder mm across, as Sancho's write_crop maps it */
static int64_t get_column(const ft_share_metadata* metadata, double across_mm,
                          double scale_ppi) {
  double ccd_pixel = ft_get_ccd_pixel_from_mm(across_mm, scale_ppi);
  return round_to_int64((ccd_pixel - FT_CCD_FIRST_PIXEL) / metadata->bin) -
         metadata->first_pixel;
}

/* Detection on a ~500 ppi copy of the developed window, in holder mm.
 * Returns true when it ran and found the layout. */
static bool detect_window(const float* transmittance, uint32_t lines,
                          const ft_share_metadata* metadata, int32_t row_offset,
                          ft_layout* layout) {
  int32_t factor = (int32_t)floor(metadata->ppi / DETECTION_PPI + 0.5);
  if(factor < 1) factor = 1;
  double scale_ppi = metadata->ppi * metadata->bin;
  ft_axes axes;
  axes.mm_per_line = MM_PER_INCH / metadata->ppi * factor;
  axes.first_line_mm =
    (metadata->first_line + row_offset + (factor - 1) / 2.0) *
    MM_PER_INCH /
    metadata->ppi;
  /* the centre of a box: factor x bin CCD pixels */
  double box_centre = (factor * metadata->bin - 1) / 2.0;
  double first_ccd_pixel =
    ft_get_ccd_pixel_from_output(metadata->first_pixel, 0, metadata->bin) +
    box_centre;
  double next_ccd_pixel =
    ft_get_ccd_pixel_from_output(metadata->first_pixel, factor, metadata->bin) +
    box_centre;
  axes.first_pixel_mm = ft_get_mm_from_ccd_pixel(first_ccd_pixel, scale_ppi);
  axes.mm_per_pixel =
    ft_get_mm_from_ccd_pixel(next_ccd_pixel, scale_ppi) - axes.first_pixel_mm;

  uint32_t small_lines = lines / factor;
  uint32_t small_pixels = (uint32_t)metadata->pixel_count / factor;
  if(small_lines < MIN_DETECTION_SIDE || small_pixels < MIN_DETECTION_SIDE) {
    return false;
  }
  float* small = malloc((size_t)small_lines * small_pixels * 3 * sizeof *small);
  if(small == NULL) return false;
  ft_box_average(transmittance, lines, (uint32_t)metadata->pixel_count,
                 (uint32_t)factor, small);
  ft_err detect_error;
  bool was_detected =
    ft_detect_frames(small, small_lines, small_pixels, &axes, metadata->film,
                     ft_find_holder(metadata->holder), layout,
                     &detect_error) == 0;
  free(small);
  return was_detected;
}

int32_t ft_cut_share_frames(const uint16_t* raw, uint32_t lines,
                            const uint16_t* calibration,
                            uint32_t calibration_lines, const char* faulty_dump,
                            size_t faulty_dump_length,
                            const ft_defects* defects,
                            const ft_share_metadata* metadata,
                            ft_share_cut_result* result, ft_err* error) {
  memset(result, 0, sizeof *result);
  ft_develop_options develop;
  memset(&develop, 0, sizeof develop);
  develop.first_pixel = (int32_t)metadata->first_pixel;
  develop.bin = metadata->bin;
  develop.defects = defects;
  uint32_t aligned_lines;
  float* transmittance = ft_develop_transmittance(
    raw, lines, (uint32_t)metadata->pixel_count, calibration, calibration_lines,
    faulty_dump, faulty_dump_length, &develop, &aligned_lines, error);
  if(transmittance == NULL) return -1;
  int32_t status = -1;
  int32_t row_offset = FT_CCD_ROW_SPACING_LINES / metadata->bin;
  double scale_ppi = metadata->ppi * metadata->bin;
  /* one NoiseProfile for every frame of the window (the same .cal);
   * none is not an error, export estimates */
  result->noise_result = ft_measure_noise_profile(
    calibration, calibration_lines, (uint32_t)metadata->pixel_count,
    result->noise_profile, &result->noise_error);

  /* static: ft_layout is large, keep it off the (wasm) stack */
  static ft_layout layout;
  result->was_detected =
    detect_window(transmittance, aligned_lines, metadata, row_offset, &layout);
  if(result->was_detected == false) layout.frame_count = 0;

  /* match every preview frame to a detected one */
  bool is_used[FT_MAX_FRAMES];
  bool is_found[FT_MAX_FRAMES];
  double along_start[FT_MAX_FRAMES];
  double along_end[FT_MAX_FRAMES];
  double across_start[FT_MAX_FRAMES];
  double across_end[FT_MAX_FRAMES];
  double shifts[FT_MAX_FRAMES];
  uint32_t shift_count = 0;
  memset(is_used, 0, sizeof is_used);
  for(uint32_t index = 0; index < metadata->frame_count; index++) {
    const ft_share_frame* preview = &metadata->frames[index];
    int32_t match = match_frame(&layout, preview, is_used);
    is_found[index] = match >= 0;
    if(match < 0) continue;
    const ft_detected_frame* found = &layout.frames[match];
    is_used[match] = true;
    along_start[index] = found->start_mm;
    along_end[index] = found->end_mm;
    across_start[index] = found->across_start_mm;
    across_end[index] = found->across_end_mm;
    shifts[shift_count] = along_start[index] - preview->along_start_mm;
    shift_count++;
  }
  double median_shift = 0;
  if(shift_count > 0) {
    qsort(shifts, shift_count, sizeof shifts[0], compare_doubles);
    if(shift_count % 2 == 1) {
      median_shift = shifts[shift_count / 2];
    } else {
      median_shift =
        0.5 * (shifts[shift_count / 2 - 1] + shifts[shift_count / 2]);
    }
  }
  ft_share_options share_options;
  ft_init_share_options(&share_options, 0);
  share_options.target_pixels = metadata->share_pixels;
  share_options.half_frames_at_full_frame_ppi =
    metadata->half_frames_at_full_frame_ppi;

  for(uint32_t index = 0; index < metadata->frame_count; index++) {
    const ft_share_frame* preview = &metadata->frames[index];
    ft_share_cut_frame* cut = &result->frames[result->frame_count];
    double bleed = FT_SHARE_BLEED_MM;
    if(is_found[index] == false) {
      along_start[index] = preview->along_start_mm + median_shift;
      along_end[index] =
        preview->along_start_mm + preview->length_mm + median_shift;
      across_start[index] = preview->across_start_mm;
      across_end[index] = preview->across_start_mm + preview->width_mm;
    }
    cut->index = preview->index;
    cut->type = preview->type;
    cut->is_estimated = is_found[index] == false;
    cut->shift_mm = along_start[index] - preview->along_start_mm;
    /* holder mm -> aligned rows (along) and columns (across): the frame
     * itself, and the frame with its bleed */
    int64_t frame_first_row = get_row(metadata, along_start[index], row_offset);
    int64_t frame_end_row = get_row(metadata, along_end[index], row_offset);
    int64_t frame_first_column =
      get_column(metadata, across_start[index], scale_ppi);
    int64_t frame_end_column =
      get_column(metadata, across_end[index], scale_ppi);
    int64_t first_row =
      get_row(metadata, along_start[index] - bleed, row_offset);
    int64_t end_row = get_row(metadata, along_end[index] + bleed, row_offset);
    int64_t first_column =
      get_column(metadata, across_start[index] - bleed, scale_ppi);
    int64_t end_column =
      get_column(metadata, across_end[index] + bleed, scale_ppi);
    /* clamp the bleed box into the scan, the frame into the bleed box */
    if(first_row < 0) first_row = 0;
    if(first_column < 0) first_column = 0;
    if(end_row > aligned_lines) end_row = aligned_lines;
    if(end_column > metadata->pixel_count) end_column = metadata->pixel_count;
    if(frame_first_row < first_row) frame_first_row = first_row;
    if(frame_first_column < first_column) frame_first_column = first_column;
    if(frame_end_row > end_row) frame_end_row = end_row;
    if(frame_end_column > end_column) frame_end_column = end_column;
    if(frame_end_row - frame_first_row < MIN_FRAME_SIDE ||
       frame_end_column - frame_first_column < MIN_FRAME_SIDE) {
      ft_fail(error, "share: frame %u is outside the scanned window",
              (unsigned)preview->index);
      goto cleanup;
    }

    ft_develop_options crop;
    memset(&crop, 0, sizeof crop);
    crop.crop_x = (int32_t)first_column;
    crop.crop_y = (int32_t)first_row;
    crop.crop_width = (int32_t)(end_column - first_column);
    crop.crop_height = (int32_t)(end_row - first_row);
    ft_init_image(&cut->image);
    if(ft_crop_and_orient(transmittance, aligned_lines,
                          (uint32_t)metadata->pixel_count, &crop, &cut->image,
                          error) != 0) {
      goto cleanup;
    }
    result->frame_count++;
    cut->image.ppi = metadata->ppi;
    cut->image.film = metadata->film;
    /* OK or CLAMPED: there is a profile */
    if(result->noise_result >= 0) {
      cut->image.has_noise_profile = true;
      memcpy(cut->image.noise_profile, result->noise_profile,
             sizeof cut->image.noise_profile);
    }
    cut->image.frame_number = preview->index;
    snprintf(cut->image.description, sizeof cut->image.description,
             "share frame %u, %g ppi, bin %d%s", (unsigned)preview->index,
             metadata->ppi, (int)metadata->bin,
             cut->is_estimated ? ", box estimated" : "");
    /* the image is turned: x = along (columns), y = across (rows) */
    cut->box[0] = (int32_t)(frame_first_row - first_row);
    cut->box[1] = (int32_t)(frame_first_column - first_column);
    cut->box[2] = (int32_t)(frame_end_row - frame_first_row);
    cut->box[3] = (int32_t)(frame_end_column - frame_first_column);
    ft_share_frame found_frame = *preview;
    found_frame.length_mm = along_end[index] - along_start[index];
    found_frame.width_mm = across_end[index] - across_start[index];
    double long_side = cut->box[2] > cut->box[3] ? cut->box[2] : cut->box[3];
    double target = ft_get_share_target_pixels(&found_frame, &share_options);
    cut->export_scale = target < long_side ? target / long_side : 1.0;
  }
  status = 0;

cleanup:
  free(transmittance);
  if(status != 0) ft_free_share_cut(result);
  return status;
}

void ft_free_share_cut(ft_share_cut_result* result) {
  for(uint32_t index = 0; index < result->frame_count; index++) {
    ft_free_image(&result->frames[index].image);
  }
  result->frame_count = 0;
}

/* Text going into a caller's buffer, as snprintf: length counts every
 * byte asked for; the buffer keeps what fits. */
typedef struct text_buffer {
  char* text;
  size_t capacity;
  size_t length;
} text_buffer;

#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
static void append(text_buffer* buffer, const char* format, ...) {
  bool has_room = buffer->text != NULL && buffer->length < buffer->capacity;
  char* end = has_room ? buffer->text + buffer->length : NULL;
  size_t room = has_room ? buffer->capacity - buffer->length : 0;
  va_list values;
  va_start(values, format);
  int written = vsnprintf(end, room, format, values);
  va_end(values);
  if(written > 0) buffer->length += (size_t)written;
}

int32_t ft_format_share_sidecar(char* text, size_t capacity,
                                const ft_share_metadata* metadata,
                                const ft_share_cut_frame* frame) {
  text_buffer buffer = {.text = text, .capacity = capacity, .length = 0};
  if(text != NULL && capacity > 0) text[0] = '\0';
  /* the cast to unsigned also catches values below 0 */
  const char* type_word =
    (unsigned)frame->type < TYPE_COUNT ? TYPE_WORDS[frame->type] : "full";
  append(
    &buffer, "frame=%u\ntype=%s\nfilm=%s\nppi=%g\nshare=%g\nhf_half_res=%d\n",
    (unsigned)frame->index, type_word,
    metadata->film == FT_FILM_SLIDE ? "slide" : "negative", metadata->ppi,
    metadata->share_pixels, metadata->half_frames_at_full_frame_ppi ? 1 : 0);
  if(metadata->has_film_base) {
    append(&buffer, "film_base=%.4f %.4f %.4f\n",
           metadata->film_base_density[0], metadata->film_base_density[1],
           metadata->film_base_density[2]);
  }
  if(frame->image.has_noise_profile) {
    const double* noise = frame->image.noise_profile;
    append(&buffer, "noise_profile=%.17g %.17g %.17g %.17g %.17g %.17g\n",
           noise[0], noise[1], noise[2], noise[3], noise[4], noise[5]);
  }
  append(&buffer,
         "box=%d %d %d %d\nexport_scale=%.6f\nestimated=%d\nshift_mm=%.3f\n",
         (int)frame->box[0], (int)frame->box[1], (int)frame->box[2],
         (int)frame->box[3], frame->export_scale, frame->is_estimated ? 1 : 0,
         frame->shift_mm);
  return (int32_t)buffer.length;
}

/* six numbers; has_profile is false when one is out of range */
static bool parse_noise_profile(const char* value, double profile[6],
                                bool* has_profile) {
  char extra;
  if(sscanf(value, "%lf %lf %lf %lf %lf %lf %c", &profile[0], &profile[1],
            &profile[2], &profile[3], &profile[4], &profile[5], &extra) != 6) {
    *has_profile = false;
    return false;
  }
  *has_profile = true;
  /* pairs of S (even, >= 0) and O (odd, > 0) */
  for(uint32_t index = 0; index < 6; index++) {
    double number = profile[index];
    bool is_offset = index % 2 == 1;
    if(isfinite(number) == false || number < 0 || (is_offset && number == 0)) {
      *has_profile = false;
    }
  }
  return true;
}

int32_t ft_parse_share_sidecar(const char* text, size_t text_length,
                               ft_share_sidecar* sidecar, ft_err* error) {
  char key[KEY_BYTES];
  char value[VALUE_BYTES];
  size_t position = 0;
  bool has_box = false;
  bool has_scale = false;
  memset(sidecar, 0, sizeof *sidecar);
  sidecar->film = FT_FILM_NEGATIVE;
  while(position < text_length) {
    position = read_key_value(text, text_length, position, key, sizeof key,
                              value, sizeof value);
    if(key[0] == '\0') continue;
    int64_t number;
    bool is_valid = true;
    if(strcmp(key, "frame") == 0) {
      is_valid = parse_integer(value, &number) && number >= 0;
      sidecar->frame = (uint32_t)number;
    } else if(strcmp(key, "type") == 0) {
      is_valid = parse_type(value, &sidecar->type);
    } else if(strcmp(key, "film") == 0) {
      is_valid = parse_film(value, &sidecar->film);
    } else if(strcmp(key, "ppi") == 0) {
      is_valid = parse_number(value, &sidecar->ppi) && sidecar->ppi > 0;
    } else if(strcmp(key, "share") == 0) {
      is_valid = parse_number(value, &sidecar->share_pixels) &&
                 sidecar->share_pixels >= 0;
    } else if(strcmp(key, "hf_half_res") == 0) {
      is_valid = parse_flag(value, &sidecar->half_frames_at_full_frame_ppi);
    } else if(strcmp(key, "estimated") == 0) {
      is_valid = parse_flag(value, &sidecar->is_estimated);
    } else if(strcmp(key, "shift_mm") == 0) {
      is_valid = parse_number(value, &sidecar->shift_mm);
    } else if(strcmp(key, "export_scale") == 0) {
      is_valid = parse_number(value, &sidecar->export_scale) &&
                 sidecar->export_scale > 0 &&
                 sidecar->export_scale <= 1;
      has_scale = is_valid;
    } else if(strcmp(key, "box") == 0) {
      int32_t* box = sidecar->box;
      char extra;
      is_valid = sscanf(value, "%d %d %d %d %c", &box[0], &box[1], &box[2],
                        &box[3], &extra) == 4 &&
                 box[0] >= 0 &&
                 box[1] >= 0 &&
                 box[2] >= 1 &&
                 box[3] >= 1;
      has_box = is_valid;
    } else if(strcmp(key, "film_base") == 0) {
      is_valid = parse_film_base(value, sidecar->film_base_density);
      sidecar->has_film_base = is_valid;
    } else if(strcmp(key, "noise_profile") == 0) {
      is_valid = parse_noise_profile(value, sidecar->noise_profile,
                                     &sidecar->has_noise_profile);
    }
    if(is_valid == false) {
      return ft_fail(error, "sidecar: bad %s=%s", key, value);
    }
  }
  if(has_box == false) return ft_fail(error, "sidecar: missing box");
  if(has_scale == false) return ft_fail(error, "sidecar: missing export_scale");
  return 0;
}

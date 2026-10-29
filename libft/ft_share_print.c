/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_share_print.h"

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_emulsion.h"
#include "ft_icc.h"
#include "ft_jpeg.h"
#include "ft_nr.h"
#include "ft_resize.h"
#include "ft_sharpen.h"

/* room for one formatted float */
#define FLOAT_TEXT_BYTES 32

const ft_look_detail ft_share_detail = {
  .nr_sensor = 1, .nr_chroma = 2, .nr_grain = 0, .sharpen = 0.5};

const char* ft_format_float(float value, char* text, size_t capacity) {
  for(int precision = 6; precision < 9; precision++) {
    snprintf(text, capacity, "%.*g", precision, (double)value);
    if(strtof(text, NULL) == value) return text;
  }
  snprintf(text, capacity, "%.9g", (double)value);
  return text;
}

/* do two noise profiles differ by more than rounding? */
static bool do_profiles_differ(const double first[6], const double second[6]) {
  for(uint32_t index = 0; index < 6; index++) {
    if(fabs(first[index] - second[index]) > 1e-6 * fabs(second[index])) {
      return true;
    }
  }
  return false;
}

/* The profile for sensor NR: the DNG's NoiseProfile, else the sidecar's,
 * else measured on the pixels. */
static void choose_noise_profile(const ft_image* image,
                                 const ft_share_sidecar* sidecar,
                                 const ft_image* cut, double profile[6],
                                 void* scratch, uint32_t* notes) {
  bool has_sidecar_profile = sidecar != NULL && sidecar->has_noise_profile;
  if(image->has_noise_profile) {
    memcpy(profile, image->noise_profile, 6 * sizeof *profile);
    if(has_sidecar_profile &&
       do_profiles_differ(sidecar->noise_profile, profile)) {
      *notes |= FT_NOISE_NOTE_DISAGREE;
    }
  } else if(has_sidecar_profile) {
    memcpy(profile, sidecar->noise_profile, 6 * sizeof *profile);
  } else {
    ft_estimate_noise_profile(cut->pixels, cut->width, cut->height, profile,
                              scratch);
    *notes |= FT_NOISE_NOTE_ESTIMATED;
  }
}

int32_t ft_denoise_share_frame(const ft_image* image,
                               const ft_share_sidecar* sidecar, ft_image* cut,
                               const ft_look_detail* detail, uint32_t* notes,
                               ft_err* error) {
  uint32_t found_notes = 0;
  if(notes != NULL) *notes = 0;
  if(!(detail->nr_sensor > 0) && !(detail->nr_grain > 0)) return 0;
  void* scratch =
    malloc(ft_get_noise_reduction_scratch_bytes(cut->width, cut->height));
  if(scratch == NULL) return ft_fail(error, "out of memory");
  if(detail->nr_sensor > 0) {
    double profile[6];
    choose_noise_profile(image, sidecar, cut, profile, scratch, &found_notes);
    ft_reduce_sensor_noise(cut->pixels, cut->width, cut->height, profile,
                           detail->nr_sensor,
                           detail->nr_sensor * detail->nr_chroma, scratch);
  }
  if(detail->nr_grain > 0) {
    ft_reduce_grain(cut->pixels, cut->width, cut->height, detail->nr_grain,
                    scratch);
  }
  free(scratch);
  if(notes != NULL) *notes = found_notes;
  return 0;
}

int32_t ft_finish_share_jpeg(const ft_image* cut, double scale, double sharpen,
                             ft_space space, ft_gamut gamut, int32_t quality,
                             const char* text, uint8_t** jpeg,
                             size_t* jpeg_length, uint32_t* width,
                             uint32_t* height, ft_err* error) {
  *jpeg = NULL;
  *jpeg_length = 0;
  uint32_t output_width;
  uint32_t output_height;
  ft_get_resized_size(cut->width, cut->height, scale, &output_width,
                      &output_height);
  size_t resize_scratch_bytes = ft_get_resize_scratch_bytes(
    cut->width, cut->height, output_width, output_height);
  size_t pixel_count = (size_t)output_width * output_height;
  float* small = malloc(pixel_count * 3 * sizeof *small);
  float* scratch = malloc(resize_scratch_bytes);
  uint8_t* bytes = malloc(pixel_count * 3);
  /* ~330 KB of tables: too big for the stack */
  ft_output* output = malloc(sizeof *output);
  uint8_t* icc_profile = malloc(FT_ICC_MAX_BYTES);
  int32_t result = -1;
  if(small == NULL ||
     scratch == NULL ||
     bytes == NULL ||
     output == NULL ||
     icc_profile == NULL) {
    ft_fail(error, "out of memory");
    goto cleanup;
  }
  if(ft_resize(cut->pixels, cut->width, cut->height, small, output_width,
               output_height, scratch, error) != 0 ||
     ft_compile_output(space, gamut, output, error) != 0) {
    goto cleanup;
  }
  /* output sharpening, after the resize: its scratch reuses the resize
   * scratch when that is big enough */
  if(sharpen > 0) {
    size_t needed = ft_get_sharpen_scratch_bytes(output_width, output_height);
    if(needed > resize_scratch_bytes) {
      float* bigger = realloc(scratch, needed);
      if(bigger == NULL) {
        ft_fail(error, "out of memory");
        goto cleanup;
      }
      scratch = bigger;
    }
    ft_sharpen(small, output_width, output_height, sharpen,
               (double)output_width / cut->width, scratch);
  }
  /* row by row: output colour, then 8 bits */
  for(uint32_t y = 0; y < output_height; y++) {
    float* row = small + (size_t)y * output_width * 3;
    ft_apply_output(output, row, row, output_width);
    ft_quantise_output8(row, bytes + (size_t)y * output_width * 3, output_width,
                        (size_t)y * output_width);
  }
  size_t icc_profile_length =
    ft_build_icc_profile(space, icc_profile, FT_ICC_MAX_BYTES);
  if(icc_profile_length == 0) {
    ft_fail(error, "cannot build the ICC profile");
    goto cleanup;
  }
  double ppi = cut->ppi > 0 ? cut->ppi * output_width / cut->width : 0;
  if(ft_encode_jpeg(bytes, output_width, output_height, quality, ppi,
                    icc_profile, icc_profile_length, text, jpeg, jpeg_length,
                    error) != 0) {
    goto cleanup;
  }
  if(width != NULL) *width = output_width;
  if(height != NULL) *height = output_height;
  result = 0;

cleanup:
  free(small);
  free(scratch);
  free(bytes);
  free(output);
  free(icc_profile);
  return result;
}

int32_t ft_make_share_proxy(const ft_image* cut, ft_image* proxy,
                            ft_err* error) {
  uint32_t proxy_width;
  uint32_t proxy_height;
  ft_get_print_proxy_size(cut->width, cut->height, &proxy_width, &proxy_height);
  void* scratch = malloc(ft_get_resize_scratch_bytes(
    cut->width, cut->height, proxy_width, proxy_height));
  if(scratch == NULL) return ft_fail(error, "out of memory");
  if(ft_allocate_image(proxy, proxy_width, proxy_height, error) != 0 ||
     ft_resize(cut->pixels, cut->width, cut->height, proxy->pixels, proxy_width,
               proxy_height, scratch, error) != 0) {
    free(scratch);
    ft_free_image(proxy);
    return -1;
  }
  free(scratch);
  return 0;
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

/* "name=r g b\n" with each value as ft_format_float writes it */
static void append_triple(text_buffer* buffer, const char* name,
                          const float values[3]) {
  char texts[3][FLOAT_TEXT_BYTES];
  append(buffer, "%s=%s %s %s\n", name,
         ft_format_float(values[0], texts[0], FLOAT_TEXT_BYTES),
         ft_format_float(values[1], texts[1], FLOAT_TEXT_BYTES),
         ft_format_float(values[2], texts[2], FLOAT_TEXT_BYTES));
}

/* "name=v\n" with the value as ft_format_float writes it */
static void append_float(text_buffer* buffer, const char* name, float value) {
  char text[FLOAT_TEXT_BYTES];
  append(buffer, "%s=%s\n", name,
         ft_format_float(value, text, FLOAT_TEXT_BYTES));
}

static const char* get_film_word(ft_render_mode mode) {
  switch(mode) {
    case FT_RENDER_BW:
      return "bw";
    case FT_RENDER_SLIDE:
      return "slide";
    case FT_RENDER_NEGATIVE:
      return "neg";
  }
  return "neg";
}

int32_t ft_format_share_print_text(char* text, size_t capacity,
                                   const ft_emulsion* emulsion,
                                   int32_t frame_number, ft_cmyd clicks,
                                   const ft_print_auto* automatic,
                                   const ft_recipe* recipe,
                                   const ft_look_detail* detail) {
  text_buffer buffer = {.text = text, .capacity = capacity, .length = 0};
  append(&buffer, "emulsion=%s\n", emulsion->id);
  append(&buffer, "science=%d\n", FT_EMULSION_REV);
  append(&buffer, "chain=%s %s %s %s\n", emulsion->film->name,
         emulsion->balance->name, emulsion->print->name,
         emulsion->output->name);
  if(frame_number >= 0) append(&buffer, "frame=%d\n", (int)frame_number);
  append(&buffer, "cmyd=%d %d %d %d\n", (int)clicks.cyan, (int)clicks.magenta,
         (int)clicks.yellow, (int)clicks.density);
  append_float(&buffer, "auto_exposure", automatic->exposure);
  append_triple(&buffer, "auto_tint", automatic->tint);
  append(&buffer, "film=%s\n", get_film_word(recipe->mode));
  append_triple(&buffer, "base", recipe->base_density);
  append_triple(&buffer, "range", recipe->range_density);
  append_float(&buffer, "exposure", recipe->exposure);
  append_triple(&buffer, "tint", recipe->tint);
  append_float(&buffer, "grade", recipe->grade);
  append_float(&buffer, "paper_black", recipe->paper_black);
  append_float(&buffer, "paper_white", recipe->paper_white);
  append(&buffer, "nr_sensor=%g\nnr_chroma=%g\nnr_grain=%g\nsharpen=%g\n",
         detail->nr_sensor, detail->nr_chroma, detail->nr_grain,
         detail->sharpen);
  append(&buffer, "space=srgb\ngamut=soft\n");
  return (int32_t)buffer.length;
}

int32_t ft_print_share_frame(
  const ft_emulsion* emulsion, const ft_print_roll* roll,
  const ft_print_frame* frame, ft_cmyd clicks, int32_t rotation_degrees,
  const ft_image* image, const ft_share_sidecar* sidecar, int32_t frame_number,
  uint8_t** jpeg, size_t* jpeg_length, char text[FT_SHARE_TEXT_BYTES],
  ft_share_print_info* info, ft_err* error) {
  const ft_look_detail* detail = &ft_share_detail;
  *jpeg = NULL;
  *jpeg_length = 0;
  memset(info, 0, sizeof *info);
  ft_image cut;
  ft_image turned;
  ft_init_image(&cut);
  ft_init_image(&turned);
  if(rotation_degrees != 0 &&
     rotation_degrees != 90 &&
     rotation_degrees != 180 &&
     rotation_degrees != 270) {
    return ft_fail(error, "rotate: %d is not 0, 90, 180 or 270",
                   (int)rotation_degrees);
  }
  ft_recipe recipe;
  ft_print_auto automatic;
  if(ft_make_print_recipe(emulsion, roll, frame, clicks, &recipe, &automatic,
                          &info->was_clamped, error) != 0) {
    return -1;
  }
  /* ~380 KB: too big for the stack */
  ft_print_render* render = malloc(sizeof *render);
  if(render == NULL) return ft_fail(error, "out of memory");
  int32_t result = -1;
  if(ft_compile_print(emulsion, &recipe, render, error) != 0) goto cleanup;

  /* the frame box from the sidecar, else the whole image */
  int32_t box[4] = {0, 0, (int32_t)image->width, (int32_t)image->height};
  if(sidecar != NULL) memcpy(box, sidecar->box, sizeof box);
  if(ft_crop_image(image, box, &cut, error) != 0 ||
     ft_denoise_share_frame(image, sidecar, &cut, detail, &info->noise_notes,
                            error) != 0) {
    goto cleanup;
  }
  ft_apply_print(render, cut.pixels, cut.pixels,
                 (size_t)cut.width * cut.height);
  if(rotation_degrees != 0) {
    if(ft_rotate_image(&cut, rotation_degrees, &turned, error) != 0) {
      goto cleanup;
    }
    ft_free_image(&cut);
    cut = turned;
    ft_init_image(&turned);
  }
  int32_t length = ft_format_share_print_text(text, FT_SHARE_TEXT_BYTES,
                                              emulsion, frame_number, clicks,
                                              &automatic, &recipe, detail);
  if(rotation_degrees != 0 && length >= 0 && length < FT_SHARE_TEXT_BYTES) {
    length += snprintf(text + length, FT_SHARE_TEXT_BYTES - (size_t)length,
                       "rotate=%d\n", (int)rotation_degrees);
  }
  if(length >= FT_SHARE_TEXT_BYTES) {
    ft_fail(error, "print text too long");
    goto cleanup;
  }
  double scale = sidecar != NULL ? sidecar->export_scale : 1;
  if(scale > 1) scale = 1;
  result = ft_finish_share_jpeg(
    &cut, scale, detail->sharpen, FT_SPACE_SRGB, FT_GAMUT_SOFT, FT_JPEG_QUALITY,
    text, jpeg, jpeg_length, &info->width, &info->height, error);

cleanup:
  ft_free_image(&cut);
  ft_free_image(&turned);
  free(render);
  return result;
}

int32_t ft_measure_share_balance(const ft_emulsion* emulsion,
                                 const float base[3], const ft_image* proxy,
                                 ft_print_roll* roll, ft_print_frame* frame,
                                 ft_err* error) {
  const ft_image* one[1] = {proxy};
  return ft_measure_print_roll(emulsion, base, one, 1, frame, roll, error);
}

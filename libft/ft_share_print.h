/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Scan to share as a print, after the working raw: the steps ftdev print
 * (and export, stock) and Quixote share, so both make the same
 * JPEG bytes. Pure (heap, no files), so it runs in the browser; the callers
 * read the DNG and sidecar and write the results.
 *
 *   one frame: recipe (ft_make_print_recipe: roll, frame, clicks), compile,
 *   crop the DNG image to the sidecar's box, noise reduction, the print,
 *   the turn (rotate), resize to the share size, output sharpening,
 *   output colour (sRGB, soft gamut), 8-bit, JPEG (q90, ICC, the print
 *   text as its comment). */
#ifndef FT_SHARE_PRINT_H
#define FT_SHARE_PRINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ft.h"
#include "ft_image.h"
#include "ft_look.h"
#include "ft_output.h"
#include "ft_print.h"
#include "ft_share_cut.h"

/* the share's noise reduction and sharpening (the export defaults) */
extern const ft_look_detail ft_share_detail;

/* Write the shortest %g (6..9 digits) that reads back as value into
 * text. Returns text. */
const char* ft_format_float(float value, char* text, size_t capacity);

/* ft_denoise_share_frame's notes (bits) */
/* the sidecar's profile differs from the DNG's: the DNG's was used */
#define FT_NOISE_NOTE_DISAGREE 1
/* no profile: measured on the pixels */
#define FT_NOISE_NOTE_ESTIMATED 2

/* Noise reduction on cut (transmittance, before the print) as export
 * does it: sensor NR with the profile of image (the whole frame's DNG),
 * else the sidecar's (NULL: none), else estimated; then grain NR.
 * notes (may be NULL) receives FT_NOISE_NOTE_* bits.
 * Returns 0, or -1 with error filled when memory runs out. */
int32_t ft_denoise_share_frame(const ft_image* image,
                               const ft_share_sidecar* sidecar, ft_image* cut,
                               const ft_look_detail* detail, uint32_t* notes,
                               ft_err* error);

/* The share's tail: resize cut (rendered: linear Rec.2020) by scale
 * (never up), output sharpening, output colour, 8-bit, a JPEG with its
 * ICC profile and text (NULL: none) as its comment, density from cut's
 * ppi.
 *
 * jpeg, jpeg_length  receive the JPEG, malloc'd (the caller frees it)
 * width, height      receive its size; may be NULL
 *
 * Returns 0, or -1 with error filled. */
int32_t ft_finish_share_jpeg(const ft_image* cut, double scale, double sharpen,
                             ft_space space, ft_gamut gamut, int32_t quality,
                             const char* text, uint8_t** jpeg,
                             size_t* jpeg_length, uint32_t* width,
                             uint32_t* height, ft_err* error);

/* The stats proxy of a cut frame (ft_get_print_proxy_size, linear
 * resize); proxy is ft_init_image'd by the caller.
 * Returns 0, or -1 with error filled. */
int32_t ft_make_share_proxy(const ft_image* cut, ft_image* proxy,
                            ft_err* error);

/* Per-frame balance: the frame's own range and balance, measured from
 * its stats proxy alone (a roll of one frame), so a frame prints the
 * same whatever else is on the strip and the preview predicts the share.
 * base: the film base (negative, b&w) or black (slide). roll and frame
 * go to ft_print_share_frame. Returns 0, or -1 with error filled. */
int32_t ft_measure_share_balance(const ft_emulsion* emulsion,
                                 const float base[3], const ft_image* proxy,
                                 ft_print_roll* roll, ft_print_frame* frame,
                                 ft_err* error);

/* How the print was made: the .txt beside the JPEG and its comment.
 * frame_number < 0: no frame line. As snprintf: the full length is
 * returned. */
int32_t ft_format_share_print_text(char* text, size_t capacity,
                                   const ft_emulsion* emulsion,
                                   int32_t frame_number, ft_cmyd clicks,
                                   const ft_print_auto* automatic,
                                   const ft_recipe* recipe,
                                   const ft_look_detail* detail);

/* What ft_print_share_frame did. */
typedef struct ft_share_print_info {
  /* the filtration was at its limit (ft_make_print_recipe) */
  bool was_clamped;
  /* FT_NOISE_NOTE_* bits */
  uint32_t noise_notes;
  /* the JPEG's size */
  uint32_t width;
  uint32_t height;
} ft_share_print_info;

/* room for the print text */
#define FT_SHARE_TEXT_BYTES 4096

/* Print one frame.
 *
 * image             the frame's working raw (the whole DNG image)
 * sidecar           its sidecar; NULL: none (the whole image at full
 *                   size)
 * frame_number      its number; -1 = unknown
 * rotation_degrees  the print's turn, clockwise: 0, 90, 180, 270
 *                   (ft_print_file.h), applied after the print and before
 *                   the share's tail
 * jpeg, jpeg_length receive the JPEG, malloc'd
 * text              receives the print text ("rotate=N" last when
 *                   turned)
 *
 * Returns 0, or -1 with error filled. */
int32_t ft_print_share_frame(
  const ft_emulsion* emulsion, const ft_print_roll* roll,
  const ft_print_frame* frame, ft_cmyd clicks, int32_t rotation_degrees,
  const ft_image* image, const ft_share_sidecar* sidecar, int32_t frame_number,
  uint8_t** jpeg, size_t* jpeg_length, char text[FT_SHARE_TEXT_BYTES],
  ft_share_print_info* info, ft_err* error);

#endif

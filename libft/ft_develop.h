/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Develop: a scan as the scanner sent it (<file>, .cal, .faul, .txt) ->
 * an ft_image ready for any writer (ft_out.h). The stages, in order:
 *
 *   ft_split_calibration + ft_apply_flat_field   dark/white correction
 *                                                (ft_calib.h)
 *   ft_repair_defects    Faul Pixs table + the user's defect map
 *   ft_align_rgb_rows    realign the CCD's three colour rows
 *   ft_crop_and_orient   crop, lines along x (as FlexColor saves)
 *
 * ft_develop runs them all; each can be called on its own, so a new stage
 * (dust, film base, multi-pass averaging) slots in between without
 * touching the writers. */
#ifndef FT_DEVELOP_H
#define FT_DEVELOP_H

#include <stddef.h>
#include <stdint.h>

#include "ft.h"
#include "ft_image.h"

/* the most entries a defect map holds */
#define FT_DEFECT_MAX 64

/* the channel bits of a defect */
#define FT_DEFECT_RED 1
#define FT_DEFECT_GREEN 2
#define FT_DEFECT_BLUE 4
#define FT_DEFECT_ALL 7

/* Defective CCD pixels the scanner's Faul Pixs table misses, in CCD
 * pixels (as Faul Pixs is, so one map serves every ppi and bin). */
typedef struct ft_defects {
  /* entries in ccd_pixel and channels */
  uint32_t count;
  /* the defective CCD pixel */
  int32_t ccd_pixel[FT_DEFECT_MAX];
  /* the channels it is defective in: FT_DEFECT_RED | _GREEN | _BLUE */
  uint8_t channels[FT_DEFECT_MAX];
} ft_defects;

/* Read a defect map: one defect per line, "<ccd pixel> [channels]",
 * channels any of r, g, b (default all three); '#' starts a comment.
 * Returns 0, or -1 with error filled. */
int32_t ft_parse_defects(const char* text, size_t text_length,
                         ft_defects* defects, ft_err* error);

/* How to develop a scan. */
typedef struct ft_develop_options {
  /* the scan's first output pixel across */
  int32_t first_pixel;
  /* CCD pixels per output pixel */
  int32_t bin;
  /* The crop in the RGB-aligned scan (the .txt crop_* keys): x along the
   * CCD line, y along the strip. crop_width <= 0 keeps the whole scan. */
  int32_t crop_x;
  int32_t crop_y;
  int32_t crop_width;
  int32_t crop_height;
  /* the user's defect map; NULL = none */
  const ft_defects* defects;
} ft_develop_options;

/* Interpolate the Faul Pixs entries and the user's defects.
 *
 * transmittance  lines x pixels x 3 values, repaired in place
 * faulty_dump    the Faul Pixs dump, faulty_dump_length bytes; may be NULL
 * defects        the user's defects; may be NULL
 * first_pixel    the scan's first output pixel across
 *
 * Returns 0, or -1 with error filled. */
int32_t ft_repair_defects(float* transmittance, uint32_t lines, uint32_t pixels,
                          const char* faulty_dump, size_t faulty_dump_length,
                          const ft_defects* defects, int32_t first_pixel,
                          int32_t bin, ft_err* error);

/* Copy the crop of an RGB-aligned scan into image, turned: image row =
 * CCD pixel, column = scan line. Allocates image's pixels.
 * Returns 0, or -1 with error filled when the crop is outside the scan. */
int32_t ft_crop_and_orient(const float* transmittance, uint32_t lines,
                           uint32_t pixels, const ft_develop_options* options,
                           ft_image* image, ft_err* error);

/* The stages before the crop (flat-field, defects, RGB alignment).
 *
 * raw                 lines x pixels x 3 values
 * calibration         calibration_lines x pixels x 3 values
 * options             the crop is ignored
 * aligned_lines       receives the lines of the result (the alignment
 *                     drops 12 / bin at each end)
 *
 * Returns a lines x pixels x 3 transmittance image (the caller frees
 * it), or NULL with error filled. */
float* ft_develop_transmittance(const uint16_t* raw, uint32_t lines,
                                uint32_t pixels, const uint16_t* calibration,
                                uint32_t calibration_lines,
                                const char* faulty_dump,
                                size_t faulty_dump_length,
                                const ft_develop_options* options,
                                uint32_t* aligned_lines, ft_err* error);

/* All stages. image: ft_init_image'd by the caller (its metadata is
 * kept); its pixels are allocated here. Returns 0, or -1 with error
 * filled. */
int32_t ft_develop(const uint16_t* raw, uint32_t lines, uint32_t pixels,
                   const uint16_t* calibration, uint32_t calibration_lines,
                   const char* faulty_dump, size_t faulty_dump_length,
                   const ft_develop_options* options, ft_image* image,
                   ft_err* error);

#endif

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Develop: a scan as the scanner sent it (<file>, .cal, .faul, .txt) ->
 * an ft_image ready for any writer (ft_out.h). The stages, in order:
 *
 *   ft_split_cal + ft_flatfield   dark/white correction (ft_calib.h)
 *   defects                       Faul Pixs table + the user's defect map
 *   ft_align_rgb                  tri-linear row realignment
 *   ft_crop_orient                crop, lines along x (as FlexColor saves)
 *
 * ft_develop runs them all; each is callable on its own, so a new stage
 * (dust, film base, multi-pass averaging) slots in between without
 * touching the writers. */
#ifndef FT_DEVELOP_H
#define FT_DEVELOP_H
#include <stddef.h>

#include "ft.h"
#include "ft_image.h"

#define FT_DEFECT_MAX 64

/* Defective CCD pixels the scanner's Faul Pixs table misses, in CCD
 * pixels (as Faul Pixs is, so one map serves every ppi and bin). */
typedef struct {
  int n;
  int ccd[FT_DEFECT_MAX];
  unsigned char ch[FT_DEFECT_MAX]; /* bit 0 R, 1 G, 2 B */
} ft_defects;

/* Text, one defect per line: "<ccd pixel> [channels]", channels any of
 * r, g, b (default all three); '#' starts a comment. */
int ft_defects_parse(const char* text, size_t n, ft_defects* d, ft_err* e);

typedef struct {
  int x, bin; /* the scan's first output pixel; CCD px each */
  /* crop in the RGB-aligned scan (the .txt crop_* keys): x along the CCD
   * line, y along the strip; w <= 0 keeps the whole scan */
  int crop_x, crop_y, crop_w, crop_h;
  const ft_defects* defects; /* NULL: none */
} ft_develop_opts;

/* Interpolate the Faul Pixs entries (faul, n bytes, may be NULL) and the
 * user's defects; -1 with e filled on failure. */
int ft_fix_defects(float* t, int lines, int px, const char* faul, size_t n,
                   const ft_defects* d, int x, int bin, ft_err* e);

/* Copy the crop of t (lines x px, RGB-aligned) into im, transposed: image
 * row = CCD pixel, column = scan line. Allocates im's pixels. */
int ft_crop_orient(const float* t, int lines, int px, const ft_develop_opts* o,
                   ft_image* im, ft_err* e);

/* The stages before the crop (flat-field, defects, RGB alignment): a
 * lines x px x 3 transmittance image, line-major, *lines_out lines (the
 * alignment drops 12 / bin at each end). Caller frees; NULL with e filled
 * on failure. o's crop is ignored. */
float* ft_develop_t(const unsigned short* raw, int lines, int px,
                    const unsigned short* cal, int cal_lines, const char* faul,
                    size_t faul_n, const ft_develop_opts* o, int* lines_out,
                    ft_err* e);

/* All stages. im: ft_image_init'd by the caller (its metadata is kept);
 * pixels are allocated here. raw: lines x px x 3, cal: cal_lines x px x 3. */
int ft_develop(const unsigned short* raw, int lines, int px,
               const unsigned short* cal, int cal_lines, const char* faul,
               size_t faul_n, const ft_develop_opts* o, ft_image* im,
               ft_err* e);

#endif

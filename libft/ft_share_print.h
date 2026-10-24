/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Scan to share as a print, after the working raw: the steps ftdev print
 * (and export, stock) and the web page share, so both make the same
 * JPEG bytes. Pure (browser tier: heap, no files); the callers read the
 * DNG and sidecar and write the results.
 *
 *   one frame: recipe (ft_print_recipe: roll, frame, clicks), compile,
 *   crop the DNG image to the sidecar's box, noise reduction, the print,
 *   the turn (rotate), resize to the share size, output sharpening,
 *   output colour (sRGB, soft gamut), 8-bit, JPEG (q90, ICC, the print
 *   text as its comment). */
#ifndef FT_SHARE_PRINT_H
#define FT_SHARE_PRINT_H
#include <stddef.h>

#include "ft.h"
#include "ft_image.h"
#include "ft_look.h"
#include "ft_output.h"
#include "ft_print.h"
#include "ft_share_cut.h"

/* the share's noise reduction and sharpening (the export defaults) */
extern const ft_look_detail ft_share_detail;

/* the shortest %g (6..9 digits) that reads back as v; buf */
const char* ft_fmtf(float v, char* buf, size_t cap);

/* ft_share_denoise's notes (bits) */
#define FT_NR_NOTE_DISAGREE                                              \
  1                            /* the sidecar's profile differs from the \
                                  DNG's: the DNG's used */
#define FT_NR_NOTE_ESTIMATED 2 /* no profile: measured on the pixels */

/* noise reduction on cut (transmittance, before the print) as export
 * does it: sensor NR with the profile of im (the whole frame's DNG),
 * else the sidecar's (sc NULL: none), else estimated; then grain NR.
 * *notes (NULL ok) gets FT_NR_NOTE_*. -1 with e filled if out of
 * memory. */
int ft_share_denoise(const ft_image* im, const ft_share_sidecar* sc,
                     ft_image* cut, const ft_look_detail* d, int* notes,
                     ft_err* e);

/* the share tail: resize cut (rendered: linear Rec.2020) by scale (never
 * up), output sharpening, output colour, 8-bit, a JPEG with its ICC
 * profile and text (NULL: none) as its comment, density from cut's ppi.
 * *jpg malloc'd (the caller frees it), *n bytes, *ow x *oh px. -1 with e
 * filled. */
int ft_share_finish(const ft_image* cut, double scale, double sharpen,
                    ft_space space, ft_gamut gamut, int quality,
                    const char* text, unsigned char** jpg, size_t* n, int* ow,
                    int* oh, ft_err* e);

/* the stats proxy of a cut frame (ft_print_proxy_dims, linear resize);
 * proxy ft_image_init'd by the caller */
int ft_share_print_proxy(const ft_image* cut, ft_image* proxy, ft_err* e);

/* per-frame balance: the frame's own range and balance, measured from
 * its stats proxy alone (a roll of one frame), so a frame prints the
 * same whatever else is on the strip and the preview predicts the
 * share. base: the film base (neg, b&w) or black (slide). roll and pf
 * go to ft_share_print_frame. -1 with e filled. */
int ft_share_print_balance(const ft_emulsion* em, const float base[3],
                           const ft_image* proxy, ft_print_roll* roll,
                           ft_print_frame* pf, ft_err* e);

/* how the print was made: the .txt beside the JPEG and its comment.
 * frame < 0: no frame line. snprintf's contract. */
int ft_share_print_text(char* buf, size_t cap, const ft_emulsion* em, int frame,
                        ft_cmyd k, const ft_print_auto* a, const ft_recipe* r,
                        const ft_look_detail* d);

typedef struct {
  int clamped;  /* filtration at its limit (ft_print_recipe) */
  int nr_notes; /* FT_NR_NOTE_* */
  int w, h;     /* the JPEG's size */
} ft_share_print_info;

#define FT_SHARE_TEXT_MAX 4096

/* one frame: im is the frame's working raw (the whole DNG image), sc its
 * sidecar (NULL: none; then the whole image at full size), frame its
 * number (-1 unknown), rotate the print's turn (degrees clockwise: 0, 90,
 * 180, 270; ft_print_file.h), applied after the print and before the
 * share tail. The JPEG into *jpg (malloc'd, *n bytes), the print text
 * into text (FT_SHARE_TEXT_MAX bytes; "rotate=N" last when turned). -1
 * with e filled. */
int ft_share_print_frame(const ft_emulsion* em, const ft_print_roll* roll,
                         const ft_print_frame* pf, ft_cmyd k, int rotate,
                         const ft_image* im, const ft_share_sidecar* sc,
                         int frame, unsigned char** jpg, size_t* n,
                         char text[FT_SHARE_TEXT_MAX],
                         ft_share_print_info* info, ft_err* e);

#endif

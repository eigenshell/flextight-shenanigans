/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ftdev's render flags (--film --base --exposure --tint --grade --curve),
 * shared by the develop path (-f positive) and ftdev export. */
#ifndef RENDER_FLAGS_H
#define RENDER_FLAGS_H
#include "ft_image.h"
#include "ft_look.h"
#include "ft_render.h"

typedef struct {
  int film; /* ft_film, -1 = not given */
  int bw;
  int has_base, has_exp, has_tint, has_grade;
  float base[3], tint[3], exp, grade;
  const char* curve;
} rflags;

void rflags_init(rflags* f);
/* argv[*i] a render flag? 1 = yes (*i moved past its value), 0 = not a
 * render flag, -1 = bad value (message printed: exit 2) */
int rflags_parse(rflags* f, int argc, char** argv, int* i);
int rflags_any(const rflags* f);
/* the recipe before the render: mode from film, curve, grade, a given
 * base; validated. film must be set. -1 = message printed (exit 2) */
int rflags_recipe(const rflags* f, ft_recipe* r);
/* render im in place: base from the flags, else base_d (preview film
 * base; NULL = none: estimated from the frame, with a note), auto range
 * and exposure, then the flags' exposure and tint; prints the recipe as
 * flags to paste back. 0 ok, 1 error, 2 bad recipe (messages printed). */
int rflags_render(const rflags* f, ft_recipe* r, const double* base_d,
                  ft_image* im, ft_render* g);
/* the render mode of the flags' film (film must be set) */
ft_render_mode rflags_mode(const rflags* f);
/* render im in place from a roll edit's stock (NULL: none) and a frame's
 * resolved taste (ft_look.h); base_d as rflags_render. 0 ok, 1 error, 2
 * bad recipe (messages printed). */
int look_render(const ft_stock* s, const ft_taste* t, ft_render_mode m,
                const double* base_d, ft_image* im, ft_recipe* r, ft_render* g);
/* the resolved recipe as key=value lines (the export's .txt and JPEG
 * comment): look name, frame (< 0: none), the recipe, the curves' text
 * (NULL: linear / not set), the detail settings. Returns snprintf's
 * length. */
int recipe_text(char* buf, size_t cap, const char* look, int frame,
                const ft_recipe* r, const char* const curve[4],
                const ft_look_detail* d);

/* v as the shortest "%g" that reads back as the same float */
const char* fmtf(float v, char* buf, size_t cap);
#endif

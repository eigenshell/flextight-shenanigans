/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "render_flags.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_render_auto.h"
#include "ft_share_print.h"

/* "a,b,c" -> v; -1 if malformed or not finite */
static int parse3(const char* s, float v[3]) {
  char extra;
  if(sscanf(s, "%f,%f,%f%c", &v[0], &v[1], &v[2], &extra) != 3) return -1;
  return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]) ? 0 : -1;
}

/* a finite number and nothing else -> v; -1 otherwise */
static int parse1(const char* s, float* v) {
  char* end;
  double d = strtod(s, &end);
  if(end == s || *end || !isfinite(d)) return -1;
  *v = (float)d;
  return 0;
}

/* the shortest %g text of v that reads back as the same float */
const char* fmtf(float v, char* buf, size_t cap) {
  return ft_format_float(v, buf, cap);
}

void rflags_init(rflags* f) {
  memset(f, 0, sizeof *f);
  f->film = -1;
}

int rflags_any(const rflags* f) {
  return f->has_base ||
         f->has_exp ||
         f->has_tint ||
         f->has_grade ||
         f->curve != NULL;
}

int rflags_parse(rflags* f, int argc, char** argv, int* i) {
  const char* a = argv[*i];
  if(*i + 1 >= argc) return 0;
  if(strcmp(a, "--film") == 0) {
    const char* v = argv[++*i];
    if(strcmp(v, "neg") == 0 || strcmp(v, "negative") == 0)
      f->film = FT_FILM_NEGATIVE;
    else if(strcmp(v, "slide") == 0)
      f->film = FT_FILM_SLIDE;
    else if(strcmp(v, "bw") == 0) {
      f->film = FT_FILM_NEGATIVE;
      f->bw = 1;
    } else {
      fprintf(stderr, "ftdev: --film is neg, slide or bw\n");
      return -1;
    }
  } else if(strcmp(a, "--base") == 0) {
    if(parse3(argv[++*i], f->base)) {
      fprintf(stderr, "ftdev: --base is r,g,b (densities)\n");
      return -1;
    }
    f->has_base = 1;
  } else if(strcmp(a, "--tint") == 0) {
    if(parse3(argv[++*i], f->tint)) {
      fprintf(stderr, "ftdev: --tint is r,g,b (CC)\n");
      return -1;
    }
    f->has_tint = 1;
  } else if(strcmp(a, "--exposure") == 0) {
    if(parse1(argv[++*i], &f->exp)) {
      fprintf(stderr, "ftdev: --exposure is stops\n");
      return -1;
    }
    f->has_exp = 1;
  } else if(strcmp(a, "--grade") == 0) {
    if(parse1(argv[++*i], &f->grade)) {
      fprintf(stderr, "ftdev: --grade is a number\n");
      return -1;
    }
    f->has_grade = 1;
  } else if(strcmp(a, "--curve") == 0) {
    f->curve = argv[++*i];
  } else {
    return 0;
  }
  return 1;
}

ft_render_mode rflags_mode(const rflags* f) {
  return f->bw                      ? FT_RENDER_BW
         : f->film == FT_FILM_SLIDE ? FT_RENDER_SLIDE
                                    : FT_RENDER_NEGATIVE;
}

int rflags_recipe(const rflags* f, ft_recipe* r) {
  ft_err e;
  ft_init_recipe(r, rflags_mode(f));
  if(f->curve &&
     ft_find_curve_preset(f->curve, &r->master) != 0 &&
     ft_parse_curve(f->curve, &r->master, &e) != 0) {
    const char* m = e.message;
    int k;
    fprintf(stderr, "ftdev: --curve: not a preset (");
    for(k = 0; ft_get_curve_preset_name(k); k++)
      fprintf(stderr, "%s%s", k ? " " : "", ft_get_curve_preset_name(k));
    fprintf(stderr, ") and not points: %s\n",
            strncmp(m, "curve: ", 7) == 0 ? m + 7 : m);
    return -1;
  }
  if(f->has_grade)
    r->grade = f->grade; /* before auto: exposure depends on it */
  if(f->has_exp) r->exposure = f->exp;
  if(f->has_tint) memcpy(r->tint, f->tint, sizeof r->tint);
  if(f->has_base) /* auto never changes the base */
    ft_set_recipe_base(r, f->base);
  if(ft_validate_recipe(r, &e)) { /* before any file is written */
    fprintf(stderr, "ftdev: %s\n", e.message);
    return -1;
  }
  return 0;
}

static const char* NO_BASE =
  "ftdev: note: no film_base given; "
  "film base estimated from the frame itself\n";

int rflags_render(const rflags* f, ft_recipe* r, const double* base_d,
                  ft_image* im, ft_render* g) {
  const ft_image* one[1];
  float d[3];
  ft_err e;
  one[0] = im;
  if(f->has_base) {
    memcpy(d, f->base, sizeof d);
  } else if(base_d) {
    d[0] = (float)base_d[0]; /* from the frames preview */
    d[1] = (float)base_d[1];
    d[2] = (float)base_d[2];
  } else {
    if(ft_estimate_base_from_frames(one, 1, r->mode, d, &e)) {
      fprintf(stderr, "ftdev: %s\n", e.message);
      return 1;
    }
    fputs(NO_BASE, stderr);
  }
  ft_set_recipe_base(r, d);
  if(ft_estimate_recipe(one, 1, r, &e)) {
    fprintf(stderr, "ftdev: %s\n", e.message);
    return 1;
  }
  if(f->has_exp) r->exposure = f->exp;
  if(f->has_tint) memcpy(r->tint, f->tint, sizeof r->tint);
  if(ft_compile_render(r, g, &e)) {
    fprintf(stderr, "ftdev: %s\n", e.message);
    return 2;
  }
  ft_render_image(g, im);
  {
    char s[10][32];
    printf(
      "recipe: --film %s --base %s,%s,%s --exposure %s "
      "--tint %s,%s,%s --grade %s --curve \"%s\"\n",
      f->bw                      ? "bw"
      : f->film == FT_FILM_SLIDE ? "slide"
                                 : "neg",
      fmtf(r->base_density[0], s[0], 32), fmtf(r->base_density[1], s[1], 32),
      fmtf(r->base_density[2], s[2], 32), fmtf(r->exposure, s[3], 32),
      fmtf(r->tint[0], s[4], 32), fmtf(r->tint[1], s[5], 32),
      fmtf(r->tint[2], s[6], 32), fmtf(r->grade, s[7], 32),
      f->curve ? f->curve : "linear");
  }
  return 0;
}

int look_render(const ft_stock* s, const ft_taste* t, ft_render_mode m,
                const double* base_d, ft_image* im, ft_recipe* r,
                ft_render* g) {
  const ft_image* one[1];
  float d[3];
  ft_err e;
  one[0] = im;
  if(base_d) {
    d[0] = (float)base_d[0];
    d[1] = (float)base_d[1];
    d[2] = (float)base_d[2];
  } else {
    fputs(NO_BASE, stderr);
  }
  if(ft_resolve_recipe(s, t, m, base_d ? d : NULL, one, 1, r, &e)) {
    fprintf(stderr, "ftdev: %s\n", e.message);
    return strncmp(e.message, "recipe:", 7) == 0 ? 2 : 1;
  }
  if(ft_compile_render(r, g, &e)) {
    fprintf(stderr, "ftdev: %s\n", e.message);
    return 2;
  }
  ft_render_image(g, im);
  return 0;
}

int recipe_text(char* buf, size_t cap, const char* look, int frame,
                const ft_recipe* r, const char* const curve[4],
                const ft_look_detail* d) {
  static const char* CK[4] = {"curve", "curve_r", "curve_g", "curve_b"};
  char s[3][32];
  size_t n = 0;
  int k, w;
#define APP(...)                                                               \
  do {                                                                         \
    w =                                                                        \
      snprintf(buf + (n < cap ? n : cap), n < cap ? cap - n : 0, __VA_ARGS__); \
    if(w > 0) n += (size_t)w;                                                  \
  } while(0)
#define F3(v) \
  fmtf((v)[0], s[0], 32), fmtf((v)[1], s[1], 32), fmtf((v)[2], s[2], 32)
  APP("look=%s\n", look);
  if(frame >= 0) APP("frame=%d\n", frame);
  APP("film=%s\n", r->mode == FT_RENDER_BW      ? "bw"
                   : r->mode == FT_RENDER_SLIDE ? "slide"
                                                : "neg");
  APP("base=%s %s %s\n", F3(r->base_density));
  APP("range=%s %s %s\n", F3(r->range_density));
  APP("exposure=%s\n", fmtf(r->exposure, s[0], 32));
  APP("tint=%s %s %s\n", F3(r->tint));
  APP("grade=%s\n", fmtf(r->grade, s[0], 32));
  APP("paper_black=%s\n", fmtf(r->paper_black, s[0], 32));
  APP("paper_white=%s\n", fmtf(r->paper_white, s[0], 32));
  if(r->mode == FT_RENDER_BW) APP("bw_weights=%s %s %s\n", F3(r->bw_weights));
  APP("curve=%s\n", curve[0] ? curve[0] : "linear");
  for(k = 1; k < 4; k++)
    if(curve[k]) APP("%s=%s\n", CK[k], curve[k]);
  APP("nr_sensor=%g\nnr_chroma=%g\nnr_grain=%g\nsharpen=%g\n", d->nr_sensor,
      d->nr_chroma, d->nr_grain, d->sharpen);
#undef F3
#undef APP
  return (int)n;
}

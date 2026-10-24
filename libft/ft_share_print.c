/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_share_print.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_emulsion.h"
#include "ft_icc.h"
#include "ft_jpeg.h"
#include "ft_nr.h"
#include "ft_resize.h"
#include "ft_sharpen.h"

const ft_look_detail ft_share_detail = {1, 2, 0, 0.5};

const char* ft_fmtf(float v, char* buf, size_t cap) {
  int p;
  for (p = 6; p < 9; p++) {
    snprintf(buf, cap, "%.*g", p, (double)v);
    if (strtof(buf, NULL) == v) return buf;
  }
  snprintf(buf, cap, "%.9g", (double)v);
  return buf;
}

/* the profile for sensor NR: the DNG's NoiseProfile, else the
 * sidecar's, else measured on the pixels */
static void nr_profile(const ft_image* im, const ft_share_sidecar* sc,
                       const ft_image* cut, double prof[6], void* scratch,
                       int* notes) {
  int i;
  if (im->has_noise) {
    memcpy(prof, im->noise, 6 * sizeof *prof);
    if (sc && sc->has_noise)
      for (i = 0; i < 6; i++)
        if (fabs(sc->noise[i] - prof[i]) > 1e-6 * fabs(prof[i])) {
          *notes |= FT_NR_NOTE_DISAGREE;
          break;
        }
  } else if (sc && sc->has_noise) {
    memcpy(prof, sc->noise, 6 * sizeof *prof);
  } else {
    ft_nr_estimate(cut->rgb, cut->w, cut->h, prof, scratch);
    *notes |= FT_NR_NOTE_ESTIMATED;
  }
}

int ft_share_denoise(const ft_image* im, const ft_share_sidecar* sc,
                     ft_image* cut, const ft_look_detail* d, int* notes,
                     ft_err* e) {
  void* nrs;
  int nn = 0;
  if (notes) *notes = 0;
  if (!(d->nr_sensor > 0) && !(d->nr_grain > 0)) return 0;
  nrs = malloc(ft_nr_scratch_bytes(cut->w, cut->h));
  if (!nrs) return ft_fail(e, "out of memory");
  if (d->nr_sensor > 0) {
    double prof[6];
    nr_profile(im, sc, cut, prof, nrs, &nn);
    ft_nr_sensor(cut->rgb, cut->w, cut->h, prof, d->nr_sensor,
                 d->nr_sensor * d->nr_chroma, nrs);
  }
  if (d->nr_grain > 0) ft_nr_grain(cut->rgb, cut->w, cut->h, d->nr_grain, nrs);
  free(nrs);
  if (notes) *notes = nn;
  return 0;
}

int ft_share_finish(const ft_image* cut, double scale, double sharpen,
                    ft_space space, ft_gamut gamut, int quality,
                    const char* text, unsigned char** jpg, size_t* n,
                    int* ow_out, int* oh_out, ft_err* e) {
  float *small = NULL, *scratch = NULL;
  unsigned char *px8 = NULL, *icc = NULL;
  ft_output* out = NULL;
  size_t icc_n;
  int ow, oh, i, rc = -1;

  *jpg = NULL;
  *n = 0;
  ft_resize_dims(cut->w, cut->h, scale, &ow, &oh);
  small = malloc((size_t)ow * oh * 3 * sizeof *small);
  scratch = malloc(ft_resize_scratch_bytes(cut->w, cut->h, ow, oh));
  px8 = malloc((size_t)ow * oh * 3);
  out = malloc(sizeof *out);
  icc = malloc(FT_ICC_MAX);
  if (!small || !scratch || !px8 || !out || !icc) {
    ft_fail(e, "out of memory");
    goto done;
  }
  if (ft_resize(cut->rgb, cut->w, cut->h, small, ow, oh, scratch, e) ||
      ft_output_compile(space, gamut, out, e))
    goto done;
  /* output sharpening, after the resize: its scratch reuses the resize
   * scratch when that is big enough */
  if (sharpen > 0) {
    size_t need = ft_sharpen_scratch_bytes(ow, oh);
    if (need > ft_resize_scratch_bytes(cut->w, cut->h, ow, oh)) {
      float* s2 = realloc(scratch, need);
      if (!s2) {
        ft_fail(e, "out of memory");
        goto done;
      }
      scratch = s2;
    }
    ft_sharpen(small, ow, oh, sharpen, (double)ow / cut->w, scratch);
  }
  for (i = 0; i < oh; i++) {
    float* row = small + (size_t)i * ow * 3;
    ft_output_apply(out, row, row, (size_t)ow);
    ft_output_quantise8(row, px8 + (size_t)i * ow * 3, (size_t)ow,
                        (size_t)i * ow);
  }
  icc_n = ft_icc_build(space, icc, FT_ICC_MAX);
  if (icc_n == 0) {
    ft_fail(e, "cannot build the ICC profile");
    goto done;
  }
  if (ft_jpeg_encode(px8, ow, oh, quality,
                     cut->ppi > 0 ? cut->ppi * ow / cut->w : 0, icc, icc_n,
                     text, jpg, n, e))
    goto done;
  if (ow_out) *ow_out = ow;
  if (oh_out) *oh_out = oh;
  rc = 0;
done:
  free(small);
  free(scratch);
  free(px8);
  free(out);
  free(icc);
  return rc;
}

int ft_share_print_proxy(const ft_image* cut, ft_image* proxy, ft_err* e) {
  int pw, ph;
  void* scratch;
  ft_print_proxy_dims(cut->w, cut->h, &pw, &ph);
  scratch = malloc(ft_resize_scratch_bytes(cut->w, cut->h, pw, ph));
  if (!scratch) return ft_fail(e, "out of memory");
  if (ft_image_alloc(proxy, pw, ph, e) ||
      ft_resize(cut->rgb, cut->w, cut->h, proxy->rgb, pw, ph, scratch, e)) {
    free(scratch);
    ft_image_free(proxy);
    return -1;
  }
  free(scratch);
  return 0;
}

int ft_share_print_text(char* buf, size_t cap, const ft_emulsion* em, int frame,
                        ft_cmyd k, const ft_print_auto* a, const ft_recipe* r,
                        const ft_look_detail* d) {
  char s[3][32];
  size_t n = 0;
  int w;
#define APP(...)                                         \
  do {                                                   \
    w = snprintf(buf ? buf + (n < cap ? n : cap) : NULL, \
                 n < cap ? cap - n : 0, __VA_ARGS__);    \
    if (w > 0) n += (size_t)w;                           \
  } while (0)
#define F3(v)                                           \
  ft_fmtf((v)[0], s[0], 32), ft_fmtf((v)[1], s[1], 32), \
    ft_fmtf((v)[2], s[2], 32)
  APP("emulsion=%s\n", em->id);
  APP("science=%d\n", FT_EMULSION_REV);
  APP("chain=%s %s %s %s\n", em->film->name, em->balance->name, em->print->name,
      em->output->name);
  if (frame >= 0) APP("frame=%d\n", frame);
  APP("cmyd=%d %d %d %d\n", k.c, k.m, k.y, k.d);
  APP("auto_exposure=%s\n", ft_fmtf(a->exposure, s[0], 32));
  APP("auto_tint=%s %s %s\n", F3(a->tint));
  APP("film=%s\n", r->mode == FT_RENDER_BW      ? "bw"
                   : r->mode == FT_RENDER_SLIDE ? "slide"
                                                : "neg");
  APP("base=%s %s %s\n", F3(r->base));
  APP("range=%s %s %s\n", F3(r->range));
  APP("exposure=%s\n", ft_fmtf(r->exposure, s[0], 32));
  APP("tint=%s %s %s\n", F3(r->tint));
  APP("grade=%s\n", ft_fmtf(r->grade, s[0], 32));
  APP("paper_black=%s\n", ft_fmtf(r->paper_black, s[0], 32));
  APP("paper_white=%s\n", ft_fmtf(r->paper_white, s[0], 32));
  APP("nr_sensor=%g\nnr_chroma=%g\nnr_grain=%g\nsharpen=%g\n", d->nr_sensor,
      d->nr_chroma, d->nr_grain, d->sharpen);
  APP("space=srgb\ngamut=soft\n");
#undef F3
#undef APP
  return (int)n;
}

int ft_share_print_frame(const ft_emulsion* em, const ft_print_roll* roll,
                         const ft_print_frame* pf, ft_cmyd k, int rotate,
                         const ft_image* im, const ft_share_sidecar* sc,
                         int frame, unsigned char** jpg, size_t* n,
                         char text[FT_SHARE_TEXT_MAX],
                         ft_share_print_info* info, ft_err* e) {
  const ft_look_detail* d = &ft_share_detail;
  ft_print_render* pr = NULL;
  ft_print_auto a;
  ft_recipe r;
  ft_image cut, turned;
  int box[4], t, rc = -1;
  double scale;

  *jpg = NULL;
  *n = 0;
  memset(info, 0, sizeof *info);
  ft_image_init(&cut);
  ft_image_init(&turned);
  if (rotate != 0 && rotate != 90 && rotate != 180 && rotate != 270)
    return ft_fail(e, "rotate: %d is not 0, 90, 180 or 270", rotate);
  if (ft_print_recipe(em, roll, pf, k, &r, &a, &info->clamped, e)) return -1;
  pr = malloc(sizeof *pr);
  if (!pr) return ft_fail(e, "out of memory");
  if (ft_print_compile(em, &r, pr, e)) goto done;
  if (sc) {
    memcpy(box, sc->box, sizeof box);
  } else {
    box[0] = box[1] = 0;
    box[2] = im->w;
    box[3] = im->h;
  }
  if (ft_image_crop(im, box, &cut, e) ||
      ft_share_denoise(im, sc, &cut, d, &info->nr_notes, e))
    goto done;
  ft_print_apply(pr, cut.rgb, cut.rgb, (size_t)cut.w * cut.h);
  if (rotate) {
    if (ft_image_rotate(&cut, rotate, &turned, e)) goto done;
    ft_image_free(&cut);
    cut = turned;
    ft_image_init(&turned);
  }
  t = ft_share_print_text(text, FT_SHARE_TEXT_MAX, em, frame, k, &a, &r, d);
  if (rotate && t >= 0 && t < FT_SHARE_TEXT_MAX)
    t +=
      snprintf(text + t, FT_SHARE_TEXT_MAX - (size_t)t, "rotate=%d\n", rotate);
  if (t >= FT_SHARE_TEXT_MAX) {
    ft_fail(e, "print text too long");
    goto done;
  }
  scale = sc ? sc->export_scale : 1;
  rc = ft_share_finish(&cut, scale > 1 ? 1 : scale, d->sharpen, FT_SPACE_SRGB,
                       FT_GAMUT_SOFT, FT_JPEG_QUALITY, text, jpg, n, &info->w,
                       &info->h, e);
done:
  ft_image_free(&cut);
  ft_image_free(&turned);
  free(pr);
  return rc;
}

int ft_share_print_balance(const ft_emulsion* em, const float base[3],
                           const ft_image* proxy, ft_print_roll* roll,
                           ft_print_frame* pf, ft_err* e) {
  const ft_image* one[1];
  one[0] = proxy;
  return ft_print_roll_measure(em, base, one, 1, pf, roll, e);
}

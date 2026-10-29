/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ftweb.h. Browser tier: heap and snprintf, no files. The steps follow
 * tools/ftdev (cut: share.c ftdev_cut; print: print.c ftdev_print) call
 * for call, with the files replaced by what they would hold. */
#include "ftweb.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_calib.h"
#include "ft_develop.h"
#include "ft_dmax.h"
#include "ft_dng.h"
#include "ft_emulsion.h"
#include "ft_focus.h"
#include "ft_holder.h"
#include "ft_job.h"
#include "ft_output.h"
#include "ft_plan.h"
#include "ft_pm.h"
#include "ft_preview.h"
#include "ft_print.h"
#include "ft_print_file.h"
#include "ft_render_auto.h"
#include "ft_resize.h"
#include "ft_share.h"
#include "ft_share_cut.h"
#include "ft_share_print.h"
#include "ft_stream_detect.h"
#include "ft_zip.h"

/* a landed frame, as ftdev cut leaves it on disk: the DNG's samples,
 * ppi and noise profile, and the sidecar as read back */
typedef struct {
  int frame, w, h;
  unsigned short* q;
  double ppi;
  int has_noise;
  double noise[6];
  ft_share_sidecar sc;
  ft_image proxy; /* the stats proxy (ftdev print's) */
} wframe;

/* a frame of the preview (strip.raw): its box cut from the developed
 * preview, x along, as a share frame is */
typedef struct {
  int frame;
  ft_image im;
  ft_image proxy;
  int measured; /* roll and pf: its own balance */
  ft_print_roll roll;
  ft_print_frame pf;
} pframe;

struct ftw {
  ft_lens lens;
  ft_job job; /* the preview's: every plan's base */
  int has_job;
  ft_job plan; /* strip.job as planned */
  int has_plan;
  ft_print_file pf;
  int has_print;
  int n;
  wframe f[FT_MAX_FRAMES]; /* by frame number */
  int measured;            /* per-frame balance: each its own */
  ft_print_roll rolls[FT_MAX_FRAMES];
  ft_print_frame pfr[FT_MAX_FRAMES];
  int npv; /* the preview's frames (ftw_preview_load) */
  pframe pv[FT_MAX_FRAMES];
  double live_white[3]; /* ftw_live_rows: the preview's white */
  /* the streamed preview (ftw_stream_*): rows calibrated as they come,
   * kept (frames are cut from them), fed to the streaming detector */
  int st_on, st_px;
  long st_x;
  ft_axes st_ax;
  float *st_dark, *st_white; /* px x 3 each */
  float* st_prev;            /* the last two flat-fielded raw lines */
  long st_raw;               /* raw lines in */
  float* st_t;               /* RGB-aligned lines: st_n of st_cap */
  long st_n, st_cap;
  int32_t st_faul[3][FT_FAULTY_PIXEL_MAX];
  uint32_t st_fcount[3];
  ft_stream_detector st_det;
  int st_grid;
  int disp_frame; /* the cached screen copy's frame, 0 none */
  int disp_edge;  /* and the long edge it was asked at */
  ft_image disp;
  unsigned char* out;
  size_t nout, cap;
  char text[FT_SHARE_TEXT_BYTES];
  char err[sizeof(((ft_err*)0)->message)];
};

static const char* TYPES[] = {"full", "half", "panorama", "partial"};

static void stream_free(ftw* w);
static int stream_recut(ftw* w, int frame, ft_err* e);

static int fail(ftw* w, const char* msg) {
  snprintf(w->err, sizeof w->err, "%s", msg);
  return -1;
}

static int fail_e(ftw* w, const ft_err* e) { return fail(w, e->message); }

/* ------------------------------------------------------------ out */

static void out_reset(ftw* w) {
  w->nout = 0;
  if(w->out) w->out[0] = 0;
}

/* take ownership of b as the out buffer */
static void out_take(ftw* w, unsigned char* b, size_t n) {
  free(w->out);
  w->out = b;
  w->nout = n;
  w->cap = n;
}

static int out_room(ftw* w, size_t more) {
  size_t need = w->nout + more + 1, c = w->cap ? w->cap : 1024;
  unsigned char* nb;
  if(need <= w->cap) return 0;
  while(c < need) c *= 2;
  nb = realloc(w->out, c);
  if(!nb) return -1;
  w->out = nb;
  w->cap = c;
  return 0;
}

#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
static int outf(ftw* w, const char* fmt, ...) {
  va_list ap;
  int k;
  va_start(ap, fmt);
  k = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if(k < 0 || out_room(w, (size_t)k)) return fail(w, "out of memory");
  va_start(ap, fmt);
  vsnprintf((char*)w->out + w->nout, (size_t)k + 1, fmt, ap);
  va_end(ap);
  w->nout += (size_t)k;
  return 0;
}

/* a JSON string's body (ids, names and messages: no control bytes but
 * newlines expected) */
static int out_str(ftw* w, const char* s) {
  int rc = outf(w, "\"");
  for(; *s && !rc; s++)
    rc = *s == '"' || *s == '\\'  ? outf(w, "\\%c", *s)
         : (unsigned char)*s < 32 ? outf(w, "\\u%04x", *s)
                                  : outf(w, "%c", *s);
  return rc ? rc : outf(w, "\"");
}

/* ------------------------------------------------------------ life */

ftw* ftw_new(void) {
  ftw* w = calloc(1, sizeof *w);
  if(w) {
    w->lens = ft_lens_default;
    ft_init_image(&w->disp);
  }
  return w;
}

static void frame_free(wframe* f) {
  free(f->q);
  ft_free_image(&f->proxy);
  memset(f, 0, sizeof *f);
}

static void preview_free(ftw* w) {
  int i;
  for(i = 0; i < w->npv; i++) {
    ft_free_image(&w->pv[i].im);
    ft_free_image(&w->pv[i].proxy);
  }
  w->npv = 0;
}

/* the preview's frames to be measured again (another emulsion) */
static void pv_unmeasure(ftw* w) {
  int i;
  for(i = 0; i < w->npv; i++) w->pv[i].measured = 0;
}

void ftw_free(ftw* w) {
  int i;
  if(!w) return;
  for(i = 0; i < w->n; i++) frame_free(&w->f[i]);
  preview_free(w);
  stream_free(w);
  ft_free_image(&w->disp);
  free(w->out);
  free(w);
}

const char* ftw_error(const ftw* w) { return w->err; }

const unsigned char* ftw_out(const ftw* w) { return w->out; }

size_t ftw_out_len(const ftw* w) { return w->nout; }

const char* ftw_text(const ftw* w) { return w->text; }

void* ftw_alloc(size_t n) { return malloc(n ? n : 1); }

void ftw_release(void* p) { free(p); }

int ftw_emulsions(ftw* w) {
  static const char* MODE[3] = {"negative", "slide", "bw"};
  const ft_emulsion* em;
  int i, rc;
  out_reset(w);
  rc = outf(w, "[");
  for(i = 0; !rc && (em = ft_get_emulsion_at(i)) != NULL; i++) {
    rc = outf(w, "%s{\"id\":", i ? "," : "");
    rc = rc ? rc : out_str(w, em->id);
    rc = rc ? rc : outf(w, ",\"name\":");
    rc = rc ? rc : out_str(w, em->name);
    rc = rc ? rc : outf(w, ",\"mode\":\"%s\"}", MODE[em->mode]);
  }
  return rc ? rc : outf(w, "]");
}

int ftw_resolutions(ftw* w, const double* ppi, int n) {
  /* today's optics limits: the estimator takes them as parameters */
  const ft_optics_limits lim = {FT_SCALE_MIN_PPI, FT_SCALE_MAX_PPI,
                                &ft_lens_default};
  int i, rc;
  uint32_t bins[6];
  for(i = 0; i < n; i++)
    if(!ft_find_bins_for_ppi(ppi[i], bins)) {
      snprintf(w->err, sizeof w->err, "%g ppi: out of the optics' range",
               ppi[i]);
      return -1;
    }
  out_reset(w);
  rc = outf(w, "[");
  for(i = 0; i < n && !rc; i++) {
    double in = FT_SHARE_FULL_FRAME_LONG_MM / 25.4,
           mp = ppi[i] *
                in *
                ppi[i] *
                in *
                24.0 /
                FT_SHARE_FULL_FRAME_LONG_MM /
                1e6;
    double dmax;
    rc =
      outf(w, "%s{\"ppi\":%g,\"mp\":%.1f,\"px\":%.0f,\"dmax\":", i ? "," : "",
           ppi[i], mp, ppi[i] * in);
    if(!rc)
      rc = ft_estimate_dmax(&lim, ppi[i], &dmax) ? outf(w, "%.1f}", dmax)
                                                 : outf(w, "null}");
  }
  return rc ? rc : outf(w, "]");
}

int ftw_holders(ftw* w) {
  const ft_holder* h;
  int i, rc;
  out_reset(w);
  rc = outf(w, "[");
  for(i = 0; !rc && (h = ft_get_holder_at(i)) != NULL; i++) {
    rc = outf(w, "%s{\"name\":", i ? "," : "");
    rc = rc ? rc : out_str(w, h->name);
    rc = rc ? rc : outf(w, ",\"label\":");
    rc = rc ? rc : out_str(w, h->label);
    rc = rc ? rc : outf(w, ",\"frames\":%u}", h->grid_frame_count);
  }
  return rc ? rc : outf(w, "]");
}

/* ------------------------------------------------------------ job */

int ftw_lens(ftw* w, const char* text, size_t n) {
  ft_err e;
  return ft_parse_lens(&w->lens, text, n, &e) ? fail_e(w, &e) : 0;
}

double ftw_dof95(const ftw* w, double scale, int bin) {
  return ft_estimate_depth_of_field95(&w->lens, scale, bin);
}

static int job_parse(ftw* w, const char* text, size_t n, ft_job* to) {
  static ft_job j;
  ft_err e;
  if(ft_parse_job(&j, text, n, &e)) return fail_e(w, &e);
  *to = j;
  return 0;
}

int ftw_job_load(ftw* w, const char* text, size_t n) {
  if(job_parse(w, text, n, &w->job)) return -1;
  stream_free(w); /* the box's job: not the stream's */
  w->has_job = 1;
  w->has_plan = 0;
  return 0;
}

int ftw_plan_load(ftw* w, const char* text, size_t n) {
  if(job_parse(w, text, n, &w->plan)) return -1;
  w->has_plan = 1;
  return 0;
}

/* the planned job, else the base; NULL without either */
static const ft_job* job_now(const ftw* w) {
  return w->has_plan ? &w->plan : w->has_job ? &w->job : NULL;
}

/* the "key=" line's value in the preview's metadata; -1 if none */
static long meta_key(const char* t, size_t n, const char* key) {
  size_t at = 0, kl = strlen(key);
  long v = -1;
  while(t && at < n) {
    const char *s = t + at, *nl = memchr(s, '\n', n - at);
    size_t len = nl ? (size_t)(nl - s) : n - at;
    if(len > kl + 1 &&
       len < kl + 24 &&
       memcmp(s, key, kl) == 0 &&
       s[kl] == '=') {
      char b[24];
      memcpy(b, s + kl + 1, len - kl - 1);
      b[len - kl - 1] = 0;
      v = atol(b);
    }
    at += len + 1;
  }
  return v;
}

static long preview_x(const char* t, size_t n) { return meta_key(t, n, "x"); }

/* job frame f as the page's frame object: its box in preview px (ax),
 * its planned px, ppi, group and done (plan p, NULL none) */
static int frame_json(ftw* w, const ft_job_frame* f, const ft_job* p,
                      const ft_axes* ax, int first) {
  const ft_job_frame* c = NULL;
  double px = 0, dof = 0;
  int g;
  for(g = 0; p && g < (int)p->frame_count; g++)
    if(p->frames[g].index == f->index) c = &p->frames[g];
  if(c && c->is_included && p->share_pixels > 0)
    px = ft_get_frame_share_pixels(p, c);
  if(px > 0)
    dof = ftw_dof95(w, c->ppi * ft_get_frame_bin(c), ft_get_frame_bin(c));
  return outf(w,
              "%s{\"n\":%d,\"type\":\"%s\",\"low\":%s,"
              "\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f,"
              "\"px\":%g,\"ppi\":%g,\"group\":%d,\"done\":%s,"
              "\"dof\":%.0f}",
              first ? "" : ",", f->index,
              (unsigned)f->type <= 3 ? TYPES[f->type] : "full",
              f->is_confidence_low ? "true" : "false",
              (f->along_start_mm - ax->first_line_mm) / ax->mm_per_line,
              (f->across_start_mm - ax->first_pixel_mm) / ax->mm_per_pixel,
              f->length_mm / ax->mm_per_line, f->width_mm / ax->mm_per_pixel,
              px, px > 0 ? c->ppi : 0, px > 0 ? c->group : 0,
              px > 0 && c->is_done ? "true" : "false", isfinite(dof) ? dof : 0);
}

int ftw_job_json(ftw* w, const char* preview_txt, size_t n) {
  const ft_job *j = &w->job, *p = w->has_plan ? &w->plan : NULL;
  const ft_preview* pm = ft_find_preview("standard");
  long x = preview_x(preview_txt, n);
  int i, g, rc, groups[FT_MAX_FRAMES], ng = 0;
  ft_axes ax;
  if(!w->has_job) return fail(w, "no strip.job");
  ft_get_preview_axes(pm, x >= 0 ? x : FT_PREVIEW_FIRST_PIXEL, &ax);
  for(i = 0; p && p->share_pixels > 0 && i < (int)p->frame_count; i++) {
    if(!p->frames[i].is_included) continue;
    for(g = 0; g < ng && groups[g] != p->frames[i].group; g++);
    if(g == ng) groups[ng++] = p->frames[i].group;
  }
  out_reset(w);
  rc = outf(w, "{\"film\":\"%s\",\"holder\":",
            j->film == FT_FILM_SLIDE ? "slide" : "neg");
  rc = rc ? rc : out_str(w, j->holder);
  rc = rc ? rc : outf(w, ",\"windows\":%d,\"frames\":[", ng);
  for(i = 0; !rc && i < (int)j->frame_count; i++)
    rc = frame_json(w, &j->frames[i], p, &ax, i == 0);
  return rc ? rc : outf(w, "]}");
}

int ftw_frame_box(ftw* w, int frame, const double* box, const char* preview_txt,
                  size_t n) {
  static ft_job j;
  long x = preview_x(preview_txt, n);
  ft_axes ax;
  ft_err e;
  size_t len;
  int i;
  if(!w->has_job) return fail(w, "no strip.job");
  for(i = 0;
      i < (int)w->job.frame_count && (int)w->job.frames[i].index != frame; i++);
  if(i == (int)w->job.frame_count) {
    snprintf(w->err, sizeof w->err, "frame %d is not in the job", frame);
    return -1;
  }
  ft_get_preview_axes(ft_find_preview("standard"),
                      x >= 0 ? x : FT_PREVIEW_FIRST_PIXEL, &ax);
  j = w->job;
  j.frames[i].along_start_mm = ax.first_line_mm + box[0] * ax.mm_per_line;
  j.frames[i].across_start_mm = ax.first_pixel_mm + box[1] * ax.mm_per_pixel;
  j.frames[i].length_mm = box[2] * ax.mm_per_line;
  j.frames[i].width_mm = box[3] * ax.mm_per_pixel;
  if(ft_validate_job(&j, &w->lens, &e)) return fail_e(w, &e);
  w->job = j;
  if(stream_recut(w, frame, &e)) /* a streamed frame: cut again */
    return fail_e(w, &e);
  len = ft_format_job(&j, NULL, 0);
  out_reset(w);
  if(out_room(w, len)) return fail(w, "out of memory");
  ft_format_job(&j, (char*)w->out, len + 1);
  w->nout = len;
  return 0;
}

int ftw_plan(ftw* w, const double* px, int n) {
  static ft_job j;
  const ft_holder* h;
  ft_err e;
  int ng;
  if(!w->has_job) return fail(w, "no strip.job");
  if(n != (int)w->job.frame_count)
    return fail(w, "plan: one size per frame of the job");
  j = w->job;
  h = ft_find_holder(j.holder);
  if(!h) return fail(w, "plan: unknown holder");
  ng = ft_plan_share_sizes(&j, px, j.half_frames_at_full_frame_ppi, &w->lens,
                           h->scan_length_mm, &e);
  if(ng < 0 || ft_validate_job(&j, &w->lens, &e)) return fail_e(w, &e);
  w->plan = j;
  w->has_plan = 1;
  return ng;
}

int ftw_job_text(ftw* w) {
  const ft_job* j = job_now(w);
  size_t n;
  if(!j) return fail(w, "no strip.job");
  n = ft_format_job(j, NULL, 0);
  out_reset(w);
  if(out_room(w, n)) return fail(w, "out of memory");
  ft_format_job(j, (char*)w->out, n + 1);
  w->nout = n;
  return 0;
}

/* ------------------------------------------------------------ print */

int ftw_print_load(ftw* w, const char* text, size_t n) {
  static ft_print_file pf;
  ft_err e;
  if(ft_parse_print_file(text, n, &pf, &e)) return fail_e(w, &e);
  w->pf = pf;
  w->has_print = 1;
  w->measured = 0;
  pv_unmeasure(w);
  w->disp_frame = 0;
  return 0;
}

int ftw_print_new(ftw* w, const char* emulsion) {
  if(!ft_find_emulsion(emulsion)) return fail(w, "not an emulsion");
  ft_init_print_file(&w->pf, emulsion);
  w->has_print = 1;
  w->measured = 0;
  pv_unmeasure(w);
  w->disp_frame = 0;
  return 0;
}

int ftw_print_emulsion(ftw* w, const char* emulsion) {
  static ft_print_file pf;
  const ft_emulsion* em = ft_find_emulsion(emulsion);
  ft_err e;
  int i, dropped = 0;
  if(!w->has_print) return fail(w, "no strip.print");
  if(!em) return fail(w, "not an emulsion");
  ft_init_print_file(&pf, em->id);
  for(i = 0; i < (int)w->pf.frame_count; i++) {
    if(ft_check_print_clicks(em, w->pf.frames[i].clicks, &e) ||
       ft_set_print_clicks(&pf, w->pf.frames[i].index, w->pf.frames[i].clicks,
                           &e))
      dropped++;
    /* the turn stays whatever the clicks do */
    ft_set_print_rotation(&pf, w->pf.frames[i].index,
                          w->pf.frames[i].rotation_degrees, &e);
  }
  w->pf = pf;
  w->measured = 0;
  pv_unmeasure(w);
  out_reset(w);
  return outf(w, "%s", em->id) ? -1 : dropped;
}

int ftw_print_get(ftw* w, int frame, int* cmyd) {
  ft_cmyd k;
  if(!w->has_print) return fail(w, "no strip.print");
  k = ft_get_print_clicks(&w->pf, frame);
  cmyd[0] = k.cyan;
  cmyd[1] = k.magenta;
  cmyd[2] = k.yellow;
  cmyd[3] = k.density;
  return 0;
}

int ftw_print_clicks(ftw* w, int frame, int c, int m, int y, int d) {
  ft_cmyd k;
  ft_err e;
  k.cyan = c;
  k.magenta = m;
  k.yellow = y;
  k.density = d;
  if(!w->has_print) return fail(w, "no strip.print");
  if(ft_check_print_clicks(ft_find_emulsion(w->pf.emulsion), k, &e) ||
     ft_set_print_clicks(&w->pf, frame, k, &e))
    return fail_e(w, &e);
  return 0;
}

int ftw_print_rotate(ftw* w, int frame, int deg) {
  ft_err e;
  if(!w->has_print) return fail(w, "no strip.print");
  if(ft_set_print_rotation(&w->pf, frame, deg, &e)) return fail_e(w, &e);
  return 0;
}

int ftw_print_name(ftw* w, int frame) {
  if(!w->has_print) return fail(w, "no strip.print");
  out_reset(w);
  return outf(w, "%s", ft_get_print_name(&w->pf, frame));
}

int ftw_print_set_name(ftw* w, int frame, const char* name) {
  ft_err e;
  if(!w->has_print) return fail(w, "no strip.print");
  if(ft_set_print_name(&w->pf, frame, name, &e)) return fail_e(w, &e);
  return 0;
}

int ftw_print_rotation(ftw* w, int frame) {
  if(!w->has_print) return fail(w, "no strip.print");
  return ft_get_print_rotation(&w->pf, frame);
}

int ftw_print_text(ftw* w) {
  size_t n;
  if(!w->has_print) return fail(w, "no strip.print");
  n = ft_format_print_file(&w->pf, NULL, 0);
  out_reset(w);
  if(out_room(w, n)) return fail(w, "out of memory");
  ft_format_print_file(&w->pf, (char*)w->out, n + 1);
  w->nout = n;
  return 0;
}

/* ------------------------------------------------------------ frames */

/* the frame's image as ft_read_dng gives the DNG back */
static int frame_image(const wframe* f, ft_image* im, ft_err* e) {
  size_t k, n = (size_t)f->w * f->h * 3;
  ft_init_image(im);
  if(ft_allocate_image(im, f->w, f->h, e)) return -1;
  for(k = 0; k < n; k++) im->pixels[k] = (float)f->q[k] / 65535.0f;
  im->ppi = f->ppi;
  im->has_noise_profile = f->has_noise;
  memcpy(im->noise_profile, f->noise, sizeof im->noise_profile);
  return 0;
}

static int frame_at(const ftw* w, int frame) {
  int i;
  for(i = 0; i < w->n; i++)
    if(w->f[i].frame == frame) return i;
  return -1;
}

/* a cut frame -> wf, as ftdev cut writes it and ftdev print reads it */
static int land(const ft_share_metadata* m, const ft_share_cut_frame* c,
                wframe* wf, ft_err* e) {
  char side[1024];
  ft_image hdr, im, cut;
  size_t k, n = (size_t)c->image.width * c->image.height * 3;
  int len;

  memset(wf, 0, sizeof *wf);
  ft_init_image(&wf->proxy);
  len = ft_format_share_sidecar(side, sizeof side, m, c);
  if(len < 0 || len >= (int)sizeof side) return ft_fail(e, "sidecar too long");
  if(ft_parse_share_sidecar(side, (size_t)len, &wf->sc, e)) return -1;
  wf->frame = wf->sc.frame > 0 ? wf->sc.frame : c->index;
  wf->w = c->image.width;
  wf->h = c->image.height;
  wf->q = malloc(n * sizeof *wf->q);
  if(!wf->q) return ft_fail(e, "out of memory");
  for(k = 0; k < n; k++)
    wf->q[k] = (unsigned short)ft_quantise_dng_sample(c->image.pixels[k]);
  hdr = c->image; /* ppi and noise as stored */
  hdr.pixels = NULL;
  ft_simulate_dng_storage(&hdr);
  wf->ppi = hdr.ppi;
  wf->has_noise = hdr.has_noise_profile;
  memcpy(wf->noise, hdr.noise_profile, sizeof wf->noise);
  /* the stats proxy: ftdev print's load_cut + ft_make_share_proxy */
  if(frame_image(wf, &im, e)) goto bad;
  ft_init_image(&cut);
  if(ft_crop_image(&im, wf->sc.box, &cut, e)) {
    ft_free_image(&im);
    goto bad;
  }
  ft_free_image(&im);
  if(ft_make_share_proxy(&cut, &wf->proxy, e)) {
    ft_free_image(&cut);
    goto bad;
  }
  ft_free_image(&cut);
  return 0;
bad:
  frame_free(wf);
  return -1;
}

int ftw_cut(ftw* w, const unsigned short* raw, size_t raw_n,
            const unsigned short* cal, size_t cal_n, const char* faul,
            size_t faul_n, const char* meta, size_t meta_n, const char* defects,
            size_t defects_n) {
  static ft_share_cut_result r;
  static ft_share_metadata m;
  static ft_defects dfx;
  static wframe nf;
  ft_err e;
  int i, j, rc;

  if(ft_parse_share_metadata(meta, meta_n, &m, &e)) return fail_e(w, &e);
  if(defects && ft_parse_defects(defects, defects_n, &dfx, &e))
    return fail_e(w, &e);
  if(!raw || !cal) return fail(w, "no scan or no calibration");
  if(ft_cut_share_frames(raw, (int)(raw_n / ((size_t)m.pixel_count * 6)), cal,
                         (int)(cal_n / ((size_t)m.pixel_count * 6)), faul,
                         faul ? faul_n : 0, defects ? &dfx : NULL, &m, &r, &e))
    return fail_e(w, &e);
  out_reset(w);
  rc = outf(w, "{\"detect_ok\":%d,\"noise_note\":", r.was_detected);
  rc = rc ? rc : out_str(w, r.noise_result ? r.noise_error.message : "");
  rc = rc ? rc : outf(w, ",\"frames\":[");
  for(i = 0; !rc && i < (int)r.frame_count; i++) {
    const ft_share_cut_frame* c = &r.frames[i];
    if(land(&m, c, &nf, &e)) {
      rc = fail_e(w, &e);
      break;
    }
    j = frame_at(w, nf.frame);
    if(j >= 0) {
      frame_free(&w->f[j]);
    } else {
      if(w->n == FT_MAX_FRAMES) {
        frame_free(&nf);
        rc = fail(w, "too many frames");
        break;
      }
      for(j = w->n; j > 0 && w->f[j - 1].frame > nf.frame; j--)
        w->f[j] = w->f[j - 1];
      w->n++;
    }
    w->f[j] = nf;
    if(w->disp_frame == nf.frame) w->disp_frame = 0;
    rc = outf(w,
              "%s{\"frame\":%d,\"estimated\":%d,\"shift_mm\":%.3f,"
              "\"w\":%d,\"h\":%d}",
              i ? "," : "", nf.frame, c->is_estimated, c->shift_mm, c->box[2],
              c->box[3]);
  }
  w->measured = 0;
  i = r.frame_count;
  ft_free_share_cut(&r);
  if(rc) return -1;
  return outf(w, "]}") ? -1 : i;
}

int ftw_frames(const ftw* w, int* frames, int cap) {
  int i;
  for(i = 0; i < w->n && i < cap; i++) frames[i] = w->f[i].frame;
  return w->n;
}

/* ------------------------------------------------------------ prints */

static const ft_emulsion* emulsion(ftw* w) {
  const ft_emulsion* em;
  if(!w->has_print) {
    fail(w, "no strip.print");
    return NULL;
  }
  em = ft_find_emulsion(w->pf.emulsion);
  if(!em) fail(w, "not an emulsion");
  return em;
}

/* per-frame balance: each frame measured alone (ft_measure_share_balance)
 * with its own film base (its sidecar's, else estimated from it), as
 * ftdev print does one frame a call */
int ftw_measure(ftw* w) {
  const ft_emulsion* em = emulsion(w);
  float base[3];
  int i, c, idx[FT_MAX_FRAMES], ni = 0, estimated = 0;
  ft_err e;

  if(!em) return -1;
  if(w->n == 0) return fail(w, "no frames");
  out_reset(w);
  for(i = 0; i < w->n; i++) {
    const wframe* f = &w->f[i];
    if((f->sc.film == FT_FILM_SLIDE) != (em->mode == FT_RENDER_SLIDE)) {
      char m[160];
      snprintf(m, sizeof m, "frame %d is a %s; %s prints %s", f->frame,
               f->sc.film == FT_FILM_SLIDE ? "slide" : "negative", em->id,
               em->mode == FT_RENDER_SLIDE ? "slides" : "negatives");
      return fail(w, m);
    }
  }
  if(job_now(w)) {
    const ft_job* j = job_now(w);
    for(i = 0; i < (int)j->frame_count; i++)
      idx[ni++] = (int)j->frames[i].index;
  } else {
    for(i = 0; i < w->n; i++) idx[ni++] = w->f[i].frame;
  }
  if(ft_count_unknown_print_frames(&w->pf, idx, ni, &e))
    outf(w, "warning: strip.print: %s\n", e.message);
  for(i = 0; i < w->n; i++) {
    const wframe* f = &w->f[i];
    if(f->sc.has_film_base) {
      for(c = 0; c < 3; c++) base[c] = (float)f->sc.film_base_density[c];
    } else {
      const ft_image* one[1];
      one[0] = &f->proxy;
      if(ft_estimate_base_from_frames(one, 1, em->mode, base, &e))
        return fail_e(w, &e);
      estimated = 1;
    }
    if(ft_measure_share_balance(em, base, &f->proxy, &w->rolls[i], &w->pfr[i],
                                &e))
      return fail_e(w, &e);
  }
  if(estimated)
    outf(w,
         "note: no film_base in the sidecars; estimated from the "
         "frames\n");
  w->measured = 1;
  return 0;
}

/* frame i's JPEG into *jpg, its text into w->text */
static int print_one(ftw* w, int i, unsigned char** jpg, size_t* n) {
  const ft_emulsion* em = emulsion(w);
  const wframe* f = &w->f[i];
  ft_share_print_info info;
  ft_image im;
  ft_err e;
  int rc;
  if(!em) return -1;
  if(frame_image(f, &im, &e)) return fail_e(w, &e);
  rc = ft_print_share_frame(em, &w->rolls[i], &w->pfr[i],
                            ft_get_print_clicks(&w->pf, f->frame),
                            ft_get_print_rotation(&w->pf, f->frame), &im,
                            &f->sc, f->frame, jpg, n, w->text, &info, &e);
  ft_free_image(&im);
  return rc ? fail_e(w, &e) : 0;
}

static int ensure_measured(ftw* w) {
  if(w->measured) return 0;
  return ftw_measure(w);
}

int ftw_jpeg(ftw* w, int frame) {
  unsigned char* jpg;
  size_t n;
  int i = frame_at(w, frame);
  if(i < 0) return fail(w, "no such frame");
  if(ensure_measured(w) || print_one(w, i, &jpg, &n)) return -1;
  out_take(w, jpg, n);
  return 0;
}

/* src (transmittance) printed with em's recipe for roll/fr and clicks k,
 * turned rot degrees, sRGB 8-bit -> out: RGBA rows; *pw x *ph */
static int rgba_out(ftw* w, const ft_emulsion* em, const ft_print_roll* roll,
                    const ft_print_frame* fr, ft_cmyd k, const ft_image* src,
                    int rot, int* pw, int* ph) {
  static ft_print_render pr;
  static ft_output out;
  ft_print_auto a;
  ft_recipe r;
  ft_err e;
  float* px = NULL;
  size_t npx, i;
  int y, rw, rh;
  if(ft_make_print_recipe(em, roll, fr, k, &r, &a, NULL, &e) ||
     ft_compile_print(em, &r, &pr, &e) ||
     ft_compile_output(FT_SPACE_SRGB, FT_GAMUT_SOFT, &out, &e))
    return fail_e(w, &e);
  npx = (size_t)src->width * src->height;
  px = malloc(npx * 3 * sizeof *px);
  out_reset(w);
  if(!px || out_room(w, npx * 4)) {
    free(px);
    return fail(w, "out of memory");
  }
  ft_apply_print(&pr, src->pixels, px, npx);
  rw = src->width;
  rh = src->height;
  if(rot) { /* the print's turn */
    ft_image ia, ib;
    ft_init_image(&ia);
    ft_init_image(&ib);
    ia.width = rw;
    ia.height = rh;
    ia.pixels = px;
    if(ft_rotate_image(&ia, rot, &ib, &e)) {
      free(px);
      return fail_e(w, &e);
    }
    free(px);
    px = ib.pixels;
    rw = ib.width;
    rh = ib.height;
  }
  for(y = 0; y < rh; y++) {
    float* row = px + (size_t)y * rw * 3;
    unsigned char* o = w->out + (size_t)y * rw * 4;
    ft_apply_output(&out, row, row, (size_t)rw);
    /* RGB at the row's end, then spread to RGBA in place */
    ft_quantise_output8(row, o + (size_t)rw, (size_t)rw, (size_t)y * rw);
    for(i = 0; i < (size_t)rw; i++) {
      const unsigned char* s = o + (size_t)rw + i * 3;
      unsigned char t0 = s[0], t1 = s[1], t2 = s[2];
      o[i * 4] = t0;
      o[i * 4 + 1] = t1;
      o[i * 4 + 2] = t2;
      o[i * 4 + 3] = 255;
    }
  }
  free(px);
  w->nout = npx * 4;
  *pw = rw;
  *ph = rh;
  return 0;
}

/* src resized so its long side is at most long_edge (never up) -> dst */
static int fit(ftw* w, const ft_image* src, int long_edge, ft_image* dst) {
  ft_err e;
  void* scratch;
  uint32_t dw, dh;
  uint32_t big = src->width > src->height ? src->width : src->height;
  double s = (double)long_edge / big;
  ft_init_image(dst);
  ft_get_resized_size(src->width, src->height, s > 1 ? 1 : s, &dw, &dh);
  scratch =
    malloc(ft_get_resize_scratch_bytes(src->width, src->height, dw, dh));
  if(!scratch ||
     ft_allocate_image(dst, dw, dh, &e) ||
     ft_resize(src->pixels, src->width, src->height, dst->pixels, dw, dh,
               scratch, &e)) {
    free(scratch);
    ft_free_image(dst);
    return scratch ? fail_e(w, &e) : fail(w, "out of memory");
  }
  free(scratch);
  return 0;
}

int ftw_display(ftw* w, int frame, int long_edge, int* pw, int* ph) {
  const ft_emulsion* em;
  ft_err e;
  int i = frame_at(w, frame);
  if(i < 0) return fail(w, "no such frame");
  if(long_edge < 16) return fail(w, "display: too small");
  if(ensure_measured(w) || !(em = emulsion(w))) return -1;
  if(w->disp_frame != frame || w->disp_edge != long_edge) {
    const wframe* f = &w->f[i];
    ft_image im, cut;
    ft_free_image(&w->disp);
    w->disp_frame = 0;
    if(frame_image(f, &im, &e)) return fail_e(w, &e);
    ft_init_image(&cut);
    if(ft_crop_image(&im, f->sc.box, &cut, &e)) {
      ft_free_image(&im);
      return fail_e(w, &e);
    }
    ft_free_image(&im);
    if(fit(w, &cut, long_edge, &w->disp)) {
      ft_free_image(&cut);
      return -1;
    }
    ft_free_image(&cut);
    w->disp_frame = frame;
    w->disp_edge = long_edge;
  }
  return rgba_out(w, em, &w->rolls[i], &w->pfr[i],
                  ft_get_print_clicks(&w->pf, frame), &w->disp,
                  ft_get_print_rotation(&w->pf, frame), pw, ph);
}

/* ------------------------------------------------------------ preview */

/* job frame f's box cut from the preview t (nl lines x px, RGB-aligned;
 * ax its axes) into p, x along, as a share frame is cut, with its stats
 * proxy; to be measured. 1 cut, 0 off the preview, -1 with e filled */
static int cut_pframe(const float* t, int nl, int px, const ft_axes* ax,
                      const ft_job_frame* f, pframe* p, ft_err* e) {
  ft_develop_options co;
  long l0 = (long)((f->along_start_mm - ax->first_line_mm) / ax->mm_per_line +
                   0.5),
       l1 = (long)((f->along_start_mm + f->length_mm - ax->first_line_mm) /
                     ax->mm_per_line +
                   0.5),
       c0 =
         (long)((f->across_start_mm - ax->first_pixel_mm) / ax->mm_per_pixel +
                0.5),
       c1 = (long)((f->across_start_mm + f->width_mm - ax->first_pixel_mm) /
                     ax->mm_per_pixel +
                   0.5);
  if(l0 < 0) l0 = 0;
  if(c0 < 0) c0 = 0;
  if(l1 > nl) l1 = nl;
  if(c1 > px) c1 = px;
  if(l1 - l0 < 8 || c1 - c0 < 8) return 0; /* off the preview: no frame */
  memset(&co, 0, sizeof co);
  co.crop_x = (int)c0;
  co.crop_y = (int)l0;
  co.crop_width = (int)(c1 - c0);
  co.crop_height = (int)(l1 - l0);
  ft_init_image(&p->im);
  ft_init_image(&p->proxy);
  if(ft_crop_and_orient(t, nl, px, &co, &p->im, e) ||
     ft_make_share_proxy(&p->im, &p->proxy, e)) {
    ft_free_image(&p->im);
    ft_free_image(&p->proxy);
    return -1;
  }
  p->frame = f->index;
  p->measured = 0;
  return 1;
}

int ftw_preview_load(ftw* w, const unsigned short* raw, size_t raw_n,
                     const unsigned short* cal, size_t cal_n, const char* faul,
                     size_t faul_n, const char* meta, size_t meta_n) {
  const ft_preview* pm = ft_find_preview("standard");
  const ft_job* j = &w->job;
  long px = meta_key(meta, meta_n, "px"), x = meta_key(meta, meta_n, "x"),
       bin = meta_key(meta, meta_n, "bin");
  ft_develop_options o;
  ft_axes ax;
  ft_err e;
  float* t;
  int i, lines, cl;
  uint32_t nl = 0;

  preview_free(w);
  stream_free(w);
  if(!w->has_job) return fail(w, "no strip.job");
  if(px < 1 || px > 20000 || x < 0 || bin != pm->bin)
    return fail(w, "preview: not a standard preview (px, x, bin=12)");
  if(!raw || !cal) return fail(w, "no scan or no calibration");
  lines = (int)(raw_n / ((size_t)px * 6));
  cl = (int)(cal_n / ((size_t)px * 6));
  memset(&o, 0, sizeof o);
  o.first_pixel = (int)x;
  o.bin = (int)bin;
  t = ft_develop_transmittance(raw, lines, (int)px, cal, cl, faul,
                               faul ? faul_n : 0, &o, &nl, &e);
  if(!t) return fail_e(w, &e);
  ft_get_preview_axes(pm, x, &ax);
  for(i = 0; i < (int)j->frame_count; i++) {
    int rc = cut_pframe(t, nl, (int)px, &ax, &j->frames[i], &w->pv[w->npv], &e);
    if(rc < 0) {
      free(t);
      preview_free(w);
      return fail_e(w, &e);
    }
    w->npv += rc;
  }
  free(t);
  return w->npv;
}

/* preview frame i's own balance (per-frame: measured from the frame
 * alone, locked until its box or the emulsion changes) */
static int preview_measure(ftw* w, const ft_emulsion* em, int i) {
  const ft_job* j = &w->job;
  pframe* p = &w->pv[i];
  float base[3];
  ft_err e;
  int c;
  if(p->measured) return 0;
  if((j->film == FT_FILM_SLIDE) != (em->mode == FT_RENDER_SLIDE))
    return fail(w, j->film == FT_FILM_SLIDE
                     ? "the strip is slide film; the emulsion prints "
                       "negatives"
                     : "the strip is negative film; the emulsion prints "
                       "slides");
  if(j->has_film_base) {
    for(c = 0; c < 3; c++) base[c] = (float)j->film_base_density[c];
  } else {
    const ft_image* one[1];
    one[0] = &p->proxy;
    if(ft_estimate_base_from_frames(one, 1, em->mode, base, &e))
      return fail_e(w, &e);
  }
  if(ft_measure_share_balance(em, base, &p->proxy, &p->roll, &p->pf, &e))
    return fail_e(w, &e);
  p->measured = 1;
  return 0;
}

/* a preview frame printed; with_print: strip.print's clicks and turn */
static int preview_rgba(ftw* w, int frame, int long_edge, int with_print,
                        int* pw, int* ph) {
  static const ft_cmyd none;
  const ft_emulsion* em;
  ft_image d;
  int i, rc;
  for(i = 0; i < w->npv && w->pv[i].frame != frame; i++);
  if(i == w->npv) return fail(w, "no such frame in the preview");
  if(long_edge < 16) return fail(w, "display: too small");
  if(!(em = emulsion(w)) ||
     preview_measure(w, em, i) ||
     fit(w, &w->pv[i].im, long_edge, &d))
    return -1;
  rc = rgba_out(w, em, &w->pv[i].roll, &w->pv[i].pf,
                with_print ? ft_get_print_clicks(&w->pf, frame) : none, &d,
                with_print ? ft_get_print_rotation(&w->pf, frame) : 0, pw, ph);
  ft_free_image(&d);
  return rc;
}

int ftw_preview_display(ftw* w, int frame, int long_edge, int* pw, int* ph) {
  return preview_rgba(w, frame, long_edge, 0, pw, ph);
}

int ftw_preview_print(ftw* w, int frame, int long_edge, int* pw, int* ph) {
  return preview_rgba(w, frame, long_edge, 1, pw, ph);
}

int ftw_preview_px(void) { return FT_PREVIEW_PIXEL_COUNT; }

/* a frame's JPEG in the zip: its name, else <base>_frameNN */
static void zip_name(const ftw* w, const char* base, int frame, char* name,
                     size_t cap) {
  const char* nm = w->has_print ? ft_get_print_name(&w->pf, frame) : "";
  if(nm[0])
    snprintf(name, cap, "%s.jpg", nm);
  else
    snprintf(name, cap, "%.120s_frame%02d.jpg", base, frame);
}

int ftw_live_rows(ftw* w, const unsigned short* raw, size_t n, int px, int neg,
                  int first) {
  size_t i, lines = px > 0 ? n / ((size_t)px * 3) : 0;
  unsigned char* o;
  if(px <= 0 || n % ((size_t)px * 3) || !lines) {
    snprintf(w->err, sizeof w->err, "live: not whole lines of %d px", px);
    return -1;
  }
  if(first) {
    w->live_white[0] = w->live_white[1] = w->live_white[2] = 1;
    for(i = 0; i < n; i++)
      if(raw[i] > w->live_white[i % 3]) w->live_white[i % 3] = raw[i];
  } else if(!(w->live_white[0] > 0)) {
    return fail(w, "live: no first lines yet");
  }
  out_reset(w);
  if(out_room(w, lines * (size_t)px * 4)) return fail(w, "out of memory");
  o = w->out;
  for(i = 0; i < n; i++) {
    double t = raw[i] / w->live_white[i % 3], v;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    v = pow(t, 1 / 2.2);
    *o++ = (unsigned char)((neg ? 1 - v : v) * 255 + 0.5);
    if(i % 3 == 2) *o++ = 255;
  }
  w->nout = lines * (size_t)px * 4;
  return (int)lines;
}

/* ------------------------------------------------------------ stream */

static void stream_free(ftw* w) {
  free(w->st_dark);
  free(w->st_white);
  free(w->st_prev);
  free(w->st_t);
  ft_free_stream_detector(&w->st_det);
  w->st_dark = w->st_white = w->st_prev = w->st_t = NULL;
  w->st_on = 0;
  w->st_n = w->st_cap = w->st_raw = 0;
}

int ftw_stream_begin(ftw* w, const unsigned short* cal, size_t cal_n,
                     const char* faul, size_t faul_n, const char* meta,
                     size_t meta_n, int slide) {
  const ft_preview* pm = ft_find_preview("standard");
  const ft_holder* h = ft_find_holder("24x217");
  long px = meta_key(meta, meta_n, "px"), x = meta_key(meta, meta_n, "x"),
       bin = meta_key(meta, meta_n, "bin");
  ft_film film = slide ? FT_FILM_SLIDE : FT_FILM_NEGATIVE;
  ft_err e;
  int i;

  stream_free(w);
  preview_free(w);
  if(px < 1 || px > 20000 || x < 0 || bin != pm->bin)
    return fail(w, "stream: not a standard preview (px, x, bin=12)");
  if(!cal) return fail(w, "stream: no calibration yet");
  w->st_dark = malloc((size_t)px * 3 * sizeof *w->st_dark);
  w->st_white = malloc((size_t)px * 3 * sizeof *w->st_white);
  w->st_prev = malloc((size_t)px * 6 * sizeof *w->st_prev);
  if(!w->st_dark || !w->st_white || !w->st_prev) {
    stream_free(w);
    return fail(w, "out of memory");
  }
  if(ft_split_calibration(cal, (int)(cal_n / ((size_t)px * 6)), (int)px,
                          w->st_dark, w->st_white, &e)) {
    stream_free(w);
    return fail_e(w, &e);
  }
  for(i = 0; i < 3; i++) w->st_fcount[i] = 0;
  if(faul) ft_parse_faulty_pixels(faul, faul_n, w->st_faul, w->st_fcount);
  ft_get_preview_axes(pm, x, &w->st_ax);
  if(ft_init_stream_detector(&w->st_det, &w->st_ax, (int)px, film, h, &e)) {
    stream_free(w);
    return fail_e(w, &e);
  }
  w->st_px = (int)px;
  w->st_x = x;
  w->st_on = 1;
  w->st_grid = 0;
  /* the base job: the stream's frames as they seal */
  memset(&w->job, 0, sizeof w->job);
  snprintf(w->job.holder, sizeof w->job.holder, "%s", h->name);
  w->job.film = film;
  w->job.focus_mode = FT_FOCUS_SHIFTED;
  w->has_job = 1;
  w->has_plan = 0;
  return 0;
}

/* detector frame f into the base job (as ft_detect's go:
 * ft_convert_layout_to_job) and cut from the rows in hand */
static int stream_seal(ftw* w, const ft_stream_frame* f, ft_err* e) {
  static ft_layout L;
  static ft_job j;
  const ft_holder* h = ft_find_holder("24x217");
  int has_base;
  memset(&L, 0, sizeof L);
  has_base = ft_get_stream_base(&w->st_det, L.base_transmittance);
  L.frame_count = 1;
  L.frames[0].index = f->index;
  L.frames[0].type = f->type;
  L.frames[0].start_mm = f->start_mm;
  L.frames[0].end_mm = f->end_mm;
  L.frames[0].across_start_mm = f->across_start_mm;
  L.frames[0].across_end_mm = f->across_end_mm;
  L.frames[0].is_confidence_low = f->is_from_grid;
  ft_convert_layout_to_job(&L, h, w->job.film, 2000, 0, &j);
  if(w->job.frame_count == FT_MAX_FRAMES || w->npv == FT_MAX_FRAMES)
    return ft_fail(e, "stream: too many frames");
  /* the film base: the first frame's, kept for every frame after */
  if(w->job.frame_count == 0 && has_base) {
    memcpy(w->job.film_base_density, j.film_base_density,
           sizeof j.film_base_density);
    w->job.has_film_base = 1;
  }
  w->job.frames[w->job.frame_count++] = j.frames[0];
  w->st_grid |= f->is_from_grid;
  if(cut_pframe(w->st_t, (int)w->st_n, w->st_px, &w->st_ax, &j.frames[0],
                &w->pv[w->npv], e) < 0)
    return -1;
  if((int)w->pv[w->npv].frame == (int)f->index) w->npv++;
  return 0;
}

/* the frames sealed since job frame `from`, as JSON -> out */
static int stream_out(ftw* w, int from) {
  int i, rc = 0;
  out_reset(w);
  rc = outf(w, "{\"lines\":%ld,\"grid\":%s,\"frames\":[", w->st_n,
            w->st_grid ? "true" : "false");
  for(i = from; !rc && i < (int)w->job.frame_count; i++)
    rc = frame_json(w, &w->job.frames[i], NULL, &w->st_ax, i == from);
  rc = rc ? rc : outf(w, "]}");
  return rc ? -1 : (int)w->job.frame_count - from;
}

int ftw_stream_feed(ftw* w, const unsigned short* raw, size_t n) {
  size_t row = (size_t)w->st_px * 3, lines, i;
  int from = w->job.frame_count;
  float* ff;
  ft_err e;
  if(!w->st_on) return fail(w, "stream: not begun");
  if(n % (row * 2)) return fail(w, "stream: not whole lines");
  lines = n / (row * 2);
  if(w->st_n + (long)lines > w->st_cap) {
    long cap = w->st_cap ? w->st_cap : 1024;
    float* nt;
    while(cap < w->st_n + (long)lines) cap *= 2;
    nt = realloc(w->st_t, (size_t)cap * row * sizeof *nt);
    if(!nt) return fail(w, "out of memory");
    w->st_t = nt;
    w->st_cap = cap;
  }
  ff = malloc((lines ? lines : 1) * row * sizeof *ff);
  if(!ff) return fail(w, "out of memory");
  /* develop's stages, line by line: flat-field, Faul Pixs, then the
   * tri-linear realignment (R two lines on, G one: ft_align_rgb_rows, k 1) */
  ft_apply_flat_field(raw, (int)lines, w->st_px, w->st_dark, w->st_white, ff);
  if(lines && ft_repair_faulty_pixels(ff, (int)lines, w->st_px, w->st_faul,
                                      w->st_fcount, (int)w->st_x, 12)) {
    free(ff);
    return fail(w, "out of memory");
  }
  for(i = 0; i < lines; i++) {
    const float* r = ff + i * row;
    if(w->st_raw >= 2) {
      float* o = w->st_t + (size_t)w->st_n * row;
      const float *g = w->st_prev + ((w->st_raw - 1) % 2) * row,
                  *b = w->st_prev + ((w->st_raw - 2) % 2) * row;
      size_t c;
      for(c = 0; c < row; c += 3) {
        o[c] = r[c];
        o[c + 1] = g[c + 1];
        o[c + 2] = b[c + 2];
      }
      w->st_n++;
      ft_detect_stream_line(&w->st_det, o);
      while(w->job.frame_count < w->st_det.frame_count)
        if(stream_seal(w, &w->st_det.frames[w->job.frame_count], &e)) {
          free(ff);
          return fail_e(w, &e);
        }
    }
    memcpy(w->st_prev + (w->st_raw % 2) * row, r, row * sizeof *r);
    w->st_raw++;
  }
  free(ff);
  return stream_out(w, from);
}

int ftw_stream_end(ftw* w) {
  int from = w->job.frame_count;
  ft_err e;
  if(!w->st_on) return fail(w, "stream: not begun");
  ft_end_stream_detection(&w->st_det);
  while(w->job.frame_count < w->st_det.frame_count)
    if(stream_seal(w, &w->st_det.frames[w->job.frame_count], &e))
      return fail_e(w, &e);
  return stream_out(w, from);
}

/* frame `frame` of the stream cut again from the rows (its box moved) */
static int stream_recut(ftw* w, int frame, ft_err* e) {
  int i, k;
  for(i = 0;
      i < (int)w->job.frame_count && (int)w->job.frames[i].index != frame; i++);
  for(k = 0; k < w->npv && w->pv[k].frame != frame; k++);
  if(!w->st_on || i == (int)w->job.frame_count) return 0;
  if(k < w->npv) {
    ft_free_image(&w->pv[k].im);
    ft_free_image(&w->pv[k].proxy);
  } else if(w->npv < FT_MAX_FRAMES) {
    k = w->npv;
  } else {
    return 0;
  }
  if(cut_pframe(w->st_t, (int)w->st_n, w->st_px, &w->st_ax, &w->job.frames[i],
                &w->pv[k], e) < 0)
    return -1;
  if(k == w->npv && w->pv[k].frame == frame)
    w->npv++;
  else if(w->pv[k].frame != frame) { /* off the preview now: gone */
    for(; k + 1 < w->npv; k++) w->pv[k] = w->pv[k + 1];
    w->npv--;
  }
  w->disp_frame = 0;
  return 0;
}

int ftw_stream_windows(ftw* w, const char* text, size_t n) {
  static ft_job j;
  ft_err e;
  int i, k, got = 0;
  if(!w->has_job) return fail(w, "no strip.job");
  if(ft_parse_job(&j, text, n, &e)) return fail_e(w, &e);
  for(i = 0; i < (int)j.frame_count; i++)
    for(k = 0; k < (int)w->job.frame_count; k++) {
      ft_job_frame* f = &w->job.frames[k];
      if(f->index != j.frames[i].index) continue;
      /* the job's text keeps 0.001 mm: closer is the same window */
      if(fabs(f->along_start_mm - j.frames[i].along_start_mm) > 6e-4 ||
         fabs(f->across_start_mm - j.frames[i].across_start_mm) > 6e-4 ||
         fabs(f->length_mm - j.frames[i].length_mm) > 6e-4 ||
         fabs(f->width_mm - j.frames[i].width_mm) > 6e-4) {
        f->along_start_mm = j.frames[i].along_start_mm;
        f->across_start_mm = j.frames[i].across_start_mm;
        f->length_mm = j.frames[i].length_mm;
        f->width_mm = j.frames[i].width_mm;
        if(stream_recut(w, f->index, &e)) return fail_e(w, &e);
        got++;
      }
    }
  return got;
}

int ftw_zip(ftw* w, const char* base) {
  ft_zip z;
  ft_err e;
  unsigned char *jpg, *zb;
  size_t n, nz;
  char name[160];
  int i;
  if(w->n == 0) return fail(w, "no frames");
  if(ensure_measured(w)) return -1;
  ft_init_zip(&z);
  for(i = 0; i < w->n; i++) {
    if(print_one(w, i, &jpg, &n)) {
      ft_free_zip(&z);
      return -1;
    }
    zip_name(w, base, w->f[i].frame, name, sizeof name);
    if(ft_add_to_zip(&z, name, jpg, n, &e)) {
      free(jpg);
      ft_free_zip(&z);
      return fail_e(w, &e);
    }
    free(jpg);
    snprintf(name + strlen(name), sizeof name - strlen(name), ".txt");
    if(ft_add_to_zip(&z, name, w->text, strlen(w->text), &e)) {
      ft_free_zip(&z);
      return fail_e(w, &e);
    }
  }
  if(ft_finish_zip(&z, &zb, &nz, &e)) return fail_e(w, &e);
  out_take(w, zb, nz);
  return 0;
}

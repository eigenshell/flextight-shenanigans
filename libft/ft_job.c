/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_job.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_holder.h"
#include "ft_plan.h"

static const char* TYPES[] = {"full", "half", "panorama", "partial"};
static const char* DETAIL[] = {"none", "neg", "pos"};
static const char* DFMT[] = {"dng", "tiff"};
static const char* BW[] = {"off", "mono", "rgb"};

static const char* yn(int v) { return v ? "yes" : "no"; }

int ft_job_bin(const ft_job_frame* f) {
  return f->bin ? f->bin : ft_bin_for_ppi(f->ppi);
}

double ft_job_share_px(const ft_job* j, const ft_job_frame* f) {
  return f->share_px > 0 ? f->share_px : j->share;
}

int ft_job_pass_todo(const ft_job* j, int pass) {
  int i, n = 0;
  for(i = 0; i < j->nframes; i++)
    n += j->f[i].include && !j->f[i].done && j->f[i].pass == pass;
  return n;
}

/* text into a caller's buffer: n counts every byte asked for, the buffer
 * keeps what fits and stays NUL-terminated */
typedef struct {
  char* b;
  size_t cap, n;
} wbuf;

static void putf(wbuf* w, const char* fmt, ...) {
  va_list ap;
  int k;
  va_start(ap, fmt);
  k = vsnprintf(w->n < w->cap ? w->b + w->n : NULL,
                w->n < w->cap ? w->cap - w->n : 0, fmt, ap);
  va_end(ap);
  if(k > 0) w->n += (size_t)k;
}

/* "# bin options at P ppi: b: time, light, Dmax | ..." for the frame */
static void bin_options(wbuf* out, const ft_job_frame* f) {
  int b[6], n = ft_bins_for_ppi(f->ppi, b), i;
  if(n < 2) return;
  putf(out, "# bin options at %g ppi (time, light, Dmax~):", f->ppi);
  for(i = 0; i < n; i++) {
    double l = ft_light_rel(&ft_lens_default, f->ppi * b[i]);
    putf(out, "%s %d: %.0f s %.1fx %.2f", i ? " |" : "", b[i],
         ft_scan_seconds(f->ppi, b[i], f->len), l, ft_dmax_est(l));
  }
  putf(out, "\n");
}

size_t ft_job_write_buf(const ft_job* j, char* buf, size_t cap) {
  wbuf wb = {buf, cap, 0}, *out = &wb;
  int i;
  if(cap) buf[0] = 0;
  for(i = 0; i < j->nframes; i++)
    if((int)j->f[i].type < 0 || (int)j->f[i].type > 3) return FT_JOB_BAD;
  if((int)j->detail < 0 ||
     (int)j->detail > 2 ||
     (int)j->detail_fmt < 0 ||
     (int)j->detail_fmt > 1 ||
     (int)j->detail_bw < 0 ||
     (int)j->detail_bw > 2)
    return FT_JOB_BAD;
  putf(out,
       "[strip]\nholder = %s\nfilm = %s\nfocus = %s\n"
       "focus_offset = %ld\n",
       j->holder, j->film == FT_FILM_SLIDE ? "slide" : "negative",
       j->focus == FT_FOCUS_PER_FRAME ? "per-frame" : "shifted",
       j->focus_offset);
  if(j->has_film_base)
    putf(out, "film_base = %.4f %.4f %.4f   # D r g b: unexposed film\n",
         j->film_base[0], j->film_base[1], j->film_base[2]);
  if(j->detail != FT_DETAIL_NONE)
    putf(out,
         "detail = %s   # neg: digital negative, pos: archive "
         "positive\ndetail_fmt = %s\ndetail_bw = %s\n"
         "keep_raw = %s\nring_mb = %d\n",
         DETAIL[j->detail], DFMT[j->detail_fmt], BW[j->detail_bw],
         yn(j->keep_raw), j->ring_mb);
  if(j->share > 0)
    putf(out,
         "share = %g   # scan to share: px on each frame's long "
         "side\nhf_half_res = %s\n",
         j->share, yn(j->hf_half_res));
  for(i = 0; i < j->nframes; i++) {
    const ft_job_frame* f = &j->f[i];
    putf(out,
         "\n[frame %d]\ntype = %s\ninclude = %s\nppi = %g\n"
         "bin = %d   # 0 = fastest; smaller = more light, slower\n",
         f->index, TYPES[f->type], yn(f->include), f->ppi, f->bin);
    bin_options(out, f);
    putf(out,
         "window = %.3f %.3f %.3f %.3f   # y0 x0 length width, mm\n"
         "tilt = %.2f\nsprockets = %s\nstraighten = %s\n"
         "confidence = %s\nfocus_offset = %ld\ndone = %s\n",
         f->y0, f->x0, f->len, f->width, f->tilt_deg, yn(f->sprockets),
         yn(f->straighten), f->confidence_low ? "low" : "high", f->focus_offset,
         yn(f->done));
    if(j->detail != FT_DETAIL_NONE) putf(out, "pass = %d\n", f->pass);
    if(j->share > 0)
      putf(out,
           "group = %d   # window: frames of one group scan "
           "together\n",
           f->group);
    if(f->share_px > 0)
      putf(out,
           "share_px = %g   # this frame's target, px on its "
           "long side\n",
           f->share_px);
  }
  return wb.n;
}

static char* trim(char* s) {
  char* end;
  while(isspace((unsigned char)*s)) s++;
  end = s + strlen(s);
  while(end > s && isspace((unsigned char)end[-1])) *--end = 0;
  return s;
}

static int yesno(const char* v, int* out) {
  if(strcmp(v, "yes") == 0) {
    *out = 1;
    return 0;
  }
  if(strcmp(v, "no") == 0) {
    *out = 0;
    return 0;
  }
  return -1;
}

static int num(const char* v, double* out) {
  char* end;
  *out = strtod(v, &end);
  return end != v && *trim(end) == 0 && isfinite(*out) ? 0 : -1;
}

static int lnum(const char* v, long* out) {
  char* end;
  errno = 0;
  *out = strtol(v, &end, 10);
  return end != v && *trim(end) == 0 && errno != ERANGE ? 0 : -1;
}

static const char* STRIP_KEYS[] = {"holder",       "film",      "focus",
                                   "focus_offset", "film_base", "detail",
                                   "detail_fmt",   "detail_bw", "keep_raw",
                                   "ring_mb",      "share",     "hf_half_res"};
#define N_STRIP_KEYS 12
static const char* FRAME_KEYS[] = {
  "type",      "include",    "ppi",        "window",       "tilt",
  "sprockets", "straighten", "confidence", "focus_offset", "done",
  "bin",       "pass",       "group",      "share_px"};
#define N_FRAME_KEYS 14
#define K_PPI 2
#define K_WINDOW 3

/* v as an index into tab; -1 if it is none of them */
static int word(const char** tab, int n, const char* v, int* out) {
  int i;
  for(i = 0; i < n; i++)
    if(strcmp(tab[i], v) == 0) {
      *out = i;
      return 0;
    }
  return -1;
}

static int key_index(const char** tab, int n, const char* k) {
  int i;
  for(i = 0; i < n; i++)
    if(strcmp(tab[i], k) == 0) return i;
  return -1;
}

static int finish_frame(const ft_job_frame* f, unsigned seen, ft_err* e) {
  if(!(seen & (1u << K_PPI)))
    return ft_fail(e, "line %d: frame %d has no ppi", f->line, f->index);
  if(!(seen & (1u << K_WINDOW)))
    return ft_fail(e, "line %d: frame %d has no window", f->line, f->index);
  return 0;
}

int ft_job_parse(ft_job* j, const char* text, size_t n, ft_err* e) {
  char line[256];
  const char *p = text, *end = text + n;
  int ln = 0, in_strip = 0, strip_line = 0, i;
  unsigned seen = 0;
  ft_job_frame* f = NULL;

  memset(j, 0, sizeof *j);
  j->ring_mb = FT_RING_MB_DEFAULT;
  while(p < end) {
    const char* nl = memchr(p, '\n', (size_t)(end - p));
    size_t len = (size_t)((nl ? nl : end) - p);
    char *s, *k, *v, *hash;
    int bad = 0, ki;
    ln++;
    if(memchr(p, 0, len)) return ft_fail(e, "line %d: NUL byte", ln);
    if(len >= sizeof line - 1) return ft_fail(e, "line %d: too long", ln);
    memcpy(line, p, len);
    line[len] = 0;
    p = nl ? nl + 1 : end;
    if((hash = strchr(line, '#')) != NULL) *hash = 0;
    s = trim(line);
    if(!*s) continue;
    if(*s == '[') {
      if(strcmp(s, "[strip]") == 0) {
        if(f && finish_frame(f, seen, e)) return -1;
        if(strip_line)
          return ft_fail(e,
                         "line %d: duplicate [strip] section "
                         "(first at line %d)",
                         ln, strip_line);
        strip_line = ln;
        in_strip = 1;
        f = NULL;
        seen = 0;
        continue;
      }
      if(strncmp(s, "[frame ", 7) == 0) {
        char* end;
        long n;
        errno = 0;
        n = strtol(s + 7, &end, 10);
        if(end == s + 7 ||
           end[0] != ']' ||
           end[1] != 0 ||
           errno == ERANGE ||
           n < 1 ||
           n > 1000000)
          return ft_fail(e, "line %d: bad frame header %s", ln, s);
        if(f && finish_frame(f, seen, e)) return -1;
        for(i = 0; i < j->nframes; i++)
          if(j->f[i].index == (int)n)
            return ft_fail(e,
                           "line %d: frame %d repeated "
                           "(first at line %d)",
                           ln, (int)n, j->f[i].line);
        if(j->nframes == FT_MAX_FRAMES)
          return ft_fail(e, "line %d: more than %d frames", ln, FT_MAX_FRAMES);
        f = &j->f[j->nframes++];
        f->index = (int)n;
        f->line = ln;
        f->include = 1;
        in_strip = 0;
        seen = 0;
        continue;
      }
      return ft_fail(e, "line %d: unknown section %s", ln, s);
    }
    v = strchr(s, '=');
    if(!v) return ft_fail(e, "line %d: expected key = value", ln);
    *v++ = 0;
    k = trim(s);
    v = trim(v);
    if(in_strip)
      ki = key_index(STRIP_KEYS, N_STRIP_KEYS, k);
    else if(f)
      ki = key_index(FRAME_KEYS, N_FRAME_KEYS, k);
    else
      return ft_fail(e, "line %d: %s outside a section", ln, k);
    if(ki < 0) return ft_fail(e, "line %d: unknown key %s", ln, k);
    if(seen & (1u << ki)) return ft_fail(e, "line %d: duplicate key %s", ln, k);
    seen |= 1u << ki;
    if(in_strip) {
      if(ki == 0) {
        if(strlen(v) >= sizeof j->holder || !*v)
          bad = 1;
        else
          strcpy(j->holder, v);
      } else if(ki == 1) {
        if(strcmp(v, "negative") == 0)
          j->film = FT_FILM_NEGATIVE;
        else if(strcmp(v, "slide") == 0)
          j->film = FT_FILM_SLIDE;
        else
          bad = 1;
      } else if(ki == 2) {
        if(strcmp(v, "shifted") == 0)
          j->focus = FT_FOCUS_SHIFTED;
        else if(strcmp(v, "per-frame") == 0)
          j->focus = FT_FOCUS_PER_FRAME;
        else
          bad = 1;
      } else if(ki == 3) {
        bad = lnum(v, &j->focus_offset);
      } else if(ki >= 5 && ki <= 7) {
        int d;
        bad = ki == 5   ? word(DETAIL + 1, 2, v, &d)
              : ki == 6 ? word(DFMT, 2, v, &d)
                        : word(BW, 3, v, &d);
        if(!bad && ki == 5)
          j->detail = (ft_detail_out)(d + 1);
        else if(!bad && ki == 6)
          j->detail_fmt = (ft_detail_fmt)d;
        else if(!bad)
          j->detail_bw = (ft_detail_bw)d;
      } else if(ki == 8) {
        bad = yesno(v, &j->keep_raw);
      } else if(ki == 9) {
        long m;
        bad = lnum(v, &m) || m < FT_RING_MB_MIN || m > FT_RING_MB_MAX;
        if(!bad) j->ring_mb = (int)m;
      } else if(ki == 10) {
        bad = num(v, &j->share) || !(j->share >= 100 && j->share <= 20000);
      } else if(ki == 11) {
        bad = yesno(v, &j->hf_half_res);
      } else {
        double* d = j->film_base;
        char extra;
        bad = sscanf(v, "%lf %lf %lf %c", &d[0], &d[1], &d[2], &extra) != 3 ||
              !(d[0] >= 0 &&
                d[0] <= 4.5 &&
                d[1] >= 0 &&
                d[1] <= 4.5 &&
                d[2] >= 0 &&
                d[2] <= 4.5);
        j->has_film_base = !bad;
      }
    } else {
      switch(ki) {
        case 0: {
          int t;
          for(t = 0; t < 4 && strcmp(v, TYPES[t]) != 0; t++);
          if(t == 4)
            bad = 1;
          else
            f->type = (ft_frame_type)t;
          break;
        }
        case 1:
          bad = yesno(v, &f->include);
          break;
        case 2:
          bad = num(v, &f->ppi);
          break;
        case 3: {
          char extra;
          bad = sscanf(v, "%lf %lf %lf %lf %c", &f->y0, &f->x0, &f->len,
                       &f->width, &extra) != 4 ||
                !isfinite(f->y0) ||
                !isfinite(f->x0) ||
                !isfinite(f->len) ||
                !isfinite(f->width);
          break;
        }
        case 4:
          bad = num(v, &f->tilt_deg);
          break;
        case 5:
          bad = yesno(v, &f->sprockets);
          break;
        case 6:
          bad = yesno(v, &f->straighten);
          break;
        case 7:
          if(strcmp(v, "low") == 0)
            f->confidence_low = 1;
          else if(strcmp(v, "high") == 0)
            f->confidence_low = 0;
          else
            bad = 1;
          break;
        case 8:
          bad = lnum(v, &f->focus_offset);
          break;
        case 10: {
          long b;
          bad = lnum(v, &b) || b < 0 || b > 12;
          if(!bad) f->bin = (int)b;
          break;
        }
        case 11: {
          long ps;
          bad = lnum(v, &ps) || ps < 0 || ps > FT_MAX_FRAMES;
          if(!bad) f->pass = (int)ps;
          break;
        }
        case 12: {
          long g;
          bad = lnum(v, &g) || g < 0 || g > FT_MAX_FRAMES;
          if(!bad) f->group = (int)g;
          break;
        }
        case 13:
          bad = num(v, &f->share_px) ||
                !(f->share_px >= 100 && f->share_px <= 20000);
          break;
        default:
          bad = yesno(v, &f->done);
          break;
      }
    }
    if(bad) return ft_fail(e, "line %d: bad value for %s: %s", ln, k, v);
  }
  if(f && finish_frame(f, seen, e)) return -1;
  if(!strip_line) return ft_fail(e, "no [strip] section");
  if(!j->holder[0])
    return ft_fail(e, "line %d: [strip] has no holder", strip_line);
  return 0;
}

static void where(char* buf, size_t n, const ft_job_frame* f) {
  if(f->line > 0)
    snprintf(buf, n, "line %d (frame %d)", f->line, f->index);
  else
    snprintf(buf, n, "frame %d", f->index);
}

/* A share job scans each group of frames as one window: every included
 * frame is in a group, included frames follow the strip, a group's frames
 * share ppi and bin and are not split by another group's. An ordinary job
 * has no groups. */
static int share_groups(const ft_job* j, ft_err* e) {
  int i, k, prev = -1;
  char w[48];
  if(!(j->share > 0)) {
    for(i = 0; i < j->nframes; i++)
      if(j->f[i].group) {
        where(w, sizeof w, &j->f[i]);
        return ft_fail(e, "%s: group without share in [strip]", w);
      }
    for(i = 0; i < j->nframes; i++)
      if(j->f[i].share_px > 0) {
        where(w, sizeof w, &j->f[i]);
        return ft_fail(e, "%s: share_px without share in [strip]", w);
      }
    return 0;
  }
  for(i = 0; i < j->nframes; i++) {
    const ft_job_frame* f = &j->f[i];
    if(!f->include) continue;
    where(w, sizeof w, f);
    if(f->group < 1)
      return ft_fail(e, "%s: share job frame without a group", w);
    if(prev >= 0 && f->y0 < j->f[prev].y0)
      return ft_fail(e, "%s: share job frames must follow the strip", w);
    if(prev >= 0 && f->group != j->f[prev].group)
      for(k = 0; k < prev; k++)
        if(j->f[k].include && j->f[k].group == f->group)
          return ft_fail(e, "%s: group %d is split by group %d", w, f->group,
                         j->f[prev].group);
    if(prev >= 0 &&
       f->group == j->f[prev].group &&
       (f->ppi != j->f[prev].ppi || f->bin != j->f[prev].bin))
      return ft_fail(e, "%s: frames of group %d differ in ppi or bin", w,
                     f->group);
    if(prev >= 0 &&
       f->group == j->f[prev].group &&
       ft_job_share_px(j, f) != ft_job_share_px(j, &j->f[prev]))
      return ft_fail(e, "%s: frames of group %d differ in share_px", w,
                     f->group);
    prev = i;
  }
  return 0;
}

int ft_job_validate(const ft_job* j, const ft_lens* lens, ft_err* e) {
  const ft_holder* h = ft_find_holder(j->holder);
  int i;
  if(!h) return ft_fail(e, "unknown holder '%s'", j->holder);
  if(j->nframes <= 0) return ft_fail(e, "no frames");
  if(j->detail == FT_DETAIL_POS && j->film != FT_FILM_NEGATIVE)
    return ft_fail(e,
                   "detail = pos (archive positive) is for negative "
                   "film; slides: detail = neg");
  if(j->detail == FT_DETAIL_POS && j->detail_fmt != FT_DFMT_TIFF)
    return ft_fail(e, "detail = pos is always TIFF (detail_fmt = tiff)");
  if(j->detail == FT_DETAIL_NEG && j->detail_bw != FT_BW_OFF)
    return ft_fail(e, "detail_bw is for detail = pos only");
  if(j->detail == FT_DETAIL_POS && !j->has_film_base)
    return ft_fail(e, "detail = pos needs film_base (from the preview)");
  if(share_groups(j, e)) return -1;
  for(i = 0; i < j->nframes; i++) {
    const ft_job_frame* f = &j->f[i];
    long ccd, optic, off;
    char w[48];
    if(!f->include) continue;
    where(w, sizeof w, f);
    if(!isfinite(f->ppi) || !ft_ppi_supported(f->ppi))
      return ft_fail(e,
                     "%s: ppi %g not supported "
                     "(525..6300)",
                     w, f->ppi);
    if(f->bin && !ft_bin_ok(f->ppi, f->bin))
      return ft_fail(e,
                     "%s: bin %d not usable at %g ppi (the optics "
                     "ppi x bin must be 1500..6300)",
                     w, f->bin, f->ppi);
    if(!(f->len > 0 &&
         f->width > 0 &&
         f->y0 >= 0 &&
         f->x0 >= 0 &&
         f->y0 + f->len <= h->scan_length_mm &&
         f->x0 + f->width <= h->scan_width_mm))
      return ft_fail(e, "%s: window outside the holder (%.0f x %.0f mm)", w,
                     h->scan_length_mm, h->scan_width_mm);
    if(!(fabs(f->tilt_deg) <= 45))
      return ft_fail(e, "%s: tilt %g out of range (+-45 degrees)", w,
                     f->tilt_deg);
    if(j->detail != FT_DETAIL_NONE) {
      int k;
      if(f->pass < 1) return ft_fail(e, "%s: detailed scan without a pass", w);
      if(ft_job_bin(f) != 1)
        return ft_fail(e, "%s: detailed scans are bin 1", w);
      for(k = 0; k < i; k++)
        if(j->f[k].include && j->f[k].pass == f->pass && j->f[k].ppi != f->ppi)
          return ft_fail(e, "%s: pass %d mixes %g and %g ppi", w, f->pass,
                         j->f[k].ppi, f->ppi);
    }
    off = j->focus == FT_FOCUS_PER_FRAME ? f->focus_offset : j->focus_offset;
    if(off < -FT_CCD_MAX ||
       off > FT_CCD_MAX ||
       ft_optics_focus(lens, f->ppi * ft_job_bin(f), off, &ccd, &optic) ||
       !ft_positions_ok(ccd, optic))
      return ft_fail(e,
                     "%s: focus offset %ld puts the CCD/lens "
                     "outside the safe range",
                     w, off);
  }
  return 0;
}

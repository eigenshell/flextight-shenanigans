/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_share.h"

#include <math.h>
#include <string.h>

#include "ft_plan.h"

void ft_share_opts_default(ft_share_opts* o, double scan_len_mm) {
  memset(o, 0, sizeof *o);
  o->target_px = FT_SHARE_TARGET_PX;
  o->margin_mm = FT_SHARE_MARGIN_MM;
  o->scan_len_mm = scan_len_mm;
}

double ft_share_long_mm(const ft_share_frame* f, int hf_half_res) {
  if(hf_half_res && f->type == FT_FRAME_HALF) return FT_SHARE_FF_LONG_MM;
  return f->len > f->width ? f->len : f->width;
}

double ft_share_target_px(const ft_share_frame* f, const ft_share_opts* o) {
  double own = f->len > f->width ? f->len : f->width;
  if(o->hf_half_res && f->type == FT_FRAME_HALF)
    return o->target_px * own / FT_SHARE_FF_LONG_MM;
  return o->target_px;
}

double ft_share_ppi_needed(const ft_share_frame* f, int n,
                           const ft_share_opts* o) {
  double need = 0;
  int i;
  for(i = 0; i < n; i++) {
    double l = ft_share_long_mm(&f[i], o->hf_half_res);
    double p = l > 0 ? o->target_px / (l / 25.4) : 0;
    if(p > need) need = p;
  }
  return ceil(need - 1e-9);
}

/* one window over frames i..j: margins out, clipped to the holder */
static void window(const ft_share_frame* f, const ft_share_opts* o, int i,
                   int j, ft_share_window* w) {
  double a = f[i].y0 - o->margin_mm, b = f[j].y0 + f[j].len + o->margin_mm;
  if(a < 0) a = 0;
  if(b > o->scan_len_mm) b = o->scan_len_mm;
  w->y0 = a;
  w->len = b - a;
  w->first = i;
  w->last = j;
}

int ft_share_estimate(const ft_share_frame* f, int n, const ft_share_opts* o,
                      double ppi, int bin, const ft_lens* lens,
                      ft_share_est* out, ft_err* e) {
  double best[FT_MAX_FRAMES + 1], scale;
  int from[FT_MAX_FRAMES + 1], i, j, k;

  if(n < 1 || n > FT_MAX_FRAMES)
    return ft_fail(e, "share: %d frames selected (1..%d)", n, FT_MAX_FRAMES);
  if(!(o->target_px >= 100 && o->target_px <= 20000) ||
     !(o->margin_mm >= 0 && o->margin_mm <= 10) ||
     !(o->scan_len_mm > 0))
    return ft_fail(e, "share: bad options");
  for(i = 0; i < n; i++) {
    if(!(f[i].len > 0 &&
         f[i].width > 0 &&
         f[i].y0 >= 0 &&
         f[i].y0 + f[i].len <= o->scan_len_mm))
      return ft_fail(e, "share: frame %d outside the holder", f[i].index);
    if(i && !(f[i].y0 >= f[i - 1].y0 + f[i - 1].len - 1e-9))
      return ft_fail(e, "share: frame %d overlaps or precedes frame %d",
                     f[i].index, f[i - 1].index);
  }
  scale = ppi * bin;
  if(!(bin >= 1 &&
       bin <= 12 &&
       12 % bin == 0 &&
       scale >= FT_SCALE_MIN - 1e-6 &&
       scale <= FT_SCALE_MAX + 1e-6))
    return ft_fail(e,
                   "share: %g ppi x bin %d = optics %g, outside "
                   "%.0f..%.0f",
                   ppi, bin, scale, FT_SCALE_MIN, FT_SCALE_MAX);
  memset(out, 0, sizeof *out);
  out->ppi = ppi;
  out->bin = bin;
  out->scale = scale;
  out->light = ft_light_rel(lens, scale);
  out->dmax = ft_dmax_est(out->light);

  /* best[j]: cheapest scan of frames 0..j-1; from[j]: its last window's
   * first frame */
  best[0] = 0;
  for(j = 1; j <= n; j++) {
    best[j] = HUGE_VAL;
    for(i = 0; i < j; i++) {
      ft_share_window w;
      double c;
      window(f, o, i, j - 1, &w);
      c = best[i] + ft_scan_seconds(ppi, bin, w.len);
      if(c < best[j] - 1e-9) {
        best[j] = c;
        from[j] = i;
      }
    }
  }
  out->scan_s = best[n];
  /* walk back, then reverse into strip order */
  for(j = n, k = 0; j > 0; j = from[j])
    window(f, o, from[j], j - 1, &out->win[k++]);
  out->nwin = k;
  out->focus_s = (k > 0) * FT_SHARE_FOCUS_CROPS * FT_FOCUS_SAMPLE_S;
  out->seconds = out->scan_s + out->focus_s;
  for(i = 0; i < k / 2; i++) {
    ft_share_window t = out->win[i];
    out->win[i] = out->win[k - 1 - i];
    out->win[k - 1 - i] = t;
  }
  return 0;
}

int ft_share_candidates(const ft_share_frame* f, int n, const ft_share_opts* o,
                        const ft_lens* lens, ft_share_est c[6], ft_err* e) {
  static const int bins[] = {12, 6, 4, 3, 2, 1};
  double need;
  int i, m = 0;
  if(n < 1) return ft_fail(e, "share: no frames selected");
  need = ft_share_ppi_needed(f, n, o);
  if(!(need > 0)) return ft_fail(e, "share: frames have no size");
  for(i = 0; i < 6; i++) {
    double ppi = need, lo = ceil(FT_SCALE_MIN / bins[i] - 1e-9);
    if(ppi < lo) ppi = lo;
    if(ppi < 525 || ppi * bins[i] > FT_SCALE_MAX + 1e-6) continue;
    if(ft_share_estimate(f, n, o, ppi, bins[i], lens, &c[m], e)) return -1;
    m++;
  }
  if(!m)
    return ft_fail(e, "share: %g ppi is beyond the optics (max %.0f)", need,
                   FT_SCALE_MAX);
  return m;
}

int ft_share_pick_max_dmax(const ft_share_est* c, int n) {
  int i, best = 0;
  for(i = 1; i < n; i++)
    if(c[i].dmax > c[best].dmax + 1e-9 ||
       (fabs(c[i].dmax - c[best].dmax) <= 1e-9 &&
        c[i].seconds < c[best].seconds))
      best = i;
  return best;
}

/* one number at *p (digits only); -1 if none or too big */
static long number(const char** p) {
  long v = 0;
  if(**p < '0' || **p > '9') return -1;
  while(**p >= '0' && **p <= '9') {
    v = v * 10 + (**p - '0');
    if(v > 1000000) return -1;
    (*p)++;
  }
  return v;
}

static int job_has(const ft_job* j, long idx) {
  int i;
  for(i = 0; i < j->nframes; i++)
    if(j->f[i].index == idx) return 1;
  return 0;
}

int ft_share_select(const ft_job* j, const char* list, ft_share_frame* out,
                    int* n, ft_err* e) {
  unsigned char want[FT_MAX_FRAMES];
  int i, k, m = 0;
  memset(want, 0, sizeof want);
  if(!list || !*list) {
    for(i = 0; i < j->nframes; i++) want[i] = j->f[i].type != FT_FRAME_PARTIAL;
  } else {
    const char* p = list;
    for(;;) {
      long a = number(&p), b;
      if(a < 0) return ft_fail(e, "share: bad frame list '%s'", list);
      b = a;
      if(*p == '-') {
        p++;
        b = number(&p);
        if(b < a) return ft_fail(e, "share: bad frame list '%s'", list);
      }
      for(k = (int)a; k <= b; k++) {
        if(!job_has(j, k))
          return ft_fail(e, "share: the job has no frame %d", k);
        for(i = 0; i < j->nframes; i++)
          if(j->f[i].index == k) want[i] = 1;
      }
      if(!*p) break;
      if(*p++ != ',') return ft_fail(e, "share: bad frame list '%s'", list);
    }
  }
  for(i = 0; i < j->nframes; i++) {
    const ft_job_frame* f = &j->f[i];
    ft_share_frame s;
    if(!want[i]) continue;
    s.index = f->index;
    s.type = f->type;
    s.y0 = f->y0;
    s.len = f->len;
    s.x0 = f->x0;
    s.width = f->width;
    /* insertion by y0: detection writes strip order already */
    for(k = m; k > 0 && out[k - 1].y0 > s.y0; k--) out[k] = out[k - 1];
    out[k] = s;
    m++;
  }
  if(!m) return ft_fail(e, "share: no frames selected");
  *n = m;
  return 0;
}

void ft_share_apply(ft_job* j, const ft_share_frame* sel, int n,
                    const ft_share_opts* o, const ft_share_est* est) {
  int i, k, w;
  j->share = o->target_px;
  j->hf_half_res = o->hf_half_res;
  for(i = 0; i < j->nframes; i++) {
    ft_job_frame* f = &j->f[i];
    f->include = 0;
    f->group = 0;
    for(k = 0; k < n; k++)
      if(sel[k].index == f->index) break;
    if(k == n) continue;
    for(w = 0; w < est->nwin && k > est->win[w].last; w++);
    f->include = 1;
    f->ppi = est->ppi;
    f->bin = est->bin;
    f->group = w + 1;
  }
}

int ft_share_plan_sizes(ft_job* j, const double* target_px, int hf_half_res,
                        const ft_lens* lens, double scan_len_mm, ft_err* e) {
  ft_share_frame sel[FT_MAX_FRAMES];
  double tg[FT_MAX_FRAMES], least = 0;
  ft_share_est c[6];
  int i, k, m = 0, a, b, groups = 0;

  for(i = 0; i < j->nframes; i++) {
    const ft_job_frame* f = &j->f[i];
    ft_share_frame s;
    double t = target_px[i];
    if(t == 0) continue;
    if(!(t >= 100 && t <= 20000))
      return ft_fail(e,
                     "share: frame %d: %g px is out of range "
                     "(100..20000)",
                     f->index, t);
    s.index = f->index;
    s.type = f->type;
    s.y0 = f->y0;
    s.len = f->len;
    s.x0 = f->x0;
    s.width = f->width;
    for(k = m; k > 0 && sel[k - 1].y0 > s.y0; k--) {
      sel[k] = sel[k - 1];
      tg[k] = tg[k - 1];
    }
    sel[k] = s;
    tg[k] = t;
    m++;
    if(least == 0 || t < least) least = t;
  }
  if(!m) return ft_fail(e, "share: no frames selected");
  for(i = 0; i < j->nframes; i++) {
    j->f[i].include = 0;
    j->f[i].group = 0;
    j->f[i].share_px = 0;
  }
  for(a = 0; a < m; a = b + 1) {
    ft_share_opts o;
    const ft_share_est* est;
    int nc, q, w;
    for(b = a; b + 1 < m && tg[b + 1] == tg[a]; b++);
    ft_share_opts_default(&o, scan_len_mm);
    o.target_px = tg[a];
    o.hf_half_res = hf_half_res;
    nc = ft_share_candidates(sel + a, b - a + 1, &o, lens, c, e);
    if(nc < 1) return -1;
    est = &c[ft_share_pick_max_dmax(c, nc)];
    for(q = a; q <= b; q++) {
      for(w = 0; w < est->nwin && q - a > est->win[w].last; w++);
      for(i = 0; i < j->nframes; i++)
        if(j->f[i].index == sel[q].index) {
          ft_job_frame* f = &j->f[i];
          f->include = 1;
          f->ppi = est->ppi;
          f->bin = est->bin;
          f->group = groups + w + 1;
          f->share_px = tg[a] == least ? 0 : tg[a];
        }
    }
    groups += est->nwin;
  }
  j->share = least;
  j->hf_half_res = hf_half_res;
  j->focus = FT_FOCUS_PER_FRAME; /* measured on the first window */
  j->focus_offset = 0;
  return groups;
}

int ft_share_group_window(const ft_job* j, int g, const ft_share_opts* o,
                          double* y0, double* len) {
  double a = HUGE_VAL, b = -HUGE_VAL;
  int i;
  for(i = 0; i < j->nframes; i++) {
    const ft_job_frame* f = &j->f[i];
    if(!f->include || f->group != g) continue;
    if(f->y0 < a) a = f->y0;
    if(f->y0 + f->len > b) b = f->y0 + f->len;
  }
  if(a > b) return -1;
  a -= o->margin_mm;
  b += o->margin_mm;
  if(a < 0) a = 0;
  if(b > o->scan_len_mm) b = o->scan_len_mm;
  *y0 = a;
  *len = b - a;
  return 0;
}

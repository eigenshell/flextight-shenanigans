/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_dplan.h"

#include <math.h>

#include "ft_geom.h"
#include "ft_plan.h"

static double pass_window(const ft_dframe* f, int i, int j, double scan_len_mm,
                          double* y0) {
  double a = f[i].y0 - FT_DETAIL_MARGIN_MM;
  double b = f[j].y0 + f[j].len + FT_DETAIL_MARGIN_MM;
  if (a < 0) a = 0;
  if (b > scan_len_mm) b = scan_len_mm;
  *y0 = a;
  return b - a;
}

int ft_dplan_make(const ft_dframe* f, int n, double ppi, double scan_len_mm,
                  long span, ft_dplan* out, ft_err* e) {
  double best[FT_MAX_FRAMES + 1];
  int from[FT_MAX_FRAMES + 1], i, j, k, np;

  if (n < 1 || n > FT_MAX_FRAMES)
    return ft_fail(e, "detail plan: %d frames (1..%d)", n, FT_MAX_FRAMES);
  if (!ft_bin_ok(ppi, FT_DETAIL_BIN))
    return ft_fail(e,
                   "detail plan: %g ppi cannot be scanned at bin 1 "
                   "(optics %.0f..%.0f)",
                   ppi, FT_SCALE_MIN, FT_SCALE_MAX);
  if (!(span >= 0) || !(scan_len_mm > 0))
    return ft_fail(e, "detail plan: bad focus span or holder");
  for (i = 0; i < n; i++) {
    if (!(f[i].len > 0 && f[i].y0 >= 0 && f[i].y0 + f[i].len <= scan_len_mm))
      return ft_fail(e, "detail plan: frame %d is outside the holder",
                     f[i].index);
    if (i && !(f[i].y0 >= f[i - 1].y0 + f[i - 1].len))
      return ft_fail(e,
                     "detail plan: frame %d overlaps or precedes "
                     "frame %d",
                     f[i].index, f[i - 1].index);
  }
  /* best[j]: fastest plan for frames 0..j-1; from[j]: its last pass's
   * first frame */
  best[0] = 0;
  for (j = 1; j <= n; j++) {
    long lo = f[j - 1].focus, hi = lo;
    best[j] = HUGE_VAL;
    from[j] = -1;
    for (i = j; i >= 1; i--) {
      double y0, t;
      if (f[i - 1].focus < lo) lo = f[i - 1].focus;
      if (f[i - 1].focus > hi) hi = f[i - 1].focus;
      if (hi - lo > span) break; /* adding frames only widens it */
      t = best[i - 1] +
          ft_scan_seconds(ppi, FT_DETAIL_BIN,
                          pass_window(f, i - 1, j - 1, scan_len_mm, &y0));
      if (t < best[j]) {
        best[j] = t;
        from[j] = i - 1;
      }
    }
  }
  /* walk back, then reverse into strip order */
  np = 0;
  for (j = n; j > 0; j = from[j]) {
    ft_dpass* p = &out->p[np++];
    long lo, hi;
    p->first = from[j];
    p->last = j - 1;
    lo = hi = f[p->first].focus;
    for (k = p->first; k <= p->last; k++) {
      if (f[k].focus < lo) lo = f[k].focus;
      if (f[k].focus > hi) hi = f[k].focus;
    }
    p->focus = ft_dplan_mid(lo, hi);
    p->len = pass_window(f, p->first, p->last, scan_len_mm, &p->y0);
    p->seconds = ft_scan_seconds(ppi, FT_DETAIL_BIN, p->len);
  }
  for (i = 0; i < np / 2; i++) {
    ft_dpass t = out->p[i];
    out->p[i] = out->p[np - 1 - i];
    out->p[np - 1 - i] = t;
  }
  out->npass = np;
  out->seconds = best[n];
  return 0;
}

long ft_dplan_mid(long lo, long hi) {
  long s = lo + hi;
  return s >= 0 ? (s + 1) / 2 : -((-s + 1) / 2);
}

int ft_dplan_selected(const char* list, int index, ft_err* e) {
  const char* p = list;
  int hit = 0;
  if (!p || !*p) return ft_fail(e, "select: empty frame list");
  for (;;) {
    long a = 0, b;
    int digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 6) {
      a = a * 10 + (*p++ - '0');
      digits++;
    }
    if (!digits || a < 1)
      return ft_fail(e, "select: expected a frame number in '%s'", list);
    b = a;
    if (*p == '-') {
      p++;
      b = 0;
      digits = 0;
      while (*p >= '0' && *p <= '9' && digits < 6) {
        b = b * 10 + (*p++ - '0');
        digits++;
      }
      if (!digits || b < a)
        return ft_fail(e, "select: bad range in '%s'", list);
    }
    hit |= index >= a && index <= b;
    if (*p == 0) return hit;
    if (*p++ != ',')
      return ft_fail(e, "select: unexpected '%c' in '%s'", p[-1], list);
  }
}

void ft_dscan_for(double ppi, int bin, double x0, double w, double y0,
                  double len, ft_dscan* s) {
  double scale = ppi * bin;
  s->ppi = ppi;
  s->bin = bin;
  s->x_out = (long)((ft_mm_to_ccd(x0, scale) - FT_CCD_FIRST) / bin + 0.5);
  s->y_line = (long)(y0 / 25.4 * ppi + 0.5);
  s->px = (long)(w / 25.4 * ppi + 0.5);
  s->lines = (long)(len / 25.4 * ppi + 0.5);
}

void ft_dscan_reach(double ppi, int bin, double* lo, double* hi) {
  *lo = ft_ccd_to_mm(FT_CCD_FIRST, ppi * bin);
  *hi = ft_ccd_to_mm(FT_CCD_FIRST + FT_CCD_PIXELS, ppi * bin);
}

static long round_l(double v) { return (long)floor(v + 0.5); }

long ft_dscan_line(const ft_dscan* s, double y_mm) {
  /* aligned line l is green row l + k of the scan (write_crop) */
  return round_l(y_mm * s->ppi / 25.4) - s->y_line - 12 / s->bin;
}

long ft_dscan_px(const ft_dscan* s, double x_mm) {
  return round_l((ft_mm_to_ccd(x_mm, s->ppi * s->bin) - FT_CCD_FIRST) /
                 s->bin) -
         s->x_out;
}

int ft_dscan_ranges(const ft_dscan* s, double y0, double y1, double x0,
                    double x1, int r[4]) {
  long aligned = s->lines - 2 * (12 / s->bin);
  long l0 = ft_dscan_line(s, y0), l1 = ft_dscan_line(s, y1);
  long c0 = ft_dscan_px(s, x0), c1 = ft_dscan_px(s, x1);
  if (l0 < 0) l0 = 0;
  if (c0 < 0) c0 = 0;
  if (l1 > aligned) l1 = aligned;
  if (c1 > s->px) c1 = s->px;
  if (l1 <= l0 || c1 <= c0) return -1;
  r[0] = (int)l0;
  r[1] = (int)l1;
  r[2] = (int)c0;
  r[3] = (int)c1;
  return 0;
}

int ft_dscan_thumb(const ft_dscan* s) {
  long f = round_l(s->ppi / FT_DETAIL_THUMB_PPI);
  return f < 1 ? 1 : (int)f;
}

void ft_dscan_axes(const ft_dscan* s, int f, ft_axes* ax) {
  double scale = s->ppi * s->bin;
  double half = (f * s->bin - 1) / 2.0; /* centre of a box, CCD px */
  ax->y_mm_per_line = 25.4 / s->ppi * f;
  ax->y_mm0 = (s->y_line + 12 / s->bin + (f - 1) / 2.0) * 25.4 / s->ppi;
  ax->x_mm0 = ft_ccd_to_mm(ft_out_to_ccd(s->x_out, 0, s->bin) + half, scale);
  ax->x_mm_per_px =
    ft_ccd_to_mm(ft_out_to_ccd(s->x_out, f, s->bin) + half, scale) - ax->x_mm0;
}

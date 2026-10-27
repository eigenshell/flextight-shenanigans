/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_focus.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the curve: eight focus brackets 2026-10-12 (3807..6230); the
 * correction: the focus survey's tandem peaks (frame 6 centre) less its
 * 6300 peak, 0 from 3000 up (within the film's height there) */
const ft_lens ft_lens_default = {1468601.0,
                                 268105.0,
                                 317836.0,
                                 3.4799e-4,
                                 5,
                                 {1200, 1500, 2000, 3000, 6300},
                                 {-11278, -4778, -1358, 0, 0}};

double ft_lens_correction(const ft_lens* l, double scale) {
  int i, n = l->ncorr;
  if(n < 1 || n > FT_LENS_CORR_MAX) return 0;
  if(scale <= l->corr_scale[0]) return l->corr_off[0];
  for(i = 1; i < n; i++)
    if(scale <= l->corr_scale[i]) {
      double a = l->corr_scale[i - 1], b = l->corr_scale[i];
      return l->corr_off[i - 1] +
             (scale - a) / (b - a) * (l->corr_off[i] - l->corr_off[i - 1]);
    }
  return l->corr_off[n - 1];
}

double ft_lens_dof95(const ft_lens* l, double scale, int bin) {
  double m = l->p * scale;
  return FT_DOF95_K * (1 + m) / (m * m) * (bin > 0 ? bin : 1);
}

int ft_bin_for_ppi(double ppi) {
  static const int bins[] = {12, 6, 4, 3, 2, 1};
  int i;
  for(i = 0; i < 6; i++)
    if(ppi * bins[i] <= 6300 + 1e-6) return bins[i];
  return -1;
}

int ft_ppi_supported(double ppi) {
  int bin = ft_bin_for_ppi(ppi);
  double scale;
  if(ppi < 525 || bin < 0) return 0;
  scale = ppi * bin;
  return scale >= FT_SCALE_MIN - 1e-6 && scale <= FT_SCALE_MAX + 1e-6;
}

int ft_optics_for(const ft_lens* l, double scale, long* ccd, long* optic) {
  double m, u, v, lens, cc;
  if(!(scale >= FT_SCALE_MIN - 1e-6 && scale <= FT_SCALE_MAX + 1e-6)) return -1;
  if(!isfinite(l->p) ||
     !(l->p > 0) ||
     !isfinite(l->fl) ||
     !(l->fl > 0) ||
     !isfinite(l->film) ||
     !(l->film > 0) ||
     !isfinite(l->w))
    return -1;
  m = l->p * scale;
  u = l->fl * (1 + 1 / m);
  v = l->fl * (1 + m);
  lens = l->film - u + ft_lens_correction(l, scale);
  cc = lens + l->w - v;
  if(!isfinite(lens) ||
     !isfinite(cc) ||
     lens < 0 ||
     lens > 2e9 ||
     cc < 0 ||
     cc > 2e9)
    return -1;
  *optic = (long)(lens + 0.5);
  *ccd = (long)(cc + 0.5);
  return 0;
}

int ft_optics_focus(const ft_lens* l, double scale, long off, long* ccd,
                    long* optic) {
  long c, o;
  if(ft_optics_for(l, scale, &c, &o)) return -1;
  *ccd = c + off;
  *optic = o + off;
  return 0;
}

int ft_positions_ok(long ccd, long optic) {
  return ccd >= FT_CCD_MIN &&
         ccd <= FT_CCD_MAX &&
         optic >= FT_OPTIC_MIN &&
         optic <= FT_OPTIC_MAX &&
         optic - ccd >= FT_GAP_MIN;
}

/* Tenengrad on a 3x3-box-denoised image, over its mean: sobel x and y
 * on the smoothed pixels with full box support, rms magnitude / 8 (a
 * ramp's per-pixel step) / mean. Dividing by the window mean, not by
 * each pixel (log gradient), keeps dense film from inflating its noise:
 * on a dense slide the log gradient was flat across a whole focus sweep
 * (hardware 2026-10-13). */
double ft_sharpness(const float* g, int w, int h) {
  int sw = w - 2, sh = h - 2, x, y, i, j;
  float* b;
  double sum = 0, t = 0, mean;
  long n = 0;

  if(w < 5 || h < 5) return 0;
  b = malloc((size_t)sw * sh * sizeof *b);
  if(!b) return NAN;
  for(y = 0; y < sh; y++) /* b[y][x] = box at g[y+1][x+1] */
    for(x = 0; x < sw; x++) {
      double a = 0;
      for(j = 0; j < 3; j++)
        for(i = 0; i < 3; i++) a += g[(long)(y + j) * w + x + i];
      b[(long)y * sw + x] = (float)(a / 9);
      sum += a / 9;
    }
  mean = sum / ((double)sw * sh);
  for(y = 1; y < sh - 1; y++)
    for(x = 1; x < sw - 1; x++) {
      const float *u = b + (long)(y - 1) * sw + x, *m = u + sw, *d = m + sw;
      double gx = (u[1] + 2.0 * m[1] + d[1]) - (u[-1] + 2.0 * m[-1] + d[-1]);
      double gy = (d[-1] + 2.0 * d[0] + d[1]) - (u[-1] + 2.0 * u[0] + u[1]);
      t += gx * gx + gy * gy;
      n++;
    }
  free(b);
  return mean > 0 ? sqrt(t / n) / (8 * mean) : 0;
}

int ft_peak(const long* x, const double* y, int n, double* peak) {
  int i, m = 0;
  double x0, x1, x2, y0, y1, y2, num, den;
  if(n < 3) return -1;
  for(i = 0; i < n; i++)
    if(!isfinite(y[i])) return -1;
  for(i = 1; i < n; i++)
    if(y[i] > y[m]) m = i;
  if(m == 0 || m == n - 1) {
    *peak = (double)x[m];
    return 1;
  }
  x0 = x[m - 1];
  x1 = x[m];
  x2 = x[m + 1];
  y0 = y[m - 1];
  y1 = y[m];
  y2 = y[m + 1];
  num = (x1 - x0) * (x1 - x0) * (y1 - y2) - (x1 - x2) * (x1 - x2) * (y1 - y0);
  den = (x1 - x0) * (y1 - y2) - (x1 - x2) * (y1 - y0);
  *peak = den != 0 ? x1 - 0.5 * num / den : x1;
  return 0;
}

ft_focus_mode ft_focus_decide(const long off[3], long* shift) {
  double m = (off[0] + off[1] + off[2]) / 3.0;
  int i;
  *shift = (long)(m < 0 ? m - 0.5 : m + 0.5);
  for(i = 0; i < 3; i++)
    if(fabs(off[i] - m) > FT_FOCUS_MARGIN) return FT_FOCUS_PER_FRAME;
  return FT_FOCUS_SHIFTED;
}

/* parameters in comparable units: film, w, fl in Msteps, p x 1e4 */
static void lens_predict(const double q[4], double M, double* c, double* o) {
  double m = q[3] * 1e-4 * M;
  double u = q[2] * 1e6 * (1 + 1 / m), v = q[2] * 1e6 * (1 + m);
  double lens = q[0] * 1e6 - u;
  *o = lens;
  *c = lens + q[1] * 1e6 - v;
}

static double fit_cost(const double q[4], const double* M, const long* C,
                       const long* L, int n) {
  double s = 0, c, o;
  int i;
  for(i = 0; i < n; i++) {
    lens_predict(q, M[i], &c, &o);
    s += (c - C[i]) * (c - C[i]) + (o - L[i]) * (o - L[i]);
  }
  return s;
}

/* solve a x = b for 4x4 a (partial pivoting); -1 if singular */
static int solve4(double a[4][4], double b[4], double x[4]) {
  int i, j, k, p;
  for(i = 0; i < 4; i++) {
    p = i;
    for(j = i + 1; j < 4; j++)
      if(fabs(a[j][i]) > fabs(a[p][i])) p = j;
    if(fabs(a[p][i]) < 1e-300) return -1;
    for(k = 0; k < 4; k++) {
      double t = a[i][k];
      a[i][k] = a[p][k];
      a[p][k] = t;
    }
    {
      double t = b[i];
      b[i] = b[p];
      b[p] = t;
    }
    for(j = i + 1; j < 4; j++) {
      double f = a[j][i] / a[i][i];
      for(k = i; k < 4; k++) a[j][k] -= f * a[i][k];
      b[j] -= f * b[i];
    }
  }
  for(i = 3; i >= 0; i--) {
    double s = b[i];
    for(k = i + 1; k < 4; k++) s -= a[i][k] * x[k];
    x[i] = s / a[i][i];
  }
  return 0;
}

int ft_lens_fit(const double* M, const long* C, const long* L, int n,
                ft_lens* l, double* rms_ccd, double* rms_lens) {
  double q[4], lambda = 1e-3, cost;
  double sc = 0, so = 0, c, o;
  int it, i, j, k, r, stepped = 0;
  static const ft_lens zero;
  ft_lens res_l = zero; /* the fit has no correction table */
  if(n < 4) return -1;
  for(i = 0; i < n; i++)
    if(!isfinite(M[i]) || !(M[i] > 0)) return -1;
  if(!isfinite(l->film) ||
     !isfinite(l->w) ||
     !isfinite(l->fl) ||
     !isfinite(l->p) ||
     !(l->fl > 0) ||
     !(l->p > 0))
    return -1;
  q[0] = l->film / 1e6;
  q[1] = l->w / 1e6;
  q[2] = l->fl / 1e6;
  q[3] = l->p * 1e4;
  cost = fit_cost(q, M, C, L, n);
  if(!isfinite(cost)) return -1;
  for(it = 0; it < 200; it++) { /* Levenberg-Marquardt */
    double JTJ[4][4] = {{0}}, JTr[4] = {0}, step[4], qn[4], nc;
    for(i = 0; i < n; i++) {
      double c0, o0, res[2], J[2][4];
      lens_predict(q, M[i], &c0, &o0);
      res[0] = c0 - C[i];
      res[1] = o0 - L[i];
      for(k = 0; k < 4; k++) {
        double qq[4], c1, o1, h = 1e-6;
        memcpy(qq, q, sizeof qq);
        qq[k] += h;
        lens_predict(qq, M[i], &c1, &o1);
        J[0][k] = (c1 - c0) / h;
        J[1][k] = (o1 - o0) / h;
      }
      for(r = 0; r < 2; r++)
        for(j = 0; j < 4; j++) {
          JTr[j] += J[r][j] * res[r];
          for(k = 0; k < 4; k++) JTJ[j][k] += J[r][j] * J[r][k];
        }
    }
    for(j = 0; j < 4; j++) JTJ[j][j] *= 1 + lambda;
    if(solve4(JTJ, JTr, step)) {
      if(!stepped) return -1;
      break;
    }
    for(k = 0; k < 4; k++) qn[k] = q[k] - step[k];
    nc = fit_cost(qn, M, C, L, n);
    if(nc < cost) {
      int done = cost - nc < 1e-12 * cost;
      memcpy(q, qn, sizeof q);
      cost = nc;
      stepped = 1;
      lambda *= 0.3;
      if(done) break;
    } else {
      lambda *= 10;
      if(lambda > 1e10) break;
    }
  }
  res_l.film = q[0] * 1e6;
  res_l.w = q[1] * 1e6;
  res_l.fl = q[2] * 1e6;
  res_l.p = q[3] * 1e-4;
  if(!isfinite(cost) ||
     !isfinite(res_l.film) ||
     !isfinite(res_l.w) ||
     !isfinite(res_l.fl) ||
     !isfinite(res_l.p) ||
     res_l.fl <= 0 ||
     res_l.p <= 0 ||
     res_l.film <= res_l.w)
    return -1;
  for(i = 0; i < n; i++) {
    lens_predict(q, M[i], &c, &o);
    sc += (c - C[i]) * (c - C[i]);
    so += (o - L[i]) * (o - L[i]);
  }
  *l = res_l;
  *rms_ccd = sqrt(sc / n);
  *rms_lens = sqrt(so / n);
  return 0;
}

/* text[0..n) a line at a time, as fgets(line, 160) would see it: a line
 * of 159 or more characters (without its '\n') is "too long" unless it
 * is a comment */
int ft_lens_parse(ft_lens* l, const char* text, size_t n, ft_err* e) {
  static const ft_lens zero;
  ft_lens t = zero;
  size_t at = 0;
  int ln = 0, seen = 0;
  while(at < n) {
    char line[160], key[16];
    const char *s = text + at, *nl = memchr(s, '\n', n - at);
    size_t len = nl ? (size_t)(nl - s) : n - at, k = len;
    double v;
    int used = 0, over = len >= sizeof line - 1;
    const char* p = line;
    at += len + (nl != NULL);
    ln++;
    if(over) k = sizeof line - 1;
    memcpy(line, s, k);
    line[k] = 0;
    while(*p == ' ' || *p == '\t') p++;
    if(*p == '#' || *p == '\0' || *p == '\r') continue;
    if(over) return ft_fail(e, "focus file line %d: too long", ln);
    if(strncmp(p, "correct", 7) == 0 && (p[7] == ' ' || p[7] == '=')) {
      double sc, off;
      if(sscanf(p, "correct = %lf %lf %n", &sc, &off, &used) != 2 ||
         p[used] != '\0')
        return ft_fail(e,
                       "focus file line %d: expected correct = "
                       "scale offset",
                       ln);
      if(!isfinite(sc) || !isfinite(off) || !(sc > 0) || fabs(off) > 50000)
        return ft_fail(e,
                       "focus file line %d: correction out of "
                       "range",
                       ln);
      if(t.ncorr == FT_LENS_CORR_MAX)
        return ft_fail(e,
                       "focus file line %d: more than %d "
                       "corrections",
                       ln, FT_LENS_CORR_MAX);
      if(t.ncorr && !(sc > t.corr_scale[t.ncorr - 1]))
        return ft_fail(e,
                       "focus file line %d: correction scales "
                       "must ascend",
                       ln);
      t.corr_scale[t.ncorr] = sc;
      t.corr_off[t.ncorr++] = off;
      continue;
    }
    if(sscanf(p, "%15[a-z] = %lf %n", key, &v, &used) != 2 || p[used] != '\0')
      return ft_fail(e, "focus file line %d: expected key = number", ln);
    if(!isfinite(v))
      return ft_fail(e, "focus file line %d: value not finite", ln);
    if(strcmp(key, "film") == 0) {
      t.film = v;
      seen |= 1;
    } else if(strcmp(key, "w") == 0) {
      t.w = v;
      seen |= 2;
    } else if(strcmp(key, "fl") == 0) {
      t.fl = v;
      seen |= 4;
    } else if(strcmp(key, "p") == 0) {
      t.p = v;
      seen |= 8;
    } else
      return ft_fail(e, "focus file line %d: unknown key %s", ln, key);
  }
  if(seen != 15) return ft_fail(e, "focus file: needs film, w, fl and p");
  if(!(t.fl > 0) || !(t.p > 0) || !isfinite(t.film))
    return ft_fail(e, "focus file: fl and p must be positive");
  *l = t;
  return 0;
}

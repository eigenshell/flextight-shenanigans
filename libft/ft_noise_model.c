/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_noise_model.h"

#include <math.h>

#include "ft_calib.h"

/* the 0.25 DN^2 floor: at least the quantisation of a 1-DN step */
#define O_FLOOR_DN2 0.25

int ft_noise_model(const unsigned short* cal, int lines, int px, double prof[6],
                   ft_err* e) {
  signed char cls[FT_NOISE_MAX_LINES]; /* 0 dark, 1 white, -1 neither */
  double off[FT_NOISE_MAX_LINES][3];   /* line mean - its class mean */
  double lm, lo = 0, hi = 0, band;
  long n3 = (long)px * 3, i;
  int l, c, nd = 0, nw = 0, clamped = 0;

  if (lines < 1 || px < 1 || lines > FT_NOISE_MAX_LINES)
    return ft_fail(e, "noise profile: %d calibration lines", lines);
  /* line classes, as ft_split_cal */
  for (l = 0; l < lines; l++) {
    for (lm = 0, i = 0; i < n3; i++) lm += cal[l * n3 + i];
    lm /= n3;
    if (l == 0 || lm < lo) lo = lm;
    if (l == 0 || lm > hi) hi = lm;
  }
  band = FT_CAL_BAND * (hi - lo);
  for (l = 0; l < lines; l++) {
    for (lm = 0, i = 0; i < n3; i++) lm += cal[l * n3 + i];
    lm /= n3;
    cls[l] = lm < lo + band ? 0 : lm > hi - band ? 1 : -1;
    nd += cls[l] == 0;
    nw += cls[l] == 1;
  }
  if (nd < FT_NOISE_MIN_DARK || nw < FT_NOISE_MIN_WHITE)
    return ft_fail(e,
                   "noise profile: %d dark / %d white lines "
                   "(need %d / %d)",
                   nd, nw, FT_NOISE_MIN_DARK, FT_NOISE_MIN_WHITE);
  /* each line's offset from its class mean, per channel */
  for (c = 0; c < 3; c++) {
    double cm[2] = {0, 0};
    for (l = 0; l < lines; l++) {
      off[l][c] = 0;
      if (cls[l] < 0) continue;
      for (i = c; i < n3; i += 3) off[l][c] += cal[l * n3 + i];
      off[l][c] /= px;
      cm[cls[l]] += off[l][c];
    }
    cm[0] /= nd;
    cm[1] /= nw;
    for (l = 0; l < lines; l++)
      if (cls[l] >= 0) off[l][c] -= cm[cls[l]];
  }
  for (c = 0; c < 3; c++) {
    double rd2 = 0, dvar = 0, sig = 0, isp = 0, isp2 = 0, g, s, o, fl;
    long p;
    for (p = 0; p < px; p++) {
      long at = p * 3 + c;
      double m[2] = {0, 0}, v[2] = {0, 0}, span;
      for (l = 0; l < lines; l++)
        if (cls[l] >= 0) m[cls[l]] += cal[l * n3 + at];
      m[0] /= nd;
      m[1] /= nw;
      /* about each pixel's own mean (fixed pattern) and each line's
       * own offset (lamp drift): neither is pixel noise */
      for (l = 0; l < lines; l++)
        if (cls[l] >= 0) {
          double d = cal[l * n3 + at] - m[cls[l]] - off[l][c];
          v[cls[l]] += d * d;
        }
      v[0] /= nd - 1;
      v[1] /= nw - 1;
      span = m[1] - m[0];
      if (!(span >= 1))
        return ft_fail(e,
                       "noise profile: channel %d pixel %ld has "
                       "no white-dark span",
                       c, p);
      rd2 += v[0];
      dvar += v[1] - v[0];
      sig += span;
      isp += 1 / span;
      isp2 += 1 / (span * span);
    }
    rd2 /= px;
    isp /= px;
    isp2 /= px;
    g = dvar / sig; /* variance DN per DN of signal */
    s = g * isp;
    o = rd2 * (1 + 1.0 / nd) * isp2;
    fl = O_FLOOR_DN2 * isp2;
    if (!isfinite(s) || s < 0) {
      s = 0;
      clamped = 1;
    }
    if (!isfinite(o) || o < fl) {
      o = fl;
      clamped = 1;
    }
    prof[2 * c] = s;
    prof[2 * c + 1] = o;
  }
  if (clamped) {
    ft_fail(e,
            "noise profile: clamped (shot or read noise not "
            "measurable)");
    return 1;
  }
  return 0;
}

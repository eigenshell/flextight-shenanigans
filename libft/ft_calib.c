/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_calib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_geom.h"

/* each line dark (-1), white (1) or neither (0) by its mean level: within
 * FT_CAL_BAND of the range from the darkest or brightest line mean */
static int classify(const unsigned short* cal, int lines, long n3,
                    signed char* cls) {
  double *lm = malloc(lines * sizeof *lm), lo, hi, band;
  long i;
  int l;
  if (!lm) return -1;
  for (l = 0; l < lines; l++) {
    double s = 0;
    for (i = 0; i < n3; i++) s += cal[l * n3 + i];
    lm[l] = s / n3;
  }
  lo = hi = lm[0];
  for (l = 1; l < lines; l++) {
    if (lm[l] < lo) lo = lm[l];
    if (lm[l] > hi) hi = lm[l];
  }
  band = FT_CAL_BAND * (hi - lo);
  for (l = 0; l < lines; l++)
    cls[l] = lm[l] < lo + band ? -1 : lm[l] > hi - band ? 1 : 0;
  free(lm);
  return 0;
}

int ft_split_cal(const unsigned short* cal, int lines, int px, float* dark,
                 float* white, ft_err* e) {
  long n3 = (long)px * 3, i;
  double *sd, *sw;
  signed char* cls;
  int l, nd = 0, nw = 0, rc = 0;

  if (lines < 1 || px < 1) return ft_fail(e, "calibration: bad dimensions");
  cls = malloc((size_t)lines);
  sd = calloc(n3, sizeof *sd);
  sw = calloc(n3, sizeof *sw);

  if (!cls || !sd || !sw || classify(cal, lines, n3, cls)) {
    rc = ft_fail(e, "calibration: out of memory");
    goto out;
  }
  for (l = 0; l < lines; l++) {
    double* acc = cls[l] < 0 ? sd : cls[l] > 0 ? sw : NULL;
    if (!acc) continue;
    if (acc == sd)
      nd++;
    else
      nw++;
    for (i = 0; i < n3; i++) acc[i] += cal[l * n3 + i];
  }
  if (nd < 10 || nw < 10) {
    rc = ft_fail(e, "calibration: %d dark / %d white lines", nd, nw);
    goto out;
  }
  for (i = 0; i < n3; i++) {
    dark[i] = (float)(sd[i] / nd);
    white[i] = (float)(sw[i] / nw);
  }
out:
  free(cls);
  free(sd);
  free(sw);
  return rc;
}

int ft_white_var(const unsigned short* cal, int lines, int px, double var[3],
                 ft_err* e) {
  long n3 = (long)px * 3, i;
  float *dark = malloc(n3 * sizeof *dark), *white = malloc(n3 * sizeof *white);
  double *ss = calloc(n3, sizeof *ss), sum[3] = {0, 0, 0};
  signed char* cls = malloc(lines > 0 ? (size_t)lines : 1);
  int l, nw = 0, c, rc = -1;

  if (!dark || !white || !ss || !cls) {
    ft_fail(e, "calibration: out of memory");
    goto out;
  }
  if (ft_split_cal(cal, lines, px, dark, white, e)) goto out;
  if (classify(cal, lines, n3, cls)) {
    ft_fail(e, "calibration: out of memory");
    goto out;
  }
  for (l = 0; l < lines; l++) {
    if (cls[l] <= 0) continue;
    nw++;
    for (i = 0; i < n3; i++) {
      double d = cal[l * n3 + i] - white[i];
      ss[i] += d * d;
    }
  }
  for (i = 0; i < n3; i++) {
    double span = white[i] - dark[i];
    if (span < 1) span = 1;
    sum[i % 3] += ss[i] / nw / (span * span);
  }
  for (c = 0; c < 3; c++) var[c] = sum[c] / px;
  rc = 0;
out:
  free(dark);
  free(white);
  free(ss);
  free(cls);
  return rc;
}

void ft_flatfield(const unsigned short* raw, int lines, int px,
                  const float* dark, const float* white, float* t) {
  long n3 = (long)px * 3, i;
  int l;
  for (l = 0; l < lines; l++)
    for (i = 0; i < n3; i++) {
      float span = white[i] - dark[i];
      float v = (raw[l * n3 + i] - dark[i]) / (span < 1 ? 1 : span);
      t[l * n3 + i] = v < 0 ? 0 : v > 1 ? 1 : v;
    }
}

int ft_faul_parse(const char* text, size_t n, int pix[3][FT_FAUL_MAX],
                  int count[3]) {
  int ch = -1, first = 1;
  size_t i = 0;
  count[0] = count[1] = count[2] = 0;
  while (i < n) {
    char ln[64], val[16];
    size_t k = 0;
    long p;
    while (i < n && text[i] != '\n' && text[i] != '\r' && text[i] != '\0') {
      if (k < sizeof ln - 1) ln[k++] = text[i];
      i++;
    }
    i++;
    ln[k] = 0;
    if (first) { /* header line */
      first = 0;
      continue;
    }
    if (sscanf(ln, " %ld %15s", &p, val) != 2) continue;
    if (p == 100 && strcmp(val, "0") == 0) {
      ch++;
      continue;
    }
    if (ch >= 0 &&
        ch < 3 &&
        strcmp(val, "+999") == 0 &&
        count[ch] < FT_FAUL_MAX)
      pix[ch][count[ch]++] = (int)p;
  }
  return count[0] + count[1] + count[2];
}

int ft_faul_fix(float* t, int lines, int px, int pix[3][FT_FAUL_MAX],
                const int count[3], int x, int bin) {
  unsigned char* bad;
  int ch, i, j, l;
  if (bin < 1 || px < 1) return -1;
  bad = malloc(px);
  if (!bad) return -1;
  for (ch = 0; ch < 3; ch++) {
    memset(bad, 0, px);
    for (i = 0; i < count[ch]; i++) {
      long d = pix[ch][i] - (long)FT_CCD_FIRST;
      long q = (d >= 0 ? d / bin : -((-d + bin - 1) / bin)) - x;
      if (q >= 0 && q < px) bad[q] = 1;
    }
    for (j = 0; j < px; j++) {
      int lo = j - 1, hi = j + 1;
      if (!bad[j]) continue;
      while (lo >= 0 && bad[lo]) lo--;
      while (hi < px && bad[hi]) hi++;
      if (lo < 0 && hi >= px) continue;
      for (l = 0; l < lines; l++) {
        float *r = t + (long)l * px * 3, v;
        if (lo < 0)
          v = r[hi * 3 + ch];
        else if (hi >= px)
          v = r[lo * 3 + ch];
        else {
          float w = (float)(j - lo) / (hi - lo);
          v = (1 - w) * r[lo * 3 + ch] + w * r[hi * 3 + ch];
        }
        r[j * 3 + ch] = v;
      }
    }
  }
  free(bad);
  return 0;
}

int ft_align_rgb(float* t, int lines, int px, int k) {
  long n3 = (long)px * 3;
  int l, n, c;
  if (k <= 0 || lines <= 2 * k) return lines;
  n = lines - 2 * k;
  /* in place, increasing l: every source row is >= the row written */
  for (l = 0; l < n; l++)
    for (c = 0; c < px; c++) {
      t[l * n3 + c * 3 + 0] = t[(l + 2 * k) * n3 + c * 3 + 0];
      t[l * n3 + c * 3 + 1] = t[(l + k) * n3 + c * 3 + 1];
    }
  return n;
}

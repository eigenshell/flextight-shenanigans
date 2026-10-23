/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_calib.h"
#include "ft_noise_model.h"
#include "test/ftest.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

static uint32_t rng = 77;
static double urand(void) {
  rng = rng * 1664525u + 1013904223u;
  return ((rng >> 8) + 0.5) / 16777216.0;
}
static double gauss(void) {
  return sqrt(-2 * log(urand())) * cos(6.283185307179586 * urand());
}

enum { PX = 256, ND = 200, NW = 50, NL = 64 };
static const double RD = 3.0, G = 1.5; /* read sigma DN, DN var / DN */
static double dark[PX * 3], span[PX * 3];
static unsigned short cal[(ND + NW) * PX * 3], raw[NL * PX * 3];
static float dref[PX * 3], wref[PX * 3], t[NL * PX * 3];

static unsigned short dn(double mean, double sig) {
  double v = floor(mean + sig * gauss() + 0.5);
  return (unsigned short)(v < 0 ? 0 : v > 65535 ? 65535 : v);
}

/* a .cal: dark lines carry read noise only, white lines shot noise on the
 * signal above the black level too; each pixel its own level and span */
static void make_cal(int nd, int nw) {
  int l, i;
  for(l = 0; l < nd + nw; l++)
    for(i = 0; i < PX * 3; i++)
      cal[(size_t)l * PX * 3 + i] =
          l < nd ? dn(dark[i], RD)
                 : dn(dark[i] + span[i], sqrt(G * span[i] + RD * RD));
}

int main(void) {
  double prof[6];
  ft_err e;
  int i, c, lvl, rc;

  for(i = 0; i < PX * 3; i++) {
    dark[i] = 290 + 20 * urand();
    span[i] = 5500 + 1000 * urand() + 300 * (i % 3);
  }

  /* S and O within 3 % of the synthesis (rounding to DN is read noise
   * too: +1/12 DN^2) */
  make_cal(ND, NW);
  CHECK(ft_noise_model(cal, ND + NW, PX, prof, &e) == 0);
  for(c = 0; c < 3; c++) {
    double isp = 0, isp2 = 0, s, o;
    for(i = c; i < PX * 3; i += 3) {
      isp += 1 / span[i];
      isp2 += 1 / (span[i] * span[i]);
    }
    isp /= PX;
    isp2 /= PX;
    s = G * isp;
    o = (RD * RD + 1 / 12.0) * (1 + 1.0 / ND) * isp2;
    CHECK_NEAR(prof[2 * c], s, 0.03 * s);
    CHECK_NEAR(prof[2 * c + 1], o, 0.03 * o);
  }

  /* the profile predicts the variance of developed pixels: flat fields
   * at four levels through ft_split_cal + ft_flatfield, within 5 % */
  CHECK(ft_split_cal(cal, ND + NW, PX, dref, wref, &e) == 0);
  for(lvl = 0; lvl < 4; lvl++) {
    static const double tv[4] = {0.01, 0.05, 0.3, 0.85};
    for(i = 0; i < NL * PX * 3; i++) {
      int p = i % (PX * 3);
      raw[i] =
          dn(dark[p] + tv[lvl] * span[p],
             sqrt(G * tv[lvl] * span[p] + RD * RD));
    }
    ft_flatfield(raw, NL, PX, dref, wref, t);
    for(c = 0; c < 3; c++) {
      double m = 0, m2 = 0, n = 0, want;
      for(i = c; i < NL * PX * 3; i += 3) {
        m += t[i];
        m2 += (double)t[i] * t[i];
        n++;
      }
      m /= n;
      want = prof[2 * c] * tv[lvl] + prof[2 * c + 1];
      CHECK_NEAR(m2 / n - m * m, want, 0.05 * want);
    }
  }

  /* a real .cal at bin 1 (2026-10-14): a half-lit lamp-transition line
   * inside the 10 % dark band, a partly lit one after the whites, and
   * the lamp warming over the white lines. None of it is pixel noise:
   * S and O stay within 3 %. */
  make_cal(ND, NW);
  for(i = 0; i < PX * 3; i++) {
    int l;
    cal[(size_t)(ND - 1) * PX * 3 + i] =
        dn(dark[i] + (i / 3 < PX / 2 ? 0.08 : 0.02) * span[i], RD);
    for(l = ND; l < ND + NW; l++)
      cal[(size_t)l * PX * 3 + i] += (unsigned short)(l - ND) / 5;
  }
  CHECK(ft_noise_model(cal, ND + NW, PX, prof, &e) == 0);
  for(c = 0; c < 3; c++) {
    double isp = 0, isp2 = 0, s, o;
    for(i = c; i < PX * 3; i += 3) {
      isp += 1 / span[i];
      isp2 += 1 / (span[i] * span[i]);
    }
    isp /= PX;
    isp2 /= PX;
    s = G * isp;
    o = (RD * RD + 1 / 12.0) * (1 + 1.0 / (ND - 1)) * isp2;
    CHECK_NEAR(prof[2 * c], s, 0.03 * s);
    CHECK_NEAR(prof[2 * c + 1], o, 0.03 * o);
  }

  /* too few dark lines: no profile */
  make_cal(40, NW);
  CHECK(ft_noise_model(cal, 40 + NW, PX, prof, &e) == -1);
  CHECK(strstr(e.msg, "40 dark") != NULL);

  /* no noise at all: S = 0, O at the 0.25 DN^2 floor, flagged */
  for(i = 0; i < (ND + NW) * PX * 3; i++)
    cal[i] = (unsigned short)(i < ND * PX * 3 ? 300 : 6300);
  rc = ft_noise_model(cal, ND + NW, PX, prof, &e);
  CHECK(rc == 1 && strstr(e.msg, "clamped") != NULL);
  for(c = 0; c < 3; c++) {
    CHECK(prof[2 * c] == 0);
    CHECK_NEAR(prof[2 * c + 1], 0.25 / (6000.0 * 6000.0), 1e-15);
  }
  return TEST_DONE();
}

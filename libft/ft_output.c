/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_output.h"

#include <math.h>
#include <string.h>

#define PI 3.14159265358979323846

static const struct {
  const char *name, *what;
  double xy[8];
} SPACES[] = {
  {"srgb",    "sRGB",    {0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290}},
  {"p3",
   "Display P3",         {0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290}},
  {"adobe",
   "Adobe RGB (1998)",   {0.640, 0.330, 0.210, 0.710, 0.150, 0.060, 0.3127, 0.3290}},
  {"rec2020",
   "Rec. ITU-R BT.2020", {0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290}},
};
#define NSPACES 4

int ft_space_find(const char* name, ft_space* s) {
  int i;
  for (i = 0; name && i < NSPACES; i++)
    if (strcmp(name, SPACES[i].name) == 0) {
      *s = (ft_space)i;
      return 0;
    }
  return -1;
}

const char* ft_space_name(ft_space s) {
  return (unsigned)s < NSPACES ? SPACES[s].name : "?";
}

const char* ft_space_what(ft_space s) {
  return (unsigned)s < NSPACES ? SPACES[s].what : "?";
}

int ft_gamut_find(const char* name, ft_gamut* g) {
  if (name && strcmp(name, "soft") == 0) {
    *g = FT_GAMUT_SOFT;
    return 0;
  }
  if (name && strcmp(name, "clip") == 0) {
    *g = FT_GAMUT_CLIP;
    return 0;
  }
  return -1;
}

void ft_space_primaries(ft_space s, double xy[8]) {
  memcpy(xy, SPACES[(unsigned)s < NSPACES ? s : 0].xy, 8 * sizeof *xy);
}

int ft_mat3_inv(const double m[9], double r[9]) {
  double d = m[0] * (m[4] * m[8] - m[5] * m[7]) -
             m[1] * (m[3] * m[8] - m[5] * m[6]) +
             m[2] * (m[3] * m[7] - m[4] * m[6]);
  if (!(fabs(d) > 1e-12)) return -1;
  r[0] = (m[4] * m[8] - m[5] * m[7]) / d;
  r[1] = (m[2] * m[7] - m[1] * m[8]) / d;
  r[2] = (m[1] * m[5] - m[2] * m[4]) / d;
  r[3] = (m[5] * m[6] - m[3] * m[8]) / d;
  r[4] = (m[0] * m[8] - m[2] * m[6]) / d;
  r[5] = (m[2] * m[3] - m[0] * m[5]) / d;
  r[6] = (m[3] * m[7] - m[4] * m[6]) / d;
  r[7] = (m[1] * m[6] - m[0] * m[7]) / d;
  r[8] = (m[0] * m[4] - m[1] * m[3]) / d;
  return 0;
}

static void mul3(const double a[9], const double b[9], double r[9]) {
  int i, j;
  for (i = 0; i < 3; i++)
    for (j = 0; j < 3; j++)
      r[3 * i + j] =
        a[3 * i] * b[j] + a[3 * i + 1] * b[3 + j] + a[3 * i + 2] * b[6 + j];
}

static void apply3(const double m[9], const double v[3], double r[3]) {
  r[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
  r[1] = m[3] * v[0] + m[4] * v[1] + m[5] * v[2];
  r[2] = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
}

void ft_rgb_to_xyz(const double xy[8], double m[9]) {
  /* columns: each primary's XYZ (Y = 1), scaled so R + G + B = white */
  double p[9], inv[9], w[3], s[3];
  int i;
  for (i = 0; i < 3; i++) {
    double x = xy[2 * i], y = xy[2 * i + 1];
    p[i] = x / y;
    p[3 + i] = 1;
    p[6 + i] = (1 - x - y) / y;
  }
  w[0] = xy[6] / xy[7];
  w[1] = 1;
  w[2] = (1 - xy[6] - xy[7]) / xy[7];
  ft_mat3_inv(p, inv);
  apply3(inv, w, s);
  for (i = 0; i < 9; i++) m[i] = p[i] * s[i % 3];
}

double ft_space_encode(ft_space s, double v) {
  if (!(v > 0)) return 0;
  if (v >= 1) return 1; /* exact: 1.055 - 0.055 is not 1 in double */
  if (s == FT_SPACE_ADOBE_RGB) return pow(v, 256.0 / 563.0);
  if (s == FT_SPACE_REC2020) /* BT.2020 / BT.709 curve */
    return v < 0.018053968510807
             ? 4.5 * v
             : 1.09929682680944 * pow(v, 0.45) - 0.09929682680944;
  return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1 / 2.4) - 0.055;
}

double ft_space_decode(ft_space s, double v) {
  if (!(v > 0)) return 0;
  if (v >= 1) return 1;
  if (s == FT_SPACE_ADOBE_RGB) return pow(v, 563.0 / 256.0);
  if (s == FT_SPACE_REC2020)
    return v < 0.081242858298635
             ? v / 4.5
             : pow((v + 0.09929682680944) / 1.09929682680944, 1 / 0.45);
  return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

/* Ottosson's M1 (XYZ D65 -> LMS) with each row scaled (by 1.0001,
 * 0.99997, 0.99952) so that D65 from its xy (0.3127, 0.3290), the white
 * of every space here, is LMS 1, 1, 1: greys get a = b = 0 exactly (the
 * published M1 gives white a chroma of ~1e-4, which the gamut step near
 * L = 1 would tint). M2: LMS' -> Lab as published. */
static const double M1[9] = {
  0.8190224426, 0.3619062605, -0.1288737860, 0.0329836559, 0.9292868603,
  0.0361446659, 0.0481771938, 0.2642395277,  0.6335478283,
};
static const double M2[9] = {
  0.2104542553, 0.7936177850, -0.0040720468, 1.9779984951,  -2.4285922050,
  0.4505937099, 0.0259040371, 0.7827717662,  -0.8086757660,
};

/* the exact inverses of M1 and M2 above (computed once, in rational
 * arithmetic, then rounded to double) */
static const double M1I[9] = {
  1.2268798683314697,    -0.55781499359524622, 0.28139105244024526,
  -0.040575747350531759, 1.1122868053781236,   -0.071711058134508787,
  -0.076372944091338227, -0.42149332243093923, 1.5869240173438854,
};
static const double M2I[9] = {
  0.9999999984505199, 0.39633779217376786,   0.21580375806075877,
  1.0000000088817607, -0.10556134232365634,  -0.063854174771705893,
  1.0000000546724108, -0.089484182094965753, -1.2914855378640917,
};

void ft_xyz_to_oklab(const double xyz[3], double lab[3]) {
  double lms[3];
  apply3(M1, xyz, lms);
  lms[0] = cbrt(lms[0]);
  lms[1] = cbrt(lms[1]);
  lms[2] = cbrt(lms[2]);
  apply3(M2, lms, lab);
}

void ft_oklab_to_xyz(const double lab[3], double xyz[3]) {
  double lms[3];
  apply3(M2I, lab, lms);
  lms[0] = lms[0] * lms[0] * lms[0];
  lms[1] = lms[1] * lms[1] * lms[1];
  lms[2] = lms[2] * lms[2] * lms[2];
  apply3(M1I, lms, xyz);
}

/* largest chroma at (L, h) inside the gamut of from_xyz, by bisection.
 * The slack scales with the luminance (Y ~ L^3): an absolute one would
 * stretch the edge by tens of percent near black. */
static double boundary(const double from_xyz[9], double L, double h) {
  double lo = 0, hi = 0.6, ch = cos(h), sh = sin(h);
  double tol = 1e-7 * L * L * L;
  int i, k;
  if (L <= 0 || L >= 1) return 0;
  if (tol < 1e-12) tol = 1e-12;
  for (i = 0; i < 40; i++) {
    double c = 0.5 * (lo + hi), lab[3], xyz[3], rgb[3];
    int in = 1;
    lab[0] = L;
    lab[1] = c * ch;
    lab[2] = c * sh;
    ft_oklab_to_xyz(lab, xyz);
    apply3(from_xyz, xyz, rgb);
    for (k = 0; k < 3; k++) in &= rgb[k] >= -tol && rgb[k] <= 1 + tol;
    if (in)
      lo = c;
    else
      hi = c;
  }
  return lo;
}

int ft_output_compile(ft_space s, ft_gamut g, ft_output* o, ft_err* e) {
  double xy[8], t2x[9], x2t[9], r2x[9], x2r[9], m[9];
  int i, j;
  if ((unsigned)s >= NSPACES)
    return ft_fail(e, "output: unknown colour space %d", (int)s);
  if (g != FT_GAMUT_SOFT && g != FT_GAMUT_CLIP)
    return ft_fail(e, "output: unknown gamut mode %d", (int)g);
  memset(o, 0, sizeof *o);
  o->space = s;
  o->gamut = g;
  ft_space_primaries(s, xy);
  ft_rgb_to_xyz(xy, t2x);
  ft_mat3_inv(t2x, x2t);
  ft_space_primaries(FT_SPACE_REC2020, xy);
  ft_rgb_to_xyz(xy, r2x);
  ft_mat3_inv(r2x, x2r);
  mul3(x2t, r2x, m);
  for (i = 0; i < 9; i++) {
    o->m[i] = (float)m[i];
    o->to_xyz[i] = t2x[i];
    o->from_xyz[i] = x2t[i];
  }
  if (g == FT_GAMUT_SOFT)
    for (i = 0; i <= FT_OUTPUT_NL; i++)
      for (j = 0; j <= FT_OUTPUT_NH; j++) {
        double L = (double)i / FT_OUTPUT_NL;
        double h = 2 * PI * j / FT_OUTPUT_NH;
        o->cmax[i][j] = (float)boundary(x2t, L, h);
        o->csrc[i][j] = (float)boundary(x2r, L, h);
      }
  for (i = 0; i < FT_OUTPUT_TRC_N; i++) {
    double x = (double)i / (FT_OUTPUT_TRC_N - 1);
    o->trc[i] = (float)ft_space_encode(s, x * x);
  }
  return 0;
}

/* bilinear in a boundary table; h in radians, any range */
static double table(const float t[FT_OUTPUT_NL + 1][FT_OUTPUT_NH + 1], double L,
                    double h) {
  double x = L * FT_OUTPUT_NL, y = h / (2 * PI) * FT_OUTPUT_NH, fx, fy;
  int i, j;
  if (x - x != 0 || y - y != 0) return 0;
  y -= floor(y / FT_OUTPUT_NH) * FT_OUTPUT_NH;
  if (x < 0) x = 0;
  if (x > FT_OUTPUT_NL) x = FT_OUTPUT_NL;
  i = (int)x;
  j = (int)y;
  if (i >= FT_OUTPUT_NL) i = FT_OUTPUT_NL - 1;
  if (j >= FT_OUTPUT_NH) j = FT_OUTPUT_NH - 1;
  fx = x - i;
  fy = y - j;
  return (1 - fx) * ((1 - fy) * t[i][j] + fy * t[i][j + 1]) +
         fx * ((1 - fy) * t[i + 1][j] + fy * t[i + 1][j + 1]);
}

/* chroma c above the knee k compressed so that src maps onto dst (the
 * ACES gamut compressor's power curve, p = 1.2): slope 1 at the knee,
 * continuous, strictly increasing, below dst for every c */
static double compress(double c, double k, double src, double dst) {
  double d = c - k, D = src - k, R = dst - k, p = 1.2, s;
  if (d <= 0) return c;
  if (R <= 0) return dst;
  if (D <= R) /* the source fits: only clamp */
    return c < dst ? c : dst;
  s = D / pow(pow(D / R, p) - 1, 1 / p);
  return k + d / pow(1 + pow(d / s, p), 1 / p);
}

static void clamp3(float rgb[3]) {
  int i;
  for (i = 0; i < 3; i++) rgb[i] = rgb[i] > 0 ? (rgb[i] < 1 ? rgb[i] : 1) : 0;
}

void ft_output_gamut(const ft_output* o, float rgb[3]) {
  double v[3], xyz[3], lab[3], L, C, h, cm, cs, k;
  /* NaN or inf in: black (x - x is 0 only for finite x) */
  if (rgb[0] - rgb[0] != 0 || rgb[1] - rgb[1] != 0 || rgb[2] - rgb[2] != 0) {
    rgb[0] = rgb[1] = rgb[2] = 0;
    return;
  }
  if (o->gamut == FT_GAMUT_CLIP || o->space == FT_SPACE_REC2020) {
    clamp3(rgb);
    return;
  }
  v[0] = rgb[0];
  v[1] = rgb[1];
  v[2] = rgb[2];
  apply3(o->to_xyz, v, xyz);
  ft_xyz_to_oklab(xyz, lab);
  C = sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
  h = atan2(lab[2], lab[1]);
  /* a huge float overflows the cube root's range: also black */
  if (C - C != 0 || h - h != 0 || lab[0] - lab[0] != 0) {
    rgb[0] = rgb[1] = rgb[2] = 0;
    return;
  }
  L = lab[0] > 0 ? (lab[0] < 1 ? lab[0] : 1) : 0;
  cm = table(o->cmax, L, h);
  k = FT_OUTPUT_KNEE * cm;
  if (C <= k || C <= FT_OUTPUT_C_EPS) { /* untouched if it fits */
    clamp3(rgb);
    return;
  }
  cs = table(o->csrc, L, h);
  if (cs < C) cs = C; /* beyond Rec.2020: compress from there */
  C = compress(C, k, cs, cm);
  lab[0] = L;
  lab[1] = C * cos(h);
  lab[2] = C * sin(h);
  ft_oklab_to_xyz(lab, xyz);
  apply3(o->from_xyz, xyz, v);
  rgb[0] = (float)v[0];
  rgb[1] = (float)v[1];
  rgb[2] = (float)v[2];
  clamp3(rgb); /* table interpolation error */
}

static float encode(const ft_output* o, float v) {
  float x, f;
  int i;
  if (!(v > 0)) return o->trc[0];
  if (v >= 1) return o->trc[FT_OUTPUT_TRC_N - 1];
  x = sqrtf(v) * (FT_OUTPUT_TRC_N - 1);
  i = (int)x;
  if (i >= FT_OUTPUT_TRC_N - 1) return o->trc[FT_OUTPUT_TRC_N - 1];
  f = x - (float)i;
  return o->trc[i] + f * (o->trc[i + 1] - o->trc[i]);
}

void ft_output_apply(const ft_output* o, const float* in, float* out,
                     size_t npx) {
  size_t p;
  for (p = 0; p < npx; p++) {
    float r = in[3 * p], g = in[3 * p + 1], b = in[3 * p + 2], v[3];
    int c;
    /* NaN or inf anywhere: black (x - x is 0 only for finite x) */
    if (r - r != 0 || g - g != 0 || b - b != 0) r = g = b = 0;
    v[0] = o->m[0] * r + o->m[1] * g + o->m[2] * b;
    v[1] = o->m[3] * r + o->m[4] * g + o->m[5] * b;
    v[2] = o->m[6] * r + o->m[7] * g + o->m[8] * b;
    ft_output_gamut(o, v);
    for (c = 0; c < 3; c++) out[3 * p + c] = encode(o, v[c]);
  }
}

/* lowbias32 (Chris Wellons): a well-mixed 32-bit hash */
static unsigned long hash32(unsigned long x) {
  x &= 0xffffffffUL;
  x ^= x >> 16;
  x = (x * 0x7feb352dUL) & 0xffffffffUL;
  x ^= x >> 15;
  x = (x * 0x846ca68bUL) & 0xffffffffUL;
  x ^= x >> 16;
  return x;
}

void ft_output_quantise8(const float* enc, unsigned char* out, size_t npx,
                         size_t first_px) {
  size_t k;
  for (k = 0; k < npx * 3; k++) {
    float v = enc[k];
    double q;
    if (!(v > 0)) {
      out[k] = 0;
      continue;
    }
    if (v >= 1) {
      out[k] = 255;
      continue;
    }
    {
      unsigned long n = (unsigned long)(first_px * 3 + k);
      double a = hash32(2 * n) / 4294967296.0;
      double b = hash32(2 * n + 1) / 4294967296.0;
      q = floor(v * 255.0 + 0.5 + (a - b)); /* TPDF, +-1 LSB */
    }
    out[k] = (unsigned char)(q < 0 ? 0 : q > 255 ? 255 : q);
  }
}

void ft_output_quantise16(const float* enc, unsigned short* out, size_t npx) {
  size_t k;
  for (k = 0; k < npx * 3; k++) {
    float v = enc[k];
    out[k] = (unsigned short)(!(v > 0) ? 0
                              : v >= 1 ? 65535
                                       : (unsigned)(v * 65535.0f + 0.5f));
  }
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Frame detection on a flat-fielded preview. Outline (see the design,
 * "Frame detection"):
 *  1. lines: open holder / holder metal / film; film = first..last film line
 *  2. columns over film lines: image band between the hole rows
 *  3. base level per ~10 mm along, from the rebate between the holes
 *     (pixels well away from open beam), robust median + MAD noise
 *  4. per pixel: exposed / base-like / skip (open beam and its halo)
 *  5. strip tilt from the two hole rows; everything below works on lines
 *     deskewed by that tilt
 *  6. gap lines (>= 97 % of image columns base-like) and straight edges
 *  7. exposed runs between trusted gaps -> frames; long untrusted
 *     stretches are gridded and flagged; long gaps count blank frames
 */
#include "ft_detect.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846
#define OPEN_T 0.85f /* all channels above: open beam */
/* All channels below: holder metal (or Dmax). Metal reads ~0.0006-0.0014
 * per channel with noise on the hardware (2026-10-12); at 0.005, 95 % of
 * its pixels count, against at most 14 % on any colour-negative film line.
 * Slide Dmax lines can be as dark: hole-row light keeps them film. */
#define METAL_T 0.005f
#define LINE_FRAC 0.80 /* line is open / metal when this much of it is */
#define HOLE_LINE 0.02 /* open share of a line that marks hole rows */
/* Narrowest hole row in view. The 24x217 window hides most of each 2.8 mm
 * hole row: 0.7-1.6 mm of them show, the narrow side moving with where
 * the strip lies across (hardware 2026-10-12/13). Blur and the open halo
 * are ~0.3 mm; no other column run is 25-90 % open over the film. */
#define HOLE_MIN_MM 0.5
#define BASE_DENS 0.08 /* log10 density within this of the base */
/* Additive noise floor on the base-like test, in units of the base noise
 * (MAD of the rebate). Needed for slide Dmax (~0.0005) where sensor noise
 * is larger than 0.08 density; 4 sigma keeps false exposed pixels < 1e-4. */
#define K_SIGMA 4.0
/* Open-beam halo excluded from the base sample and the gap test: blur
 * spreads hole and speck light ~0.1-0.15 mm; 0.3 mm leaves margin. */
#define DIL_MM 0.3
/* Local base segment: absorbs base drift along the roll (+-10 % over the
 * strip changes the base < 3 % within 10 mm, well inside BASE_DENS). */
#define SEG_MM 10.0
#define SEG_MIN 200   /* samples for a segment to count */
#define GAP_FRAC 0.97 /* image columns base-like on a gap line */
#define NEAR_GAP 0.85
#define EDGE_WIN_MM 3.0
#define EDGE_FOUND 0.80  /* inlier columns needed for an edge */
#define EDGE_RMS_MM 0.25 /* inlier rms of the edge line */
#define EDGE_OUTER 0.40  /* inliers of an outermost-cluster edge */
#define EDGE_BEYOND 0.02 /* columns allowed past it, into the gap */
/* Smallest inlier band of the robust edge fit: blur + grain jitter a
 * straight edge by 1-2 lines per column; 0.15 mm (3 lines) keeps those. */
#define EDGE_CLIP_MM 0.15
#define MIN_GAP_MM 0.2
#define MIN_RUN_MM 6.0
#define END_SLIVER 3.0
/* Strip ends: lines within END_CUT_MM whose image band is at least
 * CUT_OPEN open beam are past the film (a cut on the diagonal, hardware
 * 2026-10-12), never frame. Dust specks cover ~1 % of the band. */
#define END_CUT_MM 10.0
#define CUT_OPEN 0.05
/* A line whose image band is at least this much open beam has no film on
 * it (open holder between loose pieces, hardware 2026-10-12): it counts as
 * a gap line, never as a frame or a blank frame. */
#define NOFILM_OPEN 0.50
#define BLANK_MM 12.0
#define FALLBACK_MM 45.0
/* A run longer than this without a trusted edge inside is gridded and
 * flagged. Design suggests ~70 mm, but swing-lens panoramas reach 66 mm
 * and the test strip has a 100 mm panorama; 110 mm separates that from
 * three touching frames (114 mm). */
#define GRID_RUN_MM 110.0
/* Real panoramas on 35 mm: Horizon/Widelux 58-59 mm, XPan 65, Noblex 66.
 * A longer run between trusted gaps is more likely frames whose gap was
 * missed (touching frames, a light leak across the gap): flagged low. */
#define PANO_MIN_MM 55.0
#define PANO_MAX_MM 70.0
#define MAX_TILT_DEG 2.0
#define MIN_TILT_DEG 0.05 /* edge-based tilt below this is ignored */

enum { S_EXP = 0, S_BASE = 1, S_SKIP = 2 };

typedef struct {
  int a, b; /* gap lines [a, b), deskewed */
  int lead_ok, trail_ok, ok;
  int nofilm;                     /* open holder in it: not blank film */
  double lead_pos, trail_pos;     /* edge line at the image centre */
  double lead_slope, trail_slope; /* residual, mm along per mm across */
} gap_t;

typedef struct {
  const float* t;
  int lines, px;
  double ym, xm;
  ft_film film;
  int f0, f1;            /* film lines */
  int b0, b1;            /* image band (between hole rows) */
  int hl0, hr1;          /* outer hole-row columns */
  int ic0, ic1;          /* image columns */
  unsigned char* st;     /* S_* per pixel (film lines) */
  int* off;              /* deskew offset per column, lines */
  double tn;             /* tan(strip tilt) */
  double* g;             /* gap score per deskewed line */
  unsigned char* nofilm; /* per line: band mostly open beam */
  gap_t* gaps;
  int ng;
} det_t;

static const float* pix(const float* t, int px, int l, int c) {
  return t + ((long)l * px + c) * 3;
}

static int is_open(const float* p) {
  return p[0] > OPEN_T && p[1] > OPEN_T && p[2] > OPEN_T;
}

static int is_dark(const float* p) {
  return p[0] < METAL_T && p[1] < METAL_T && p[2] < METAL_T;
}

/* state at deskewed line l, column c */
/* open-beam share of line l across the image band */
static double band_open(const det_t* d, int l) {
  int c, n = 0;
  for(c = d->b0; c < d->b1; c++) n += is_open(pix(d->t, d->px, l, c));
  return d->b1 > d->b0 ? (double)n / (d->b1 - d->b0) : 0;
}

static int S(const det_t* d, int l, int c) {
  int ll = l + d->off[c];
  if(ll < d->f0 || ll >= d->f1) return S_SKIP;
  return d->st[(long)ll * d->px + c];
}

/* k-th smallest of v[0..n) (reorders v) */
static double kth(float* v, long n, long k) {
  long lo = 0, hi = n - 1;
  while(lo < hi) {
    float piv = v[(lo + hi) / 2], tmp;
    long i = lo, j = hi;
    while(i <= j) {
      while(v[i] < piv) i++;
      while(v[j] > piv) j--;
      if(i <= j) {
        tmp = v[i];
        v[i] = v[j];
        v[j] = tmp;
        i++;
        j--;
      }
    }
    if(k <= j)
      hi = j;
    else if(k >= i)
      lo = i;
    else
      break;
  }
  return v[k];
}

static double dkth(double* v, int n, int k) {
  int lo = 0, hi = n - 1;
  while(lo < hi) {
    double piv = v[(lo + hi) / 2], tmp;
    int i = lo, j = hi;
    while(i <= j) {
      while(v[i] < piv) i++;
      while(v[j] > piv) j--;
      if(i <= j) {
        tmp = v[i];
        v[i] = v[j];
        v[j] = tmp;
        i++;
        j--;
      }
    }
    if(k <= j)
      hi = j;
    else if(k >= i)
      lo = i;
    else
      break;
  }
  return v[k];
}

/* median and 1.4826 * MAD of v[0..n) (reorders v) */
static void med_mad(float* v, long n, double* med, double* sig) {
  long i;
  *med = kth(v, n, n / 2);
  for(i = 0; i < n; i++) v[i] = (float)fabs(v[i] - *med);
  *sig = 1.4826 * kth(v, n, n / 2);
}

/* widest run of i in [0, n) with lo <= v[i] < hi, as [*r0, *r1) */
static int widest_run(const double* v, int n, double lo, double hi, int* r0,
                      int* r1) {
  int i, s = -1, best = 0;
  *r0 = *r1 = 0;
  for(i = 0; i <= n; i++) {
    int in = i < n && v[i] >= lo && v[i] < hi;
    if(in && s < 0) s = i;
    if(!in && s >= 0) {
      if(i - s > best) {
        best = i - s;
        *r0 = s;
        *r1 = i;
      }
      s = -1;
    }
  }
  return best;
}

/* walk from `from` in direction dir while lo <= v < hi; first index outside */
static int grow(const double* v, int n, int from, int dir, double lo,
                double hi) {
  int i = from;
  while(i >= 0 && i < n && v[i] >= lo && v[i] < hi) i += dir;
  return i;
}

/* Mark pixels within DIL_MM of an open pixel (film lines only), as a
 * separable box dilation with running counts. */
static unsigned char* open_halo(const det_t* d) {
  int px = d->px, rx = (int)ceil(DIL_MM / d->xm),
      ry = (int)ceil(DIL_MM / d->ym);
  long n = (long)d->lines * px;
  unsigned char *h = calloc(n, 1), *out = calloc(n, 1);
  int l, c;
  if(!h || !out) {
    free(h);
    free(out);
    return NULL;
  }
  for(l = d->f0; l < d->f1; l++) {
    int cnt = 0;
    for(c = 0; c < rx && c < px; c++) cnt += is_open(pix(d->t, px, l, c));
    for(c = 0; c < px; c++) {
      if(c + rx < px) cnt += is_open(pix(d->t, px, l, c + rx));
      if(c - rx - 1 >= 0) cnt -= is_open(pix(d->t, px, l, c - rx - 1));
      h[(long)l * px + c] = cnt > 0;
    }
  }
  for(c = 0; c < px; c++) {
    int cnt = 0;
    for(l = d->f0; l < d->f0 + ry && l < d->f1; l++) cnt += h[(long)l * px + c];
    for(l = d->f0; l < d->f1; l++) {
      if(l + ry < d->f1) cnt += h[(long)(l + ry) * px + c];
      if(l - ry - 1 >= d->f0) cnt -= h[(long)(l - ry - 1) * px + c];
      out[(long)l * px + c] = cnt > 0;
    }
  }
  free(h);
  return out;
}

/*
 * Base level per line. With hole rows: the rebate between the holes in the
 * hole-row columns is unexposed film everywhere along the strip (frames do
 * not reach it), so its median per SEG_MM segment is the local base and
 * its MAD the base noise. Without hole rows (cropped preview): one global
 * value, the median of the extreme cluster (brightest on negative,
 * darkest on slide) of all band pixels. Fills base_l (3 per film line),
 * L->base and sig[3].
 */
static int base_levels(const det_t* d, const unsigned char* halo, int has_holes,
                       float* base_l, float lbase[3], double sig[3]) {
  int px = d->px, nl = d->f1 - d->f0;
  int segl = (int)(SEG_MM / d->ym + 0.5), nseg, i, k, l, c, nv = 0;
  int ncol = has_holes ? (d->b0 - d->hl0) + (d->hr1 - d->b1) : d->b1 - d->b0;
  long cap;
  float* buf[3];
  double *sb, *ss, *tmp;
  int* ok;

  if(segl < 1) segl = 1;
  nseg = (nl + segl - 1) / segl;
  if(!has_holes) segl = nseg = 0;
  cap = (long)(has_holes ? segl : nl) * (ncol > 0 ? ncol : 1);
  sb = calloc((size_t)(nseg + 1) * 3, sizeof *sb);
  ss = calloc((size_t)(nseg + 1) * 3, sizeof *ss);
  tmp = calloc((size_t)(nseg + 1), sizeof *tmp);
  ok = calloc((size_t)(nseg + 1), sizeof *ok);
  for(k = 0; k < 3; k++) buf[k] = malloc((size_t)cap * sizeof(float));
  if(!sb || !ss || !tmp || !ok || !buf[0] || !buf[1] || !buf[2]) {
    free(sb);
    free(ss);
    free(tmp);
    free(ok);
    for(k = 0; k < 3; k++) free(buf[k]);
    return -1;
  }

  if(has_holes) {
    for(i = 0; i < nseg; i++) {
      long n = 0;
      int l0 = d->f0 + i * segl, l1 = l0 + segl > d->f1 ? d->f1 : l0 + segl;
      for(l = l0; l < l1; l++)
        for(c = d->hl0; c < d->hr1; c++) {
          const float* p;
          if(c == d->b0) c = d->b1; /* skip the image band */
          p = pix(d->t, px, l, c);
          if(halo[(long)l * px + c] || is_open(p)) continue;
          for(k = 0; k < 3; k++) buf[k][n] = p[k];
          n++;
        }
      if(n < SEG_MIN) continue;
      for(k = 0; k < 3; k++) med_mad(buf[k], n, &sb[i * 3 + k], &ss[i * 3 + k]);
      ok[i] = 1;
      nv++;
    }
  }
  if(nv == 0) {
    /* global extreme cluster over the band */
    long n = 0, m;
    /* line stride so the whole strip fits the sample buffer */
    int ls = (int)(((long)nl * (d->b1 - d->b0) + cap - 1) / cap);
    has_holes = 0;
    for(l = d->f0; l < d->f1; l += ls > 0 ? ls : 1)
      for(c = d->b0; c < d->b1; c++) {
        const float* p = pix(d->t, px, l, c);
        if(halo[(long)l * px + c] || is_open(p) || n >= cap) continue;
        for(k = 0; k < 3; k++) buf[k][n] = p[k];
        n++;
      }
    if(n < SEG_MIN) {
      free(sb);
      free(ss);
      free(tmp);
      free(ok);
      for(k = 0; k < 3; k++) free(buf[k]);
      return -1;
    }
    for(k = 0; k < 3; k++) {
      double ext =
        kth(buf[k], n,
            d->film == FT_FILM_NEGATIVE ? (long)(n * 0.99) : (long)(n * 0.01));
      double lo = d->film == FT_FILM_NEGATIVE ? ext * pow(10, -0.1) : 0;
      double hi = d->film == FT_FILM_NEGATIVE ? 2 : ext * pow(10, 0.1);
      for(i = 0, m = 0; i < n; i++)
        if(buf[k][i] >= lo && buf[k][i] <= hi) buf[k][m++] = buf[k][i];
      med_mad(buf[k], m, &sb[k], &ss[k]);
    }
    nseg = 1;
    ok[0] = 1;
  }

  /* fill segments without samples from the nearest good one */
  for(i = 0; i < nseg; i++)
    if(!ok[i]) {
      int j, best = -1;
      for(j = 0; j < nseg; j++)
        if(ok[j] && (best < 0 || abs(j - i) < abs(best - i))) best = j;
      for(k = 0; k < 3; k++) {
        sb[i * 3 + k] = sb[best * 3 + k];
        ss[i * 3 + k] = ss[best * 3 + k];
      }
    }
  for(k = 0; k < 3; k++) {
    for(i = 0; i < nseg; i++) tmp[i] = sb[i * 3 + k];
    lbase[k] = (float)dkth(tmp, nseg, nseg / 2);
    for(i = 0; i < nseg; i++) tmp[i] = ss[i * 3 + k];
    sig[k] = dkth(tmp, nseg, nseg / 2);
  }
  /* per line: linear between segment centres */
  for(l = d->f0; l < d->f1; l++)
    for(k = 0; k < 3; k++) {
      double v;
      if(!has_holes) {
        v = sb[k];
      } else {
        double u = (double)(l - d->f0) / segl - 0.5;
        int s0 = (int)floor(u);
        double w = u - s0;
        if(s0 < 0) {
          s0 = 0;
          w = 0;
        }
        if(s0 >= nseg - 1) {
          s0 = nseg - 1;
          w = 0;
        }
        v = sb[s0 * 3 + k] * (1 - w) + (w > 0 ? sb[(s0 + 1) * 3 + k] * w : 0);
      }
      base_l[(l - d->f0) * 3 + k] = (float)(v > 1e-5 ? v : 1e-5);
    }
  free(sb);
  free(ss);
  free(tmp);
  free(ok);
  for(k = 0; k < 3; k++) free(buf[k]);
  return 0;
}

/* Strip tilt from the two hole rows: the along-shift that best aligns the
 * right row's open profile with the left row's (sub-line by parabola).
 * *ok = 0 (tilt 0) when the strip is too short or nothing correlates. */
static double holes_tilt(const det_t* d, int* ok) {
  int nl = d->f1 - d->f0, l, s, best = 0, ms;
  double xl = 0.5 * (d->hl0 + d->b0) * d->xm,
         xr = 0.5 * (d->b1 + d->hr1) * d->xm;
  double *pl = calloc(nl, sizeof *pl), *pr = calloc(nl, sizeof *pr);
  double ml = 0, mr = 0, cbest = -1e300, cm = 0, cp = 0, sh;
  *ok = 0;
  if(!pl || !pr || xr - xl < 5) {
    free(pl);
    free(pr);
    return 0;
  }
  ms = (int)ceil((xr - xl) * tan(MAX_TILT_DEG * PI / 180) / d->ym) + 1;
  if(nl < 4 * ms) { /* too short to correlate: no tilt */
    free(pl);
    free(pr);
    return 0;
  }
  for(l = 0; l < nl; l++) {
    int c, n = 0;
    for(c = d->hl0; c < d->b0; c++)
      n += is_open(pix(d->t, d->px, d->f0 + l, c));
    pl[l] = (double)n / (d->b0 - d->hl0);
    n = 0;
    for(c = d->b1; c < d->hr1; c++)
      n += is_open(pix(d->t, d->px, d->f0 + l, c));
    pr[l] = (double)n / (d->hr1 - d->b1);
    ml += pl[l];
    mr += pr[l];
  }
  ml /= nl;
  mr /= nl;
  {
    double* cc = calloc(2 * ms + 1, sizeof *cc);
    if(!cc) {
      free(pl);
      free(pr);
      return 0;
    }
    for(s = -ms; s <= ms; s++) {
      double acc = 0;
      for(l = ms; l < nl - ms; l++) acc += (pl[l] - ml) * (pr[l + s] - mr);
      cc[s + ms] = acc;
      if(acc > cbest) {
        cbest = acc;
        best = s;
      }
    }
    if(best > -ms && best < ms) {
      cm = cc[best - 1 + ms];
      cp = cc[best + 1 + ms];
    }
    free(cc);
  }
  if(!(cbest > 0)) { /* no hole pattern match: no tilt */
    free(pl);
    free(pr);
    return 0;
  }
  *ok = 1;
  sh = best;
  if(best > -ms && best < ms && cm - 2 * cbest + cp < 0)
    sh += 0.5 * (cm - cp) / (cm - 2 * cbest + cp);
  free(pl);
  free(pr);
  return sh * d->ym / (xr - xl);
}

/* Robust line fy = a + b fx from the start value a (slope 0): refit on
 * the points within 3 rms (at least clip) until the set settles. The first
 * band is 3 x the MAD about a, or just clip when tight (a second cluster
 * would widen the MAD over both). Returns the inlier count (in[] marks
 * them), 0 when degenerate. */
static int line_fit(const double* fx, const double* fy, int n, double a,
                    double clip, int tight, unsigned char* in, double* ao,
                    double* bo, double* rmso) {
  double b = 0, rms = 0;
  int k, it, nin = 0;

  if(!tight) {
    double* r = malloc(n * sizeof *r);
    if(!r) return 0;
    for(k = 0; k < n; k++) r[k] = fabs(fy[k] - a);
    rms = 1.4826 * dkth(r, n, n / 2);
    free(r);
  }
  for(it = 0; it < 8; it++) {
    double th = 3 * rms > clip ? 3 * rms : clip;
    double sx = 0, sy = 0, sxx = 0, sxy = 0, rr = 0, den;
    int changed = 0, m = 0;
    for(k = 0; k < n; k++) {
      unsigned char v = fabs(fy[k] - (a + b * fx[k])) <= th;
      changed |= it == 0 || v != in[k];
      in[k] = v;
    }
    if(!changed) break;
    for(k = 0; k < n; k++)
      if(in[k]) {
        sx += fx[k];
        sy += fy[k];
        sxx += fx[k] * fx[k];
        sxy += fx[k] * fy[k];
        m++;
      }
    den = m * sxx - sx * sx;
    if(m < 3 || den <= 0) return 0;
    b = (m * sxy - sx * sy) / den;
    a = (sy - b * sx) / m;
    for(k = 0; k < n; k++)
      if(in[k]) {
        double e = fy[k] - (a + b * fx[k]);
        rr += e * e;
      }
    rms = sqrt(rr / m);
    nin = m;
  }
  *ao = a;
  *bo = b;
  *rmso = rms;
  return nin;
}

/*
 * A frame edge of the gap [a, b): per image column, walk from the gap
 * middle outward (dir -1: towards the frame before, dir +1: the frame
 * after) up to EDGE_WIN_MM past the gap boundary and take the first switch
 * to exposed that holds 3 lines (nearest the gap). Robust line fit: start
 * from the median, then iterate least squares on inliers within
 * max(3 rms, EDGE_CLIP_MM). Accepted with EDGE_FOUND of the image columns
 * as inliers and inlier rms <= EDGE_RMS_MM: a straight edge across the
 * band. Curved boundaries (round shadows) have no such line. Failing that,
 * the outermost straight cluster with EDGE_OUTER of the columns and at
 * most EDGE_BEYOND past it (see below).
 */
static int fit_edge(const det_t* d, const gap_t* q, int dir, double* pos,
                    double* slope) {
  int nc = d->ic1 - d->ic0, win = (int)(EDGE_WIN_MM / d->ym + 0.5);
  int mid = (q->a + q->b) / 2, c, l, k, n = 0, nin, ngap, ok = 0;
  double xc = 0.5 * (d->ic0 + d->ic1), clip = EDGE_CLIP_MM / d->ym;
  double *fx = malloc(nc * sizeof *fx), *fy = malloc(nc * sizeof *fy);
  double* r = malloc(nc * sizeof *r);
  unsigned char* in = malloc(nc);
  double a = 0, b = 0, rms = 0;

  if(!fx || !fy || !r || !in) goto out;
  for(c = d->ic0; c < d->ic1; c++) {
    if(dir < 0) {
      int lo = q->a - win < d->f0 + 2 ? d->f0 + 2 : q->a - win;
      for(l = mid; l >= lo; l--)
        if(S(d, l, c) == S_EXP &&
           S(d, l - 1, c) == S_EXP &&
           S(d, l - 2, c) == S_EXP) {
          fx[n] = c - xc;
          fy[n++] = l + 1;
          break;
        }
    } else {
      int hi = q->b + win > d->f1 - 3 ? d->f1 - 3 : q->b + win;
      for(l = mid; l <= hi; l++)
        if(S(d, l, c) == S_EXP &&
           S(d, l + 1, c) == S_EXP &&
           S(d, l + 2, c) == S_EXP) {
          fx[n] = c - xc;
          fy[n++] = l;
          break;
        }
    }
  }
  if(n < 3 || n < EDGE_FOUND * nc) goto out;
  /* start: median, slope 0 (lines are deskewed) */
  for(k = 0; k < n; k++) r[k] = fy[k];
  nin = line_fit(fx, fy, n, dkth(r, n, n / 2), clip, 0, in, &a, &b, &rms);
  if(nin >= EDGE_FOUND * nc && rms * d->ym <= EDGE_RMS_MM) {
    ok = 1;
    goto done;
  }
  /* Thin content at the frame end only moves the found edge into the
   * frame, never into the gap: the true edge is the outermost straight
   * cluster. Start on the gap side; accept a smaller cluster when hardly
   * anything lies beyond it (hardware 2026-10-12: 53-56 % of columns on
   * a clean line, the rest up to 3 mm inside). */
  for(k = 0; k < n; k++) r[k] = fy[k];
  nin = line_fit(fx, fy, n, dkth(r, n, dir < 0 ? n * 9 / 10 : n / 10), clip, 1,
                 in, &a, &b, &rms);
  for(k = 0, ngap = 0; k < n; k++)
    if(!in[k] && (fy[k] - (a + b * fx[k])) * -dir > 0) ngap++;
  if(nin >= EDGE_OUTER * nc &&
     ngap <= EDGE_BEYOND * nc &&
     rms * d->ym <= EDGE_RMS_MM)
    ok = 1;
done:
  if(ok) {
    *pos = a; /* at the image centre */
    *slope = b * d->ym / d->xm;
  }
out:
  free(fx);
  free(fy);
  free(r);
  free(in);
  return ok;
}

/* deskew offsets, gap score, gap runs and their edges; returns # trusted */
static int analyse(det_t* d) {
  int c, l, i, k, nok = 0, s = -1, minl = (int)ceil(MIN_GAP_MM / d->ym);
  double xc = 0.5 * (d->ic0 + d->ic1);

  for(c = 0; c < d->px; c++)
    d->off[c] = (int)floor((c - xc) * d->xm * d->tn / d->ym + 0.5);
  for(l = d->f0; l < d->f1; l++) {
    int nb = 0, ne = 0;
    for(c = d->ic0; c < d->ic1; c++) {
      int v = S(d, l, c);
      nb += v == S_BASE;
      ne += v == S_EXP;
    }
    d->g[l] = d->nofilm[l]                         ? 1
              : nb + ne >= 0.5 * (d->ic1 - d->ic0) ? (double)nb / (nb + ne)
                                                   : -1;
  }
  d->ng = 0;
  for(l = d->f0; l <= d->f1; l++) {
    int in = l < d->f1 && d->g[l] >= GAP_FRAC;
    if(in && s < 0) s = l;
    if(!in && s >= 0) {
      if(l - s >= minl) {
        gap_t* q = &d->gaps[d->ng++];
        memset(q, 0, sizeof *q);
        q->a = s;
        q->b = l;
        for(k = s; k < l && !q->nofilm; k++) q->nofilm = d->nofilm[k];
      }
      s = -1;
    }
  }
  for(i = 0; i < d->ng; i++) {
    gap_t* q = &d->gaps[i];
    if(q->a > d->f0)
      q->lead_ok = fit_edge(d, q, -1, &q->lead_pos, &q->lead_slope);
    if(q->b < d->f1)
      q->trail_ok = fit_edge(d, q, +1, &q->trail_pos, &q->trail_slope);
    q->ok = q->lead_ok || q->trail_ok;
    nok += q->ok;
  }
  return nok;
}

/* a full or half frame's length */
static int frame_sized(double len) {
  return (len >= 15.5 && len <= 21.0) || (len >= 33.0 && len <= 39.5);
}

static void classify(double len, int at_end, ft_frame_type* type, int* low,
                     int* keep) {
  *low = 0;
  *keep = 1;
  if(at_end) {
    if(len < END_SLIVER)
      *keep = 0;
    else if(fabs(len - 36) <= 1.5)
      *type = FT_FRAME_FULL;
    else if(fabs(len - 18) <= 1.5)
      *type = FT_FRAME_HALF;
    else {
      *type = FT_FRAME_PARTIAL;
      *low = len > 39.5; /* longer than any frame: unsure */
    }
    return;
  }
  if(len >= 15.5 && len <= 21.0) {
    *type = FT_FRAME_HALF;
    return;
  }
  if(len >= 33.0 && len <= 39.5) {
    *type = FT_FRAME_FULL;
    return;
  }
  if(len > 39.5) {
    *type = FT_FRAME_PANORAMA;
    return;
  }
  if(len < MIN_RUN_MM) {
    *keep = 0;
    return;
  }
  *low = 1; /* between types: nearest type */
  *type = len < 27.0 ? FT_FRAME_HALF : FT_FRAME_FULL;
}

static ft_det_frame* new_frame(ft_layout* L) {
  ft_det_frame* f;
  if(L->nframes == FT_MAX_FRAMES) return NULL;
  f = &L->f[L->nframes++];
  memset(f, 0, sizeof *f);
  f->index = L->nframes;
  return f;
}

/* image extent across of lines [s, e): columns exposed on >= 30 % */
static void run_extent(const det_t* d, int s, int e, int* c0, int* c1) {
  int c, l;
  *c0 = *c1 = -1;
  for(c = d->ic0; c < d->ic1; c++) {
    long n = 0, v = 0;
    for(l = s; l < e; l++) {
      int st = S(d, l, c);
      n += st == S_EXP;
      v += st != S_SKIP;
    }
    if(v > 0 && n >= 0.3 * v) {
      if(*c0 < 0) *c0 = c;
      *c1 = c + 1;
    }
  }
  if(*c0 < 0) {
    *c0 = d->ic0;
    *c1 = d->ic1;
  }
}

/* a stretch with no trusted edge inside: tile it with the holder pitch from
 * its trusted end, every window flagged */
static void grid_run(const det_t* d, ft_layout* L, const ft_holder* h,
                     const ft_axes* ax, double y0, double y1, int from_end,
                     int c0, int c1) {
  double P = h->grid_pitch_mm / d->ym;
  int n = (int)ceil((y1 - y0) / P - 1e-9), i;
  L->fallback = 1;
  for(i = 0; i < n; i++) {
    double a = from_end ? y1 - (n - i) * P : y0 + i * P, b = a + P;
    ft_det_frame* f;
    if(a < y0) a = y0;
    if(b > y1) b = y1;
    if((b - a) * d->ym < MIN_RUN_MM || !(f = new_frame(L))) continue;
    f->type = (b - a) * d->ym >= 33.0 ? FT_FRAME_FULL : FT_FRAME_PARTIAL;
    f->y0 = ax->y_mm0 + a * d->ym;
    f->y1 = ax->y_mm0 + b * d->ym;
    f->x0 = ax->x_mm0 + c0 * d->xm;
    f->x1 = ax->x_mm0 + c1 * d->xm;
    f->tilt_deg = atan(d->tn) * 180 / PI;
    f->confidence_low = 1;
  }
}

static void add_run(const det_t* d, ft_layout* L, const ft_holder* h,
                    const ft_axes* ax, int s, int e, const gap_t* before,
                    const gap_t* after) {
  double ym = d->ym, y0, y1, slope = 0, near = 0, len;
  int nslope = 0, l, c0, c1, low, keep, at_end = !before || !after;
  int cut = (int)(END_CUT_MM / d->ym + 0.5);
  ft_frame_type type = FT_FRAME_PARTIAL;
  ft_det_frame* f;

  if(!before) /* past the last cut line */
    for(l = s + cut < e ? s + cut : e - 1; l >= s; l--)
      if(band_open(d, l) >= CUT_OPEN) {
        s = l + 1;
        break;
      }
  if(!after)
    for(l = e - cut > s ? e - cut : s; l < e; l++)
      if(band_open(d, l) >= CUT_OPEN) {
        e = l;
        break;
      }
  if(e <= s) return;
  y0 = before ? (before->trail_ok ? before->trail_pos : before->b) : s;
  y1 = after ? (after->lead_ok ? after->lead_pos : after->a) : e;
  len = (y1 - y0) * ym;

  run_extent(d, s, e, &c0, &c1);
  if(len > GRID_RUN_MM) {
    grid_run(d, L, h, ax, y0, y1, !before && after, c0, c1);
    return;
  }
  classify(len, at_end, &type, &low, &keep);
  if(!keep) return;
  for(l = s; l < e; l++)
    if(d->g[l] >= NEAR_GAP) near += ym;
  if(type == FT_FRAME_PANORAMA &&
     (near >= 0.3 || len < PANO_MIN_MM || len > PANO_MAX_MM))
    low = 1; /* maybe a gap we did not trust */
  /* One trusted edge and one untrusted: base-like content at the frame
   * end may have moved the gap boundary into the frame. Extend to the
   * nominal length from the trusted edge, staying inside the gap. */
  if(!at_end && !low && (type == FT_FRAME_FULL || type == FT_FRAME_HALF)) {
    double N = (type == FT_FRAME_FULL ? 36.0 : 18.0) / ym;
    if(y1 - y0 < N - 0.2 / ym) {
      if(!before->trail_ok && after->lead_ok) {
        y0 = y1 - N;
        if(y0 < before->a) y0 = before->a;
      } else if(before->trail_ok && !after->lead_ok) {
        y1 = y0 + N;
        if(y1 > after->b) y1 = after->b;
      }
    }
  }
  if(before && before->trail_ok) {
    slope += before->trail_slope;
    nslope++;
  }
  if(after && after->lead_ok) {
    slope += after->lead_slope;
    nslope++;
  }
  if(!(f = new_frame(L))) return;
  f->type = type;
  f->y0 = ax->y_mm0 + y0 * ym;
  f->y1 = ax->y_mm0 + y1 * ym;
  f->x0 = ax->x_mm0 + c0 * d->xm;
  f->x1 = ax->x_mm0 + c1 * d->xm;
  f->tilt_deg = atan(d->tn + (nslope ? slope / nslope : 0)) * 180 / PI;
  f->confidence_low = low;
}

/* blank frames in a long trusted gap: by the local pitch (half frames
 * next to it: 19 mm, else 38 mm) */
static int blanks(const ft_layout* L, const ft_axes* ax, double ym,
                  const gap_t* q) {
  double ya = ax->y_mm0 + q->a * ym, yb = ax->y_mm0 + q->b * ym;
  double pitch = 38.0;
  int i, n;
  for(i = 0; i < L->nframes; i++)
    if(L->f[i].type == FT_FRAME_HALF &&
       (fabs(L->f[i].y1 - ya) < 3 || fabs(L->f[i].y0 - yb) < 3))
      pitch = 19.0;
  n = (int)((yb - ya) / pitch + 0.5);
  return n < 1 ? 1 : n;
}

int ft_detect(const float* t, int lines, int px, const ft_axes* ax,
              ft_film film, const ft_holder* h, ft_layout* L, ft_err* e) {
  det_t D, *d = &D;
  double* col = NULL;
  unsigned char* halo = NULL;
  float* base_l = NULL;
  double sig[3];
  int c, l, b0, b1, i, nok, rc = -1, tilt_ok = 0;

  if(!L) return ft_fail(e, "detect: no layout");
  memset(L, 0, sizeof *L);
  memset(d, 0, sizeof *d);
  if(!t || !ax || !h || lines < 8 || px < 8)
    return ft_fail(e, "detect: bad arguments");
  if(!(ax->y_mm_per_line > 0) ||
     !(ax->x_mm_per_px > 0) ||
     !isfinite(ax->y_mm_per_line) ||
     !isfinite(ax->x_mm_per_px) ||
     !isfinite(ax->y_mm0) ||
     !isfinite(ax->x_mm0))
    return ft_fail(e, "detect: bad axes");
  d->t = t;
  d->lines = lines;
  d->px = px;
  d->ym = ax->y_mm_per_line;
  d->xm = ax->x_mm_per_px;
  d->film = film;
  col = calloc(px > lines ? px : lines, sizeof *col);
  d->st = calloc((size_t)lines * px, 1);
  d->off = calloc(px, sizeof *d->off);
  d->g = calloc(lines, sizeof *d->g);
  d->nofilm = calloc(lines, 1);
  d->gaps = calloc(lines / 2 + 1, sizeof *d->gaps);
  base_l = malloc((size_t)lines * 3 * sizeof *base_l);
  if(!col || !d->st || !d->off || !d->g || !d->nofilm || !d->gaps || !base_l) {
    ft_fail(e, "detect: out of memory");
    goto out;
  }

  /* 1. lines: open holder, holder metal, else film. Film = first..last
   * film line; dark lines inside (slide Dmax between the holes) are film. */
  d->f0 = -1;
  for(l = 0; l < lines; l++) {
    int no = 0, nd = 0;
    for(c = 0; c < px; c++) {
      const float* p = pix(t, px, l, c);
      no += is_open(p);
      nd += is_dark(p);
    }
    /* metal: dark with no hole on the line. Slide Dmax can be as dark
     * as metal, but film lines through the hole rows show open beam
     * (~17 % of the line); lines between holes are interior. */
    if(no <= LINE_FRAC * px && (nd <= LINE_FRAC * px || no >= HOLE_LINE * px)) {
      if(d->f0 < 0) d->f0 = l;
      d->f1 = l + 1;
    }
  }
  if(d->f0 < 0) {
    ft_fail(e, "detect: no film in the holder");
    goto out;
  }
  L->film_y0 = ax->y_mm0 + d->f0 * d->ym;
  L->film_y1 = ax->y_mm0 + d->f1 * d->ym;

  /* 2. open fraction per column over film lines -> band and hole rows */
  for(c = 0; c < px; c++) {
    long n = 0;
    for(l = d->f0; l < d->f1; l++) n += is_open(pix(t, px, l, c));
    col[c] = (double)n / (d->f1 - d->f0);
  }
  if(widest_run(col, px, -1, 0.25, &b0, &b1) * d->xm < 10) {
    ft_fail(e, "detect: no film band found");
    goto out;
  }
  {
    int left = grow(col, px, b0 - 1, -1, 0.25, 0.9) + 1;
    int right = grow(col, px, b1, +1, 0.25, 0.9);
    L->has_holes =
      (b0 - left) * d->xm >= HOLE_MIN_MM && (right - b1) * d->xm >= HOLE_MIN_MM;
    d->b0 = b0;
    d->b1 = b1;
    d->hl0 = L->has_holes ? left : b0;
    d->hr1 = L->has_holes ? right : b1;
    L->band_x0 = ax->x_mm0 + b0 * d->xm;
    L->band_x1 = ax->x_mm0 + b1 * d->xm;
    L->holes_x0 = ax->x_mm0 + d->hl0 * d->xm;
    L->holes_x1 = ax->x_mm0 + d->hr1 * d->xm;
  }

  for(l = d->f0; l < d->f1; l++) d->nofilm[l] = band_open(d, l) >= NOFILM_OPEN;

  /* 3. base level */
  if(!(halo = open_halo(d)) ||
     base_levels(d, halo, L->has_holes, base_l, L->base, sig)) {
    ft_fail(e, halo ? "detect: no film base found" : "detect: out of memory");
    goto out;
  }

  /* 4. pixel states. Negative: exposed = darker than the base by
   * BASE_DENS (and the noise floor); slide: brighter. */
  {
    double f = pow(10, film == FT_FILM_NEGATIVE ? -BASE_DENS : BASE_DENS);
    double ks = film == FT_FILM_NEGATIVE ? -K_SIGMA : K_SIGMA;
    for(l = d->f0; l < d->f1; l++) {
      const float* bl = base_l + (l - d->f0) * 3;
      float th[3];
      int k;
      for(k = 0; k < 3; k++) th[k] = (float)(bl[k] * f + ks * sig[k]);
      for(c = 0; c < px; c++) {
        const float* p = pix(t, px, l, c);
        unsigned char* q = &d->st[(long)l * px + c];
        if(halo[(long)l * px + c] || is_open(p)) {
          *q = S_SKIP;
          continue;
        }
        if(film == FT_FILM_NEGATIVE)
          *q = p[0] < th[0] || p[1] < th[1] || p[2] < th[2] ? S_EXP : S_BASE;
        else
          *q = p[0] > th[0] || p[1] > th[1] || p[2] > th[2] ? S_EXP : S_BASE;
      }
    }
  }

  /* 5. image columns: ever exposed inside the band */
  for(c = 0; c < px; c++) {
    long ne = 0, nv = 0;
    col[c] = 0;
    if(c < b0 || c >= b1) continue;
    for(l = d->f0; l < d->f1; l++) {
      int v = d->st[(long)l * px + c];
      ne += v == S_EXP;
      nv += v != S_SKIP;
    }
    col[c] = nv ? (double)ne / nv : 0;
  }
  if(widest_run(col, px, 0.05, 2.0, &d->ic0, &d->ic1) * d->xm < 5) {
    L->nblank = (int)((d->f1 - d->f0) * d->ym / 38 + 0.5); /* nothing exposed */
    rc = 0;
    goto out;
  }

  /* 6. tilt, gaps, edges. Without a hole-row tilt: a first pass
   * untilted, then again at the median slope of the accepted edges. */
  d->tn = L->has_holes ? holes_tilt(d, &tilt_ok) : 0;
  nok = analyse(d);
  if(!tilt_ok && nok > 0) {
    double sl[2 * FT_MAX_FRAMES];
    int n = 0;
    for(i = 0; i < d->ng && n < 2 * FT_MAX_FRAMES - 1; i++) {
      if(d->gaps[i].lead_ok) sl[n++] = d->gaps[i].lead_slope;
      if(d->gaps[i].trail_ok) sl[n++] = d->gaps[i].trail_slope;
    }
    if(n > 0) {
      double m = dkth(sl, n, n / 2);
      if(fabs(m) > tan(MIN_TILT_DEG * PI / 180)) {
        d->tn = m;
        nok = analyse(d);
      }
    }
  }
  L->tilt_deg = atan(d->tn) * 180 / PI;

  /* 7. no trusted edge anywhere on a long strip: holder grid, flagged */
  if(nok == 0 && (d->f1 - d->f0) * d->ym > FALLBACK_MM) {
    double fy0 = L->film_y0, fy1 = L->film_y1;
    L->fallback = 1;
    for(uint32_t grid_index = 0; grid_index < h->grid_frame_count;
        grid_index++) {
      double y0 = h->grid_first_frame_mm + grid_index * h->grid_pitch_mm;
      double y1 = y0 + h->grid_frame_length_mm;
      ft_det_frame* f;
      if(y0 < fy0) y0 = fy0;
      if(y1 > fy1) y1 = fy1;
      if(y1 - y0 < MIN_RUN_MM || !(f = new_frame(L))) continue;
      f->type = FT_FRAME_FULL;
      f->y0 = y0;
      f->y1 = y1;
      f->x0 = ax->x_mm0 + d->ic0 * d->xm;
      f->x1 = ax->x_mm0 + d->ic1 * d->xm;
      f->tilt_deg = L->tilt_deg;
      f->confidence_low = 1;
    }
    rc = 0;
    goto out;
  }

  /* 8. exposed runs between trusted gaps */
  {
    int s = d->f0;
    const gap_t* before = NULL;
    for(i = 0; i <= d->ng; i++) {
      const gap_t* q = i < d->ng ? &d->gaps[i] : NULL;
      /* untrusted gaps stay in the run, unless they end a
       * frame-sized piece: gap lines across the band mark a frame
       * end even when dark content hides the edge itself */
      if(q && !q->ok && !frame_sized((q->a - s) * d->ym)) continue;
      if((q ? q->a : d->f1) > s)
        add_run(d, L, h, ax, s, q ? q->a : d->f1, before, q);
      if(q) {
        s = q->b;
        before = q;
      }
    }
  }
  /* 9. long trusted gaps are blank frames; at the strip ends they may
   * be leader, so they are counted apart */
  for(i = 0; i < d->ng; i++) {
    const gap_t* q = &d->gaps[i];
    if(q->ok && !q->nofilm && (q->b - q->a) * d->ym > BLANK_MM) {
      if(q->a > d->f0 && q->b < d->f1)
        L->nblank += blanks(L, ax, d->ym, q);
      else
        L->nblank_end += blanks(L, ax, d->ym, q);
    }
  }
  rc = 0;
out:
  free(col);
  free(halo);
  free(base_l);
  free(d->st);
  free(d->off);
  free(d->g);
  free(d->nofilm);
  free(d->gaps);
  return rc;
}

static double clampd(double v, double lo, double hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

/* Windows are clamped to the holder's crop limits: the preview reaches
 * past them (across 0.92..33.14 mm, along ~232 mm), so sprockets-in
 * windows and frames cut by the strip end would otherwise fail
 * ft_job_validate. */
void ft_layout_to_job(const ft_layout* L, const ft_holder* h, ft_film film,
                      double ppi, int sprockets, ft_job* j) {
  int i;
  memset(j, 0, sizeof *j);
  snprintf(j->holder, sizeof j->holder, "%s", h->name);
  j->film = film;
  j->focus = FT_FOCUS_SHIFTED;
  /* the render's film base (negative) or black (slide), as density */
  for(i = 0; i < 3; i++) {
    double d = -log10(L->base[i] > FT_T_FLOOR ? L->base[i] : FT_T_FLOOR);
    /* a channel may exceed T = 1, and -log10(FT_T_FLOOR) is a hair
     * above 4.5; the job reader accepts [0, 4.5] only */
    j->film_base[i] = d < 0 ? 0 : d > 4.5 ? 4.5 : d;
  }
  j->has_film_base = 1;
  for(i = 0; i < L->nframes && i < FT_MAX_FRAMES; i++) {
    const ft_det_frame* d = &L->f[i];
    ft_job_frame* f = &j->f[j->nframes++];
    double x0 = sprockets ? L->holes_x0 : d->x0;
    double x1 = sprockets ? L->holes_x1 : d->x1;
    double y0 = clampd(d->y0, 0, h->scan_length_mm);
    double y1 = clampd(d->y1, 0, h->scan_length_mm);
    x0 = clampd(x0, 0, h->scan_width_mm);
    x1 = clampd(x1, 0, h->scan_width_mm);
    f->index = d->index;
    f->type = d->type;
    f->include = 1;
    f->ppi = ppi;
    f->y0 = y0;
    f->len = y1 - y0;
    f->x0 = x0;
    f->width = x1 - x0;
    f->tilt_deg = d->tilt_deg;
    f->sprockets = sprockets;
    f->confidence_low = d->confidence_low;
  }
}

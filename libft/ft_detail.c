/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_detail.h"
#include "ft_calib.h"
#include "ft_density.h"
#include "ft_detect.h"
#include "ft_icc.h"
#include "ft_stream.h"
#include "ft_tiff.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ft_detail {
  ft_detail_opts o;
  ft_stream *st;
  int k, f; /* tri-linear offset, thumbnail factor */
  int cal_lines;
  long raw_n, aligned_n;
  float *line;               /* one developed line, px x 3 */
  unsigned short *t16, *enc; /* px x 3 each */
  ft_dens *dens;             /* POS */
  double w[3];               /* POS mono weights */
  int tl, tp, acc_n;         /* thumbnail lines, px; lines summed */
  float *thumb;
  double *acc;
  int r[FT_MAX_FRAMES][4]; /* ft_dscan_ranges per frame */
  ft_tiffw *tw[FT_MAX_FRAMES];
  char path[FT_MAX_FRAMES][600];
  FILE *raw; /* keep_raw */
  char raw_path[600];
  unsigned char icc[FT_ICC_MAX]; /* negative TIFFs: linear scanner RGB */
  size_t icc_n;
};

int ft_detail_path(
    const char *base, int index, int tiff, char *buf, size_t cap
) {
  int n =
      snprintf(buf, cap, "%s_frame%02d%s", base, index, tiff ? ".tif" : ".dng");
  return n < 0 || (size_t)n + sizeof ".partial" > cap ? -1 : 0;
}

static void free_detail(ft_detail *d) {
  int i;
  for(i = 0; i < d->o.nframes; i++)
    ft_tiffw_free(d->tw[i]);
  ft_stream_close(d->st);
  if(d->raw)
    fclose(d->raw);
  free(d->line);
  free(d->t16);
  free(d->enc);
  free(d->dens);
  free(d->thumb);
  free(d->acc);
  free(d);
}

void ft_detail_abort(ft_detail *d) {
  if(d)
    free_detail(d);
}

static int write_bytes(const char *path, const void *p, size_t n) {
  FILE *f = fopen(path, "wb");
  int bad = !f;
  if(f) {
    bad = fwrite(p, 1, n, f) != n;
    bad |= fclose(f) != 0;
  }
  return bad ? -1 : 0;
}

ft_tiff_kind ft_detail_kind(
    ft_detail_out out, ft_detail_fmt fmt, ft_detail_bw bw
) {
  if(out == FT_DETAIL_NEG)
    return fmt == FT_DFMT_DNG ? FT_TIFF_DNG : FT_TIFF_RGB;
  return bw == FT_BW_MONO ? FT_TIFF_GRAY : FT_TIFF_RGB;
}

static ft_tiff_kind kind_of(const ft_detail_opts *o) {
  return ft_detail_kind(o->out, o->fmt, o->bw);
}

ft_detail *ft_detail_open(
    const ft_detail_opts *o, const unsigned short *cal, int cal_lines,
    const char *faul, size_t faul_n, ft_err *e
) {
  const ft_dscan *s = &o->scan;
  ft_develop_opts dev;
  ft_detail *d;
  size_t line = (size_t)s->px * 3;
  int i;

  if(o->nframes < 1 || o->nframes > FT_MAX_FRAMES || s->px < 1 ||
     s->lines < 1 || s->bin < 1 || !o->holder || !o->meta || !o->base ||
     (o->out != FT_DETAIL_NEG && o->out != FT_DETAIL_POS)) {
    ft_fail(
        e, "detail: bad pass (%d frames, %ld px, %ld lines)", o->nframes, s->px,
        s->lines
    );
    return NULL;
  }
  if(o->out == FT_DETAIL_POS && o->film != FT_FILM_NEGATIVE) {
    ft_fail(e, "detail: the archive positive is for negative film");
    return NULL;
  }
  d = calloc(1, sizeof *d);
  if(!d) {
    ft_fail(e, "detail: out of memory");
    return NULL;
  }
  d->o = *o;
  d->cal_lines = cal_lines;
  memset(&dev, 0, sizeof dev);
  dev.x = (int)s->x_out;
  dev.bin = s->bin;
  dev.defects = o->defects;
  d->st = ft_stream_open((int)s->px, cal, cal_lines, faul, faul_n, &dev, e);
  if(!d->st)
    goto fail;
  d->k = ft_stream_k(d->st);
  d->f = ft_dscan_thumb(s);
  d->tl = (int)((s->lines - 2 * d->k) / d->f);
  d->tp = (int)(s->px / d->f);
  d->line = malloc(line * sizeof *d->line);
  d->t16 = malloc(line * sizeof *d->t16);
  d->enc = malloc(line * sizeof *d->enc);
  d->acc = calloc((size_t)(d->tp > 0 ? d->tp : 1) * 3, sizeof *d->acc);
  if(d->tl > 0 && d->tp > 0)
    d->thumb = malloc((size_t)d->tl * d->tp * 3 * sizeof *d->thumb);
  if(!d->line || !d->t16 || !d->enc || !d->acc ||
     (d->tl > 0 && d->tp > 0 && !d->thumb)) {
    ft_fail(e, "detail: out of memory");
    goto fail;
  }
  if(o->out == FT_DETAIL_POS) {
    double var[3];
    d->dens = malloc(sizeof *d->dens);
    if(!d->dens) {
      ft_fail(e, "detail: out of memory");
      goto fail;
    }
    ft_dens_init(d->dens, o->film_base);
    if(ft_white_var(cal, cal_lines, (int)s->px, var, e))
      goto fail;
    ft_dens_weights(var, d->w);
  }

  if(kind_of(o) == FT_TIFF_RGB && o->out == FT_DETAIL_NEG) {
    d->icc_n = ft_icc_build_linear(
        o->meta->xyz_to_rgb,
        "Flextight scanner RGB, linear "
        "(flextight)",
        d->icc, sizeof d->icc
    );
    if(!d->icc_n) {
      ft_fail(e, "detail: the scanner colour matrix is singular");
      goto fail;
    }
  }
  for(i = 0; i < o->nframes; i++) {
    const ft_detail_frame *fr = &o->f[i];
    ft_tiff_opts to;
    ft_image meta = *o->meta;
    char partial[620];
    if(ft_dscan_ranges(
           s, fr->y0 - o->margin_mm, fr->y1 + o->margin_mm,
           fr->x0 - o->margin_mm, fr->x1 + o->margin_mm, d->r[i]
       )) {
      ft_fail(e, "detail: frame %d is not in the pass scan", fr->index);
      goto fail;
    }
    if(ft_detail_path(
           o->base, fr->index, kind_of(o) != FT_TIFF_DNG, d->path[i],
           sizeof d->path[i]
       )) {
      ft_fail(e, "detail: output name too long");
      goto fail;
    }
    meta.ppi = s->ppi;
    if(o->out == FT_DETAIL_NEG)
      snprintf(
          meta.desc, sizeof meta.desc,
          "flextight digital negative: transmittance x 65535; "
          "frame %d, pass %d, %g ppi, bin %d",
          fr->index, o->pass, s->ppi, s->bin
      );
    else
      snprintf(
          meta.desc, sizeof meta.desc,
          "ft-density v1 base=%.4f %.4f %.4f offset=%.1f "
          "range=%.1f bw=%s w=%.4f %.4f %.4f; frame %d, pass %d, "
          "%g ppi",
          d->dens->base[0], d->dens->base[1], d->dens->base[2], FT_DENS_OFFSET,
          FT_DENS_RANGE,
          o->bw == FT_BW_MONO  ? "mono"
          : o->bw == FT_BW_RGB ? "rgb"
                               : "off",
          d->w[0], d->w[1], d->w[2], fr->index, o->pass, s->ppi
      );
    memset(&to, 0, sizeof to);
    to.kind = kind_of(o);
    to.w = d->r[i][1] - d->r[i][0];
    to.h = d->r[i][3] - d->r[i][2];
    to.meta = &meta;
    to.icc = d->icc_n ? d->icc : NULL;
    to.icc_n = d->icc_n;
    snprintf(partial, sizeof partial, "%s.partial", d->path[i]);
    d->tw[i] = ft_tiffw_open(partial, &to, e);
    if(!d->tw[i])
      goto fail;
  }

  if(o->keep_raw) {
    char p[620];
    int n = snprintf(
        d->raw_path, sizeof d->raw_path, "%s_pass%02d.raw", o->base, o->pass
    );
    if(n < 0 || (size_t)n + 8 > sizeof d->raw_path) {
      ft_fail(e, "detail: output name too long");
      goto fail;
    }
    snprintf(p, sizeof p, "%s.cal", d->raw_path);
    if(write_bytes(p, cal, (size_t)cal_lines * line * 2)) {
      ft_fail(e, "detail: cannot write %s", p);
      goto fail;
    }
    snprintf(p, sizeof p, "%s.faul", d->raw_path);
    if(faul && faul_n && write_bytes(p, faul, faul_n)) {
      ft_fail(e, "detail: cannot write %s", p);
      goto fail;
    }
    d->raw = fopen(d->raw_path, "wb");
    if(!d->raw) {
      ft_fail(e, "detail: cannot write %s", d->raw_path);
      goto fail;
    }
  }
  return d;
fail:
  free_detail(d);
  return NULL;
}

/* box-average the developed line into the thumbnail */
static void thumb_line(ft_detail *d) {
  int j, c, q, f = d->f;
  long row = d->aligned_n / f;
  if(row >= d->tl)
    return; /* leftover lines are dropped */
  for(j = 0; j < d->tp; j++)
    for(q = 0; q < f; q++)
      for(c = 0; c < 3; c++)
        d->acc[j * 3 + c] += d->line[(j * f + q) * 3 + c];
  if(++d->acc_n < f)
    return;
  for(j = 0; j < d->tp * 3; j++) {
    d->thumb[(size_t)row * d->tp * 3 + j] =
        (float)(d->acc[j] / ((double)f * f));
    d->acc[j] = 0;
  }
  d->acc_n = 0;
}

int ft_detail_line(ft_detail *d, const unsigned short *raw, ft_err *e) {
  size_t line = (size_t)d->o.scan.px * 3;
  long l;
  int rc, i;

  if(d->raw && fwrite(raw, 2, line, d->raw) != line)
    return ft_fail(e, "detail: write error on %s", d->raw_path);
  d->raw_n++;
  rc = ft_stream_push(d->st, raw, d->line, e);
  if(rc <= 0)
    return rc;
  l = d->aligned_n;
  if(d->thumb)
    thumb_line(d);
  ft_dens_t16(d->line, d->t16, line);
  for(i = 0; i < d->o.nframes; i++) {
    const int *r = d->r[i];
    const unsigned short *col = d->t16 + (size_t)r[2] * 3;
    size_t n = (size_t)(r[3] - r[2]);
    if(l < r[0] || l >= r[1])
      continue;
    if(d->o.out == FT_DETAIL_POS) {
      if(d->o.bw == FT_BW_MONO)
        ft_dens_mono(d->dens, d->w, col, d->enc, n);
      else
        ft_dens_rgb(d->dens, col, d->enc, n);
      col = d->enc;
    }
    if(ft_tiffw_column(d->tw[i], col, e))
      return -1;
  }
  d->aligned_n++;
  return 0;
}

/* the detected frame overlapping the preview box most (at least half its
 * length, centre within margin), or NULL */
static const ft_det_frame *match(
    const ft_layout *L, const ft_detail_frame *x, double margin
) {
  const ft_det_frame *best = NULL;
  double best_ov = 0.5 * (x->y1 - x->y0);
  int i;
  for(i = 0; i < L->nframes; i++) {
    const ft_det_frame *g = &L->f[i];
    double ov =
        (g->y1 < x->y1 ? g->y1 : x->y1) - (g->y0 > x->y0 ? g->y0 : x->y0);
    double shift = ((g->y0 + g->y1) - (x->y0 + x->y1)) / 2;
    if(g->type == FT_FRAME_PARTIAL || !(fabs(shift) <= margin))
      continue;
    if(ov >= best_ov) {
      best_ov = ov;
      best = g;
    }
  }
  return best;
}

static int cmp_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

static int write_sidecar(
    const ft_detail *d, int i, const ft_detail_frame_result *fr
) {
  const ft_detail_opts *o = &d->o;
  char p[620];
  FILE *f;
  snprintf(p, sizeof p, "%s.txt", d->path[i]);
  f = fopen(p, "w");
  if(!f)
    return -1;
  fprintf(
      f,
      "frame=%d\npass=%d\nout=%s\nppi=%g\nbin=%d\nfocus_offset=%ld\n"
      "w=%d\nh=%d\nframe_box=%d %d %d %d\nmargin_mm=%.2f\n"
      "shift_mm=%.3f\nestimated=%d\nedge=%d\n"
      "scan=%ld %ld %ld %ld\nscan_range=%d %d %d %d\n",
      fr->index, o->pass, o->out == FT_DETAIL_NEG ? "neg" : "pos", o->scan.ppi,
      o->scan.bin, o->focus, fr->w, fr->h, fr->box[0], fr->box[1], fr->box[2],
      fr->box[3], o->margin_mm, fr->shift_mm, fr->estimated, fr->edge,
      o->scan.x_out, o->scan.y_line, o->scan.px, o->scan.lines, d->r[i][0],
      d->r[i][1], d->r[i][2], d->r[i][3]
  );
  if(o->out == FT_DETAIL_POS)
    fprintf(
        f,
        "film_base=%.4f %.4f %.4f\nencoding=ft-density v1\n"
        "offset=%.1f\nrange=%.1f\nbw=%s\nweights=%.4f %.4f %.4f\n",
        d->dens->base[0], d->dens->base[1], d->dens->base[2], FT_DENS_OFFSET,
        FT_DENS_RANGE,
        o->bw == FT_BW_MONO  ? "mono"
        : o->bw == FT_BW_RGB ? "rgb"
                             : "off",
        d->w[0], d->w[1], d->w[2]
    );
  else
    fprintf(
        f, "encoding=transmittance x 65535\nfmt=%s\n",
        o->fmt == FT_DFMT_DNG ? "dng" : "tiff"
    );
  return fclose(f) != 0 ? -1 : 0;
}

int ft_detail_close(ft_detail *d, ft_detail_result *r, ft_err *e) {
  const ft_detail_opts *o = &d->o;
  const ft_dscan *s = &o->scan;
  static ft_layout L;
  ft_err de;
  double shifts[FT_MAX_FRAMES], med = 0;
  const ft_det_frame *m[FT_MAX_FRAMES];
  int i, nm = 0, rc = 0, short_of = -1;

  memset(r, 0, sizeof *r);
  r->lines = d->raw_n;
  r->nframes = o->nframes;
  for(i = 0; i < o->nframes; i++)
    if(d->aligned_n < d->r[i][1] && short_of < 0)
      short_of = i;
  if(short_of >= 0) {
    long want = d->r[short_of][1] + 2L * d->k;
    ft_fail(
        e,
        "pass %d: the scan ended after %ld lines; frame %d needs "
        "%ld: unfinished frames stay .partial",
        o->pass, d->raw_n, o->f[short_of].index, want
    );
    rc = -1;
  }

  /* creep: frames found on the thumbnail, matched to the preview boxes */
  if(d->thumb && d->aligned_n >= (long)d->tl * d->f) {
    ft_axes ax;
    ft_dscan_axes(s, d->f, &ax);
    r->detected =
        ft_detect(d->thumb, d->tl, d->tp, &ax, o->film, o->holder, &L, &de) ==
        0;
  }
  for(i = 0; i < o->nframes; i++) {
    m[i] = r->detected ? match(&L, &o->f[i], o->margin_mm) : NULL;
    if(m[i])
      shifts[nm++] = ((m[i]->y0 + m[i]->y1) - (o->f[i].y0 + o->f[i].y1)) / 2;
  }
  if(nm) {
    qsort(shifts, (size_t)nm, sizeof shifts[0], cmp_double);
    med = nm % 2 ? shifts[nm / 2] : (shifts[nm / 2 - 1] + shifts[nm / 2]) / 2;
  }

  for(i = 0; i < o->nframes; i++) {
    ft_detail_frame_result *fr = &r->f[i];
    const ft_detail_frame *x = &o->f[i];
    double y0, y1, x0, x1;
    long bx, by, bw, bh;
    char partial[620];

    fr->index = x->index;
    snprintf(fr->path, sizeof fr->path, "%s", d->path[i]);
    fr->w = d->r[i][1] - d->r[i][0];
    fr->h = d->r[i][3] - d->r[i][2];
    if(m[i]) {
      y0 = m[i]->y0;
      y1 = m[i]->y1;
      x0 = m[i]->x0;
      x1 = m[i]->x1;
    } else {
      y0 = x->y0 + med;
      y1 = x->y1 + med;
      x0 = x->x0;
      x1 = x->x1;
      fr->estimated = 1;
    }
    fr->shift_mm = ((y0 + y1) - (x->y0 + x->y1)) / 2;
    bx = ft_dscan_line(s, y0) - d->r[i][0];
    bw = ft_dscan_line(s, y1) - ft_dscan_line(s, y0);
    by = ft_dscan_px(s, x0) - d->r[i][2];
    bh = ft_dscan_px(s, x1) - ft_dscan_px(s, x0);
    if(bx <= 0 || by <= 0 || bx + bw >= fr->w || by + bh >= fr->h)
      fr->edge = 1;
    if(bx < 0) {
      bw += bx;
      bx = 0;
    }
    if(by < 0) {
      bh += by;
      by = 0;
    }
    if(bw > fr->w - bx)
      bw = fr->w - bx;
    if(bh > fr->h - by)
      bh = fr->h - by;
    if(bw < 1)
      bw = 1;
    if(bh < 1)
      bh = 1;
    fr->box[0] = (int)bx;
    fr->box[1] = (int)by;
    fr->box[2] = (int)bw;
    fr->box[3] = (int)bh;

    if(d->aligned_n < d->r[i][1])
      continue; /* stays .partial */
    if(kind_of(o) == FT_TIFF_DNG)
      ft_tiffw_crop(d->tw[i], fr->box[0], fr->box[1], fr->box[2], fr->box[3]);
    {
      ft_tiffw *tw = d->tw[i];
      d->tw[i] = NULL;
      if(ft_tiffw_close(tw, rc ? &de : e)) {
        rc = -1;
        continue;
      }
    }
    snprintf(partial, sizeof partial, "%s.partial", d->path[i]);
    remove(d->path[i]); /* rename never replaces on Windows */
    if(write_sidecar(d, i, fr) || rename(partial, d->path[i]) != 0) {
      if(!rc)
        ft_fail(e, "detail: cannot finish %s", d->path[i]);
      rc = -1;
    }
  }

  if(d->raw) {
    char p[620];
    FILE *f;
    int bad = fclose(d->raw) != 0;
    d->raw = NULL;
    snprintf(p, sizeof p, "%s.txt", d->raw_path);
    f = fopen(p, "w");
    bad |= !f;
    if(f) {
      fprintf(
          f,
          "preset=detail\nppi=%g\npx=%ld\nx=%ld\nlines=%ld\n"
          "y=%ld\nbin=%d\nkept=%ld\ncal_lines=%d\npass=%d\n"
          "focus_offset=%ld\n",
          s->ppi, s->px, s->x_out, s->lines, s->y_line, s->bin, d->raw_n,
          d->cal_lines, o->pass, o->focus
      );
      bad |= fclose(f) != 0;
    }
    if(bad && !rc) {
      ft_fail(e, "detail: cannot finish %s", d->raw_path);
      rc = -1;
    }
  }
  free_detail(d);
  return rc;
}

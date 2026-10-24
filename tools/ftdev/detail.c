/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ftdev detail: re-develop a detailed-scan pass kept with keep_raw
 * (<base>_passNN.raw + .cal .faul .txt) through the same ring and worker
 * thread ftpoc runs during the scan (tools/detail).
 *
 *   ftdev detail <pass.raw> --job <file.job> [-o base] [-d defects.txt]
 *         [--out neg|pos] [--fmt dng|tiff] [--bw off|mono|rgb]
 *         [--ring-mb N] [--stall ms,lines]
 *
 * The frames, options and film base come from the job (frames whose pass
 * is the .txt's pass); --out/--fmt/--bw override the job's. --stall
 * sleeps ms every `lines` lines in the worker (a slow card).
 *
 * ftdev bench: the worker's speed on this machine, synthetic lines.
 *
 *   ftdev bench [--ppi 3150|6300] [--mm L] [--out neg|pos]
 *         [--fmt dng|tiff] [--bw off|mono|rgb] [--pace] [--ring-mb N]
 *         [-o base] [--keep]
 *
 * Without --pace lines go in as fast as the worker takes them: px/s and
 * the headroom over the scanner's ~84k px/s. With --pace they arrive at
 * the scanner's rate into a ring of --ring-mb and are refused when it is
 * full, as in a real scan: the ring's peak tells how close it came.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_detail.h"
#include "ft_dplan.h"
#include "ft_geom.h"
#include "ft_job.h"
#include "ft_plan.h"
#include "ring.h"
#include "util.h"
#include "worker.h"

static int parse_choice(const char* v, const char* const* names, int n) {
  int i;
  for (i = 0; i < n; i++)
    if (strcmp(v, names[i]) == 0) return i;
  return -1;
}

static const char* const OUTS[] = {"neg", "pos"};
static const char* const FMTS[] = {"dng", "tiff"};
static const char* const BWS[] = {"off", "mono", "rgb"};

/* feed lines through a ring of ring_mb into a worker running d; pace > 0:
 * one line every pace ms, refused when full (a scanner); else wait for
 * room (a file). Returns 0, or -1 with the reason printed. */
static int feed(ft_detail* d, const unsigned short* (*line_at)(long, void*),
                void* ctx, long lines, size_t line_bytes, int ring_mb,
                double pace, unsigned stall_ms, long stall_every,
                double* seconds, char* report, size_t cap) {
  pass_worker w;
  ring* r = ring_open(line_bytes, (size_t)ring_mb << 20);
  double t0, next;
  long l;
  int rc = 0;

  if (!r) {
    fprintf(stderr, "ftdev: no room for a %d MB ring\n", ring_mb);
    return -1;
  }
  memset(&w, 0, sizeof w);
  w.r = r;
  w.d = d;
  w.stall_ms = stall_ms;
  w.stall_every = stall_every;
  if (pass_worker_start(&w)) {
    fprintf(stderr, "ftdev: cannot start the worker thread\n");
    ring_close(r);
    return -1;
  }
  t0 = next = ring_ms();
  for (l = 0; l < lines; l++) {
    const unsigned short* p = line_at(l, ctx);
    if (pace > 0) {
      double now = ring_ms();
      if (next > now) ring_sleep_ms((unsigned)(next - now));
      next += pace;
      if (ring_push(r, p)) {
        fprintf(stderr,
                "ftdev: ring %s at line %ld of %ld: a scan "
                "would have stopped here\n",
                ring_failed(r) ? "failed" : "full", l, lines);
        rc = -1;
        break;
      }
    } else {
      while (ring_push(r, p))
        if (ring_failed(r))
          break;
        else
          ring_sleep_ms(1);
      if (ring_failed(r)) break;
    }
  }
  ring_end(r);
  pass_worker_join(&w);
  *seconds = (ring_ms() - t0) / 1000.0;
  pass_worker_report(&w, *seconds, report, cap);
  if (w.failed) {
    fprintf(stderr, "ftdev: %s\n", w.e.msg);
    rc = -1;
  }
  ring_close(r);
  return rc;
}

/* ----------------------------------------------------------- ftdev detail */

typedef struct {
  const unsigned short* raw;
  long px;
} raw_ctx;

static const unsigned short* raw_line(long l, void* ctx) {
  raw_ctx* c = ctx;
  return c->raw + (size_t)l * c->px * 3;
}

static void detail_usage(void) {
  fprintf(stderr,
          "usage: ftdev detail <pass.raw> --job <file.job> "
          "[-o base] [-d defects.txt]\n"
          "             [--out neg|pos] [--fmt dng|tiff] "
          "[--bw off|mono|rgb] [--ring-mb N]\n"
          "             [--stall ms,lines]\n");
}

static void print_result(const ft_detail_result* r) {
  int i;
  for (i = 0; i < r->nframes; i++) {
    const ft_detail_frame_result* f = &r->f[i];
    printf("%s: %dx%d, frame %d at %d,%d %dx%d, shift %+.2f mm%s%s\n", f->path,
           f->w, f->h, f->index, f->box[0], f->box[1], f->box[2], f->box[3],
           f->shift_mm,
           f->estimated ? " (ESTIMATED: not found on the scan)" : "",
           f->edge ? " (WARNING: at the file edge, frame may be cut; "
                     "margin too small)"
                   : "");
  }
}

int ftdev_detail(int argc, char** argv) {
  const char *scan = NULL, *jobp = NULL, *base = NULL, *dpath = NULL;
  int out = -1, fmt = -1, bw = -1, ring_mb = FT_RING_MB_DEFAULT, i, rc = 1;
  unsigned stall_ms = 0;
  long stall_every = 0, nmeta = 0, nraw = 0, ncal = 0, nfaul = 0, nd = 0;
  char path[1024], basebuf[1024], report[256];
  char *meta = NULL, *faul = NULL, *dtext = NULL;
  unsigned short *raw = NULL, *cal = NULL;
  static ft_job j;
  static ft_detail_opts o;
  static ft_detail_result res;
  ft_defects defects;
  ft_image metaimg;
  ft_detail* d;
  ft_err e;
  raw_ctx ctx;
  double secs;
  FILE* f;
  int pass;

  for (i = 0; i < argc; i++) {
    const char* a = argv[i];
    if (strcmp(a, "--job") == 0 && i + 1 < argc)
      jobp = argv[++i];
    else if (strcmp(a, "-o") == 0 && i + 1 < argc)
      base = argv[++i];
    else if (strcmp(a, "-d") == 0 && i + 1 < argc)
      dpath = argv[++i];
    else if (strcmp(a, "--out") == 0 && i + 1 < argc) {
      if ((out = parse_choice(argv[++i], OUTS, 2)) < 0) {
        detail_usage();
        return 2;
      }
    } else if (strcmp(a, "--fmt") == 0 && i + 1 < argc) {
      if ((fmt = parse_choice(argv[++i], FMTS, 2)) < 0) {
        detail_usage();
        return 2;
      }
    } else if (strcmp(a, "--bw") == 0 && i + 1 < argc) {
      if ((bw = parse_choice(argv[++i], BWS, 3)) < 0) {
        detail_usage();
        return 2;
      }
    } else if (strcmp(a, "--ring-mb") == 0 && i + 1 < argc) {
      ring_mb = atoi(argv[++i]);
      if (ring_mb < FT_RING_MB_MIN || ring_mb > FT_RING_MB_MAX) {
        fprintf(stderr, "ftdev: --ring-mb %d..%d\n", FT_RING_MB_MIN,
                FT_RING_MB_MAX);
        return 2;
      }
    } else if (strcmp(a, "--stall") == 0 && i + 1 < argc) {
      if (sscanf(argv[++i], "%u,%ld", &stall_ms, &stall_every) != 2 ||
          stall_every < 1) {
        fprintf(stderr, "ftdev: --stall ms,lines\n");
        return 2;
      }
    } else if (a[0] == '-' || scan) {
      detail_usage();
      return 2;
    } else
      scan = a;
  }
  if (!scan || !jobp) {
    detail_usage();
    return 2;
  }
  f = fopen(jobp, "r");
  if (!f) {
    fprintf(stderr, "ftdev: cannot read %s\n", jobp);
    return 1;
  }
  if (ft_job_read(&j, f, &e)) {
    fclose(f);
    fprintf(stderr, "ftdev: %s: %s\n", jobp, e.msg);
    return 1;
  }
  fclose(f);
  if (out >= 0) j.detail = out == 0 ? FT_DETAIL_NEG : FT_DETAIL_POS;
  if (fmt >= 0) j.detail_fmt = (ft_detail_fmt)fmt;
  if (bw >= 0) j.detail_bw = (ft_detail_bw)bw;
  if (j.detail == FT_DETAIL_NONE) {
    fprintf(stderr,
            "ftdev: %s is not a detailed-scan job (give "
            "--out)\n",
            jobp);
    return 2;
  }
  if (j.detail == FT_DETAIL_POS)
    j.detail_fmt = FT_DFMT_TIFF;
  else
    j.detail_bw = FT_BW_OFF;
  if (!base) {
    size_t n = strlen(scan);
    snprintf(basebuf, sizeof basebuf, "%s", scan);
    if (n > 11 && strncmp(basebuf + n - 11, "_pass", 5) == 0)
      basebuf[n - 11] = 0; /* <base>_passNN.raw -> <base> */
    else if (n > 4 && strcmp(basebuf + n - 4, ".raw") == 0)
      basebuf[n - 4] = 0;
    base = basebuf;
  }

  snprintf(path, sizeof path, "%s.txt", scan);
  meta = load_file(path, &nmeta);
  raw = load_file(scan, &nraw);
  snprintf(path, sizeof path, "%s.cal", scan);
  cal = load_file(path, &ncal);
  snprintf(path, sizeof path, "%s.faul", scan);
  faul = load_file(path, &nfaul);
  if (!meta || !raw || !cal) {
    fprintf(stderr, "ftdev: cannot read %s with its .txt and .cal\n", scan);
    goto done;
  }
  memset(&o, 0, sizeof o);
  o.scan.ppi = meta_double(meta, "ppi", 0);
  o.scan.bin = (int)meta_long(meta, "bin", 0);
  o.scan.x_out = meta_long(meta, "x", -1);
  o.scan.y_line = meta_long(meta, "y", -1);
  o.scan.px = meta_long(meta, "px", 0);
  o.scan.lines = meta_long(meta, "lines", 0);
  pass = (int)meta_long(meta, "pass", 0);
  if (o.scan.ppi <= 0 ||
      o.scan.bin < 1 ||
      o.scan.bin > 12 ||
      o.scan.x_out < 0 ||
      o.scan.y_line < 0 ||
      o.scan.px < 1 ||
      o.scan.px > 20000 ||
      o.scan.lines < 1 ||
      pass < 1) {
    fprintf(stderr,
            "ftdev: %s.txt lacks the pass geometry (ppi, bin, "
            "x, y, px, lines, pass)\n",
            scan);
    goto done;
  }
  if (dpath) {
    dtext = load_file(dpath, &nd);
    if (!dtext || ft_defects_parse(dtext, (size_t)nd, &defects, &e)) {
      fprintf(stderr, "ftdev: %s: %s\n", dpath, dtext ? e.msg : "cannot read");
      goto done;
    }
    o.defects = &defects;
  }
  ft_image_init(&metaimg);
  o.film = j.film;
  o.out = j.detail;
  o.fmt = j.detail_fmt;
  o.bw = j.detail_bw;
  memcpy(o.film_base, j.film_base, sizeof o.film_base);
  if (o.out == FT_DETAIL_POS && !j.has_film_base) {
    fprintf(stderr, "ftdev: %s has no film_base for --out pos\n", jobp);
    goto done;
  }
  o.margin_mm = FT_DETAIL_MARGIN_MM;
  o.holder = ft_holder_find(j.holder);
  o.meta = &metaimg;
  o.base = base;
  o.pass = pass;
  o.focus = meta_long(meta, "focus_offset", 0);
  for (i = 0; i < j.nframes; i++) {
    const ft_job_frame* fr = &j.f[i];
    if (!fr->include || fr->pass != pass) continue;
    o.f[o.nframes].index = fr->index;
    o.f[o.nframes].y0 = fr->y0;
    o.f[o.nframes].y1 = fr->y0 + fr->len;
    o.f[o.nframes].x0 = fr->x0;
    o.f[o.nframes].x1 = fr->x0 + fr->width;
    o.nframes++;
  }
  if (!o.holder || !o.nframes) {
    fprintf(stderr,
            "ftdev: %s: no frame in pass %d (or unknown "
            "holder)\n",
            jobp, pass);
    goto done;
  }
  d = ft_detail_open(&o, cal, (int)(ncal / (o.scan.px * 6)), faul,
                     (size_t)(faul ? nfaul : 0), &e);
  if (!d) {
    fprintf(stderr, "ftdev: %s\n", e.msg);
    goto done;
  }
  ctx.raw = raw;
  ctx.px = o.scan.px;
  if (feed(d, raw_line, &ctx, nraw / (o.scan.px * 6), (size_t)o.scan.px * 6,
           ring_mb, 0, stall_ms, stall_every, &secs, report, sizeof report)) {
    ft_detail_abort(d);
    goto done;
  }
  printf("pass %d: %s\n", pass, report);
  rc = ft_detail_close(d, &res, &e) ? 1 : 0;
  print_result(&res);
  if (rc) fprintf(stderr, "ftdev: %s\n", e.msg);
done:
  free(meta);
  free(raw);
  free(cal);
  free(faul);
  free(dtext);
  return rc;
}

/* ------------------------------------------------------------ ftdev bench */

typedef struct {
  unsigned short* lines; /* a few distinct lines, reused */
  long n, px;
} bench_ctx;

static const unsigned short* bench_line(long l, void* ctx) {
  bench_ctx* c = ctx;
  return c->lines + (size_t)(l % c->n) * c->px * 3;
}

int ftdev_bench(int argc, char** argv) {
  double ppi = 6300, mm = 10, pace = 0, secs, lo, hi, rate;
  int out = 0, fmt = 0, bw = 0, ring_mb = FT_RING_MB_DEFAULT, keep = 0;
  int i, rc;
  const char* base = "bench";
  static ft_detail_opts o;
  static ft_detail_result res;
  unsigned short* cal;
  unsigned seed = 1;
  char report[256];
  bench_ctx c;
  ft_image meta;
  ft_detail* d;
  ft_err e;
  long l, k;

  for (i = 0; i < argc; i++) {
    const char* a = argv[i];
    if (strcmp(a, "--ppi") == 0 && i + 1 < argc)
      ppi = atof(argv[++i]);
    else if (strcmp(a, "--mm") == 0 && i + 1 < argc)
      mm = atof(argv[++i]);
    else if (strcmp(a, "--out") == 0 && i + 1 < argc)
      out = parse_choice(argv[++i], OUTS, 2);
    else if (strcmp(a, "--fmt") == 0 && i + 1 < argc)
      fmt = parse_choice(argv[++i], FMTS, 2);
    else if (strcmp(a, "--bw") == 0 && i + 1 < argc)
      bw = parse_choice(argv[++i], BWS, 3);
    else if (strcmp(a, "--ring-mb") == 0 && i + 1 < argc)
      ring_mb = atoi(argv[++i]);
    else if (strcmp(a, "--pace") == 0)
      pace = 1;
    else if (strcmp(a, "--keep") == 0)
      keep = 1;
    else if (strcmp(a, "-o") == 0 && i + 1 < argc)
      base = argv[++i];
    else {
      fprintf(stderr,
              "usage: ftdev bench [--ppi P] [--mm L] "
              "[--out neg|pos] [--fmt dng|tiff] [--bw off|mono|rgb]\n"
              "             [--pace] [--ring-mb N] [-o base] "
              "[--keep]\n");
      return 2;
    }
  }
  if (out < 0 ||
      fmt < 0 ||
      bw < 0 ||
      !ft_bin_ok(ppi, 1) ||
      !(mm >= 1 && mm <= 231) ||
      ring_mb < FT_RING_MB_MIN ||
      ring_mb > FT_RING_MB_MAX) {
    fprintf(stderr, "ftdev bench: bad option (ppi must allow bin 1)\n");
    return 2;
  }
  memset(&o, 0, sizeof o);
  ft_dscan_reach(ppi, 1, &lo, &hi);
  if (lo < 0.5) lo = 0.5;
  if (hi > 31.5) hi = 31.5;
  ft_dscan_for(ppi, 1, lo, hi - lo, 10, mm + 2 * FT_DETAIL_MARGIN_MM, &o.scan);
  if (o.scan.px * 6 > 60000) {
    fprintf(stderr, "ftdev bench: line too long\n");
    return 2;
  }
  ft_image_init(&meta);
  o.film = FT_FILM_NEGATIVE;
  o.out = out ? FT_DETAIL_POS : FT_DETAIL_NEG;
  o.fmt = out ? FT_DFMT_TIFF : (ft_detail_fmt)fmt;
  o.bw = out ? (ft_detail_bw)bw : FT_BW_OFF;
  o.film_base[0] = 0.21;
  o.film_base[1] = 0.40;
  o.film_base[2] = 0.60;
  o.margin_mm = FT_DETAIL_MARGIN_MM;
  o.holder = ft_holder_find("24x217");
  o.meta = &meta;
  o.base = base;
  o.pass = 1;
  o.nframes = 1;
  o.f[0].index = 1;
  o.f[0].y0 = 10 + FT_DETAIL_MARGIN_MM;
  o.f[0].y1 = o.f[0].y0 + mm;
  o.f[0].x0 = lo + FT_DETAIL_MARGIN_MM;
  o.f[0].x1 = hi - FT_DETAIL_MARGIN_MM;

  c.n = 64;
  c.px = o.scan.px;
  c.lines = malloc((size_t)c.n * c.px * 3 * sizeof *c.lines);
  cal = malloc((size_t)250 * c.px * 3 * sizeof *cal);
  if (!c.lines || !cal) {
    fprintf(stderr, "ftdev bench: out of memory\n");
    free(c.lines);
    free(cal);
    return 1;
  }
  for (l = 0; l < 250; l++)
    for (k = 0; k < c.px * 3; k++)
      cal[l * c.px * 3 + k] = (unsigned short)(l < 200 ? 300 : 9000);
  for (k = 0; k < c.n * c.px * 3; k++) {
    seed = seed * 1103515245u + 12345u;
    c.lines[k] = (unsigned short)(400 + (seed >> 16) % 8000);
  }
  d = ft_detail_open(&o, cal, 250, NULL, 0, &e);
  if (!d) {
    fprintf(stderr, "ftdev bench: %s\n", e.msg);
    free(c.lines);
    free(cal);
    return 1;
  }
  /* the scanner: FT_OUT_PX_RATE output px/s over the full CCD line,
   * bin 1: lines per second */
  rate = FT_OUT_PX_RATE / FT_CCD_PIXELS;
  if (pace) pace = 1000.0 / rate;
  printf("bench: %g ppi, %ld px x %ld lines (%.1f Mpx), %s%s, ring %d MB%s\n",
         ppi, o.scan.px, o.scan.lines, o.scan.px * o.scan.lines / 1e6,
         OUTS[out],
         out   ? (bw == 1   ? " mono"
                  : bw == 2 ? " rgb"
                            : " colour")
         : fmt ? " tiff"
               : " dng",
         ring_mb, pace ? ", paced at the scanner's line rate" : "");
  rc = feed(d, bench_line, &c, o.scan.lines, (size_t)o.scan.px * 6, ring_mb,
            pace, 0, 0, &secs, report, sizeof report);
  if (rc) {
    ft_detail_abort(d);
  } else {
    double pxs = o.scan.px * o.scan.lines / secs;
    if (pace) /* the ring only matters paced */
      printf("%s\n", report);
    printf("%.1f s, %.2f Mpx/s of scan lines", secs, pxs / 1e6);
    if (!pace)
      printf("; the scanner sends %.1f lines/s: headroom %.0fx\n", rate,
             o.scan.lines / secs / rate);
    else
      printf("\n");
    rc = ft_detail_close(d, &res, &e) ? 1 : 0;
    if (rc) fprintf(stderr, "ftdev bench: %s\n", e.msg);
    if (!keep) {
      char p[700];
      remove(res.f[0].path);
      snprintf(p, sizeof p, "%s.txt", res.f[0].path);
      remove(p);
    }
  }
  free(c.lines);
  free(cal);
  return rc ? 1 : 0;
}

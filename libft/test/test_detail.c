/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A pass developed and written while it arrives, on synthetic scans
 * whose frames crept 1.5 mm since the preview. The oracle for the pixels
 * is the whole-image ft_develop of the same raw lines; libtiff (tests
 * only) reads the files back. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tiffio.h>

#include "ft_calib.h"
#include "ft_density.h"
#include "ft_detail.h"
#include "ft_develop.h"
#include "ft_tiff.h"
#include "test/ftest.h"
#include "test/synth.h"

#define CREEP 1.5 /* mm: the frames moved along since the preview */
#define BASE "test_detail_out"

typedef struct {
  ft_detail_scan s;
  int k;
  unsigned short *raw, *cal;
  int cal_lines;
} pass_t;

static unsigned rng = 4242;
static unsigned rnd(void) {
  rng = rng * 1103515245u + 12345u;
  return rng >> 8;
}

/* a pass scan at ppi / bin over 39.5..119.5 mm along, 1..31 mm across,
 * with frames at truth[i] (y0, len, blank) */
static void make_pass(pass_t* p, double ppi, int bin, int n,
                      const double (*truth)[3]) {
  synth_spec sp;
  ft_axes ax;
  float* t;
  long i, c, nl;
  int ch;

  ft_make_detail_scan(ppi, bin, 1.0, 30.0, 39.5, 80.0, &p->s);
  p->k = 12 / bin;
  ft_get_detail_axes(&p->s, 1, &ax);
  synth_default(&sp, FT_FILM_NEGATIVE);
  sp.px = (int)p->s.pixel_count;
  sp.lines = (int)p->s.line_count;
  sp.y_mm_per_line = 25.4 / ppi;
  sp.x_mm_per_px = ax.mm_per_pixel;
  sp.x_mm0 = ax.first_pixel_mm;
  sp.film_y0 = -50;
  sp.film_y1 = 1000;
  /* synth line 0 is aligned line 0: holder ax.y_mm0 */
  for(i = 0; i < n; i++)
    synth_add(&sp, truth[i][0] - ax.first_line_mm, truth[i][1],
              (int)truth[i][2]);
  t = synth_preview(&sp);
  CHECK(t != NULL);
  nl = p->s.line_count;
  p->raw = malloc((size_t)nl * p->s.pixel_count * 3 * sizeof *p->raw);
  p->cal_lines = 250;
  p->cal = malloc((size_t)p->cal_lines * p->s.pixel_count * 3 * sizeof *p->cal);
  CHECK(p->raw && p->cal);
  /* white noise 2, 4, 8 DN in R, G, B (mono weights) */
  for(i = 0; i < p->cal_lines; i++)
    for(c = 0; c < p->s.pixel_count; c++)
      for(ch = 0; ch < 3; ch++)
        p->cal[(i * p->s.pixel_count + c) * 3 + ch] =
          (unsigned short)(i < 200
                             ? 300
                             : 4300 + (long)(rnd() % (4u << ch)) - (2 << ch));
  for(i = 0; i < nl * p->s.pixel_count * 3; i++)
    p->raw[i] = (unsigned short)(300 + floor(t[i] * 4000 + 0.5));
  free(t);
}

static void free_pass(pass_t* p) {
  free(p->raw);
  free(p->cal);
}

static void opts_for(ft_detail_options* o, const pass_t* p,
                     ft_detail_output out, int n, const double (*expect)[2],
                     const ft_image* meta) {
  int i;
  memset(o, 0, sizeof *o);
  o->scan = p->s;
  o->film = FT_FILM_NEGATIVE;
  o->output = out;
  o->format =
    out == FT_DETAIL_NEGATIVE ? FT_DETAIL_FORMAT_DNG : FT_DETAIL_FORMAT_TIFF;
  o->film_base_density[0] = -log10(0.62);
  o->film_base_density[1] = -log10(0.40);
  o->film_base_density[2] = -log10(0.25);
  o->margin_mm = FT_DETAIL_MARGIN_MM;
  o->holder = ft_find_holder("24x217");
  o->metadata = meta;
  o->base_path = BASE;
  o->pass = 1;
  o->focus_offset_steps = -250;
  o->frame_count = n;
  for(i = 0; i < n; i++) {
    o->frames[i].index = i + 1;
    o->frames[i].start_mm = expect[i][0];
    o->frames[i].end_mm = expect[i][0] + expect[i][1];
    o->frames[i].across_start_mm = 4.1;
    o->frames[i].across_end_mm = 28.1;
  }
}

static int run(const pass_t* p, const ft_detail_options* o, long lines,
               ft_detail_result* r, ft_err* e) {
  ft_detail* d = ft_open_detail(o, p->cal, p->cal_lines, NULL, 0, e);
  long l;
  CHECK(d != NULL);
  for(l = 0; l < lines; l++)
    CHECK(ft_add_detail_line(d, p->raw + (size_t)l * p->s.pixel_count * 3, e) ==
          0);
  return ft_close_detail(d, r, e);
}

/* the whole-image develop of the pass (the oracle), image rows = px */
static void develop(const pass_t* p, ft_image* im) {
  ft_develop_options o;
  ft_err e;
  memset(&o, 0, sizeof o);
  o.first_pixel = (int)p->s.first_pixel;
  o.bin = p->s.bin;
  ft_init_image(im);
  CHECK(ft_develop(p->raw, (int)p->s.line_count, (int)p->s.pixel_count, p->cal,
                   p->cal_lines, NULL, 0, &o, im, &e) == 0);
}

/* every pixel of the file against want(x, y, s) */
static long compare(const char* path, int w, int h, int spp,
                    unsigned short (*want)(int, int, int)) {
  TIFF* t = TIFFOpen(path, "r");
  uint32_t iw = 0, ih = 0;
  unsigned short* buf;
  long bad = 0;
  int x, y, r, c, s;
  CHECK(t != NULL);
  CHECK(TIFFGetField(t, TIFFTAG_IMAGEWIDTH, &iw) && iw == (uint32_t)w);
  CHECK(TIFFGetField(t, TIFFTAG_IMAGELENGTH, &ih) && ih == (uint32_t)h);
  buf = malloc(TIFFTileSize(t));
  CHECK(buf != NULL);
  for(y = 0; y < h; y += FT_TILE_PIXELS)
    for(x = 0; x < w; x += FT_TILE_PIXELS) {
      CHECK(TIFFReadTile(t, buf, (uint32_t)x, (uint32_t)y, 0, 0) > 0);
      for(r = 0; r < FT_TILE_PIXELS && y + r < h; r++)
        for(c = 0; c < FT_TILE_PIXELS && x + c < w; c++)
          for(s = 0; s < spp; s++)
            bad +=
              buf[(r * FT_TILE_PIXELS + c) * spp + s] != want(x + c, y + r, s);
    }
  free(buf);
  TIFFClose(t);
  return bad;
}

/* the oracle as the frame file should hold it */
static const ft_image* g_im;
static const int* g_r;
static ft_density_encoder g_dens;
static double g_w[3];

static unsigned short t16_at(int x, int y, int s) {
  float v =
    g_im->pixels[((size_t)(g_r[2] + y) * g_im->width + g_r[0] + x) * 3 + s];
  unsigned short u;
  ft_quantise_transmittance16(&v, &u, 1);
  return u;
}

static unsigned short neg_px(int x, int y, int s) { return t16_at(x, y, s); }

static unsigned short pos_px(int x, int y, int s) {
  return g_dens.tables[s][t16_at(x, y, s)];
}

static unsigned short mono_px(int x, int y, int s) {
  unsigned short t3[3], o;
  (void)s;
  t3[0] = t16_at(x, y, 0);
  t3[1] = t16_at(x, y, 1);
  t3[2] = t16_at(x, y, 2);
  ft_encode_density_mono(&g_dens, g_w, t3, &o, 1);
  return o;
}

static int exists(const char* p) {
  FILE* f = fopen(p, "rb");
  if(f) fclose(f);
  return f != NULL;
}

static char* slurp(const char* p) {
  static char buf[4096];
  FILE* f = fopen(p, "rb");
  size_t n;
  CHECK(f != NULL);
  n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = 0;
  return buf;
}

static void cleanup(int n) {
  static const char* ext[] = {".dng", ".tif"};
  char p[300];
  int i, k;
  for(i = 1; i <= n; i++)
    for(k = 0; k < 2; k++) {
      snprintf(p, sizeof p, BASE "_frame%02d%s", i, ext[k]);
      remove(p);
      strcat(p, ".txt");
      remove(p);
      snprintf(p, sizeof p, BASE "_frame%02d%s.partial", i, ext[k]);
      remove(p);
    }
  remove(BASE "_pass01.raw");
  remove(BASE "_pass01.raw.cal");
  remove(BASE "_pass01.raw.txt");
}

int main(void) {
  static const double truth[2][3] = {{44, 36, 0}, {82, 36, 0}};
  static const double expect[2][2] = {{44 - CREEP, 36}, {82 - CREEP, 36}};
  ft_detail_result r;
  ft_detail_options o;
  ft_image meta, im;
  ft_err e;
  pass_t p;
  int i;

  TIFFSetWarningHandler(NULL);
  ft_init_image(&meta);

  CHECK(ft_get_detail_kind(FT_DETAIL_NEGATIVE, FT_DETAIL_FORMAT_DNG,
                           FT_BW_OFF) == FT_TIFF_DNG);
  CHECK(ft_get_detail_kind(FT_DETAIL_NEGATIVE, FT_DETAIL_FORMAT_TIFF,
                           FT_BW_OFF) == FT_TIFF_RGB);
  CHECK(ft_get_detail_kind(FT_DETAIL_POSITIVE, FT_DETAIL_FORMAT_TIFF,
                           FT_BW_MONO) == FT_TIFF_GRAY);
  CHECK(ft_get_detail_kind(FT_DETAIL_POSITIVE, FT_DETAIL_FORMAT_TIFF,
                           FT_BW_RGB) == FT_TIFF_RGB);

  /* names */
  {
    char b[64];
    CHECK(ft_make_detail_path("scan", 3, 0, b, sizeof b) == 0);
    CHECK(strcmp(b, "scan_frame03.dng") == 0);
    CHECK(ft_make_detail_path("scan", 12, 1, b, sizeof b) == 0);
    CHECK(strcmp(b, "scan_frame12.tif") == 0);
    CHECK(ft_make_detail_path("scan", 1, 1, b, 20) == -1); /* + .partial */
  }

  /* 525 ppi, bin 12 (thumbnail 1:1): DNG negatives */
  make_pass(&p, 525, 12, 2, truth);
  develop(&p, &im);
  opts_for(&o, &p, FT_DETAIL_NEGATIVE, 2, expect, &meta);
  CHECK(run(&p, &o, p.s.line_count, &r, &e) == 0);
  CHECK(r.lines == p.s.line_count && r.was_detected && r.frame_count == 2);
  for(i = 0; i < 2; i++) {
    int rr[4];
    ft_detail_frame_result* f = &r.frames[i];
    char side[700];
    CHECK(ft_find_scan_ranges(&p.s, o.frames[i].start_mm - 3,
                              o.frames[i].end_mm + 3, 4.1 - 3, 28.1 + 3,
                              rr) == 0);
    CHECK(f->width == rr[1] - rr[0] && f->height == rr[3] - rr[2]);
    CHECK(!f->is_estimated && !f->touches_edge);
    CHECK_NEAR(f->shift_mm, CREEP, 0.3);
    /* the box starts where the true frame does: margin + creep */
    CHECK_NEAR(f->box[0], (3 + CREEP) * 525 / 25.4, 4);
    CHECK_NEAR(f->box[2], 36 * 525 / 25.4, 6);
    CHECK(exists(f->path));
    snprintf(side, sizeof side, "%s.partial", f->path);
    CHECK(!exists(side));
    g_im = &im;
    g_r = rr;
    CHECK(compare(f->path, f->width, f->height, 3, neg_px) == 0);
    { /* DefaultCrop = the box */
      TIFF* t = TIFFOpen(f->path, "r");
      uint16_t ph = 0;
      CHECK(t && TIFFGetField(t, TIFFTAG_PHOTOMETRIC, &ph) && ph == 34892);
      TIFFClose(t);
    }
    snprintf(side, sizeof side, "%s.txt", f->path);
    {
      char want[80], *txt = slurp(side);
      CHECK(strstr(txt, "estimated=0\n") && strstr(txt, "pass=1\n"));
      CHECK(strstr(txt, "focus_offset=-250\n"));
      snprintf(want, sizeof want, "frame_box=%d %d %d %d\n", f->box[0],
               f->box[1], f->box[2], f->box[3]);
      CHECK(strstr(txt, want) != NULL);
    }
  }
  ft_free_image(&im);
  free_pass(&p);
  cleanup(2);

  /* a frame the preview had but the film has not (blank): estimated,
   * shifted by the others' median creep */
  {
    static const double t3[3][3] = {{44, 36, 0}, {82, 36, 1}};
    make_pass(&p, 525, 12, 2, t3);
    opts_for(&o, &p, FT_DETAIL_NEGATIVE, 2, expect, &meta);
    CHECK(run(&p, &o, p.s.line_count, &r, &e) == 0);
    CHECK(!r.frames[0].is_estimated);
    CHECK(r.frames[1].is_estimated);
    CHECK_NEAR(r.frames[1].shift_mm, r.frames[0].shift_mm, 1e-9);
    CHECK(strstr(slurp(BASE "_frame02.dng.txt"), "estimated=1\n"));
    cleanup(2);
    free_pass(&p);
  }

  /* the scan stops early: the finished frame is written, the other stays
   * .partial; abort leaves everything .partial */
  make_pass(&p, 525, 12, 2, truth);
  opts_for(&o, &p, FT_DETAIL_NEGATIVE, 2, expect, &meta);
  CHECK(run(&p, &o, p.s.line_count - 200, &r, &e) == -1);
  CHECK(strstr(e.message, "frame 2") != NULL);
  CHECK(exists(BASE "_frame01.dng") && !exists(BASE "_frame01.dng.partial"));
  CHECK(exists(BASE "_frame02.dng.partial") && !exists(BASE "_frame02.dng"));
  cleanup(2);
  {
    ft_detail* d = ft_open_detail(&o, p.cal, p.cal_lines, NULL, 0, &e);
    CHECK(d != NULL);
    CHECK(ft_add_detail_line(d, p.raw, &e) == 0);
    ft_abort_detail(d);
    CHECK(exists(BASE "_frame01.dng.partial"));
    CHECK(!exists(BASE "_frame01.dng"));
    cleanup(2);
  }

  /* keep_raw: the lines as they arrived, with .cal and .txt */
  o.keep_raw = 1;
  o.format = FT_DETAIL_FORMAT_TIFF;
  CHECK(run(&p, &o, p.s.line_count, &r, &e) == 0);
  {
    FILE* f = fopen(BASE "_pass01.raw", "rb");
    size_t n = (size_t)p.s.line_count * p.s.pixel_count * 3, got;
    unsigned short* back = malloc(n * 2);
    CHECK(f && back);
    got = fread(back, 2, n + 1, f);
    fclose(f);
    CHECK(got == n && memcmp(back, p.raw, n * 2) == 0);
    free(back);
    CHECK(strstr(slurp(BASE "_pass01.raw.txt"), "bin=12\n") != NULL);
    CHECK(exists(BASE "_pass01.raw.cal"));
    CHECK(exists(BASE "_frame01.tif")); /* TIFF negative */
    { /* with the linear scanner-RGB profile (ft_build_linear_icc_profile) */
      TIFF* t = TIFFOpen(BASE "_frame01.tif", "r");
      uint32_t n = 0;
      void* pr = NULL;
      CHECK(t != NULL);
      CHECK(TIFFGetField(t, TIFFTAG_ICCPROFILE, &n, &pr) && n == 500);
      CHECK(memcmp((char*)pr + 12, "mntrRGB XYZ ", 12) == 0);
      TIFFClose(t);
    }
  }
  o.keep_raw = 0;
  cleanup(2);

  /* refusals before anything is written */
  {
    ft_detail_options bad = o;
    bad.frames[1].start_mm = 300;
    bad.frames[1].end_mm = 336;
    CHECK(ft_open_detail(&bad, p.cal, p.cal_lines, NULL, 0, &e) == NULL);
    CHECK(strstr(e.message, "frame 2") != NULL);
    bad = o;
    bad.output = FT_DETAIL_POSITIVE;
    bad.film = FT_FILM_SLIDE;
    CHECK(ft_open_detail(&bad, p.cal, p.cal_lines, NULL, 0, &e) == NULL);
    bad = o;
    bad.base_path = "no/such/dir/x";
    CHECK(ft_open_detail(&bad, p.cal, p.cal_lines, NULL, 0, &e) == NULL);
    cleanup(2);
  }
  free_pass(&p);

  /* 1050 ppi, bin 6 (thumbnail 2:1): archive positives, colour and mono */
  make_pass(&p, 1050, 6, 2, truth);
  develop(&p, &im);
  opts_for(&o, &p, FT_DETAIL_POSITIVE, 2, expect, &meta);
  CHECK(run(&p, &o, p.s.line_count, &r, &e) == 0);
  CHECK(r.was_detected);
  ft_init_density_encoder(&g_dens, o.film_base_density);
  for(i = 0; i < 2; i++) {
    int rr[4];
    CHECK(!r.frames[i].is_estimated);
    CHECK_NEAR(r.frames[i].shift_mm, CREEP, 0.3);
    CHECK(ft_find_scan_ranges(&p.s, o.frames[i].start_mm - 3,
                              o.frames[i].end_mm + 3, 4.1 - 3, 28.1 + 3,
                              rr) == 0);
    g_im = &im;
    g_r = rr;
    CHECK(compare(r.frames[i].path, r.frames[i].width, r.frames[i].height, 3,
                  pos_px) == 0);
  }
  { /* density is not colorimetric: no profile on a positive */
    TIFF* t = TIFFOpen(BASE "_frame01.tif", "r");
    uint32_t n = 0;
    void* pr = NULL;
    CHECK(t != NULL);
    CHECK(!TIFFGetField(t, TIFFTAG_ICCPROFILE, &n, &pr));
    TIFFClose(t);
  }
  {
    char* txt = slurp(BASE "_frame01.tif.txt");
    CHECK(strstr(txt, "encoding=ft-density v1\n") != NULL);
    CHECK(strstr(txt, "offset=0.1\nrange=4.0\nbw=off\n") != NULL);
  }
  cleanup(2);
  o.bw = FT_BW_MONO;
  CHECK(run(&p, &o, p.s.line_count, &r, &e) == 0);
  {
    double var[3];
    int rr[4];
    CHECK(ft_measure_white_variance(p.cal, p.cal_lines, (int)p.s.pixel_count,
                                    var, &e) == 0);
    ft_compute_noise_weights(var, g_w);
    CHECK(g_w[0] > g_w[1] && g_w[1] > g_w[2]); /* blue noisiest */
    CHECK(ft_find_scan_ranges(&p.s, o.frames[0].start_mm - 3,
                              o.frames[0].end_mm + 3, 4.1 - 3, 28.1 + 3,
                              rr) == 0);
    g_r = rr;
    CHECK(compare(r.frames[0].path, r.frames[0].width, r.frames[0].height, 1,
                  mono_px) == 0);
    CHECK(strstr(slurp(BASE "_frame01.tif.txt"), "bw=mono\n") != NULL);
  }
  cleanup(2);
  ft_free_image(&im);
  free_pass(&p);
  return TEST_DONE();
}

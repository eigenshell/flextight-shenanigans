/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ftdev stock learn <roll.edit> <frame.dng>... -o <name.look> [--replace]
 *
 *   Stock hints for a colour negative film, from a roll (ft_look.h). The
 *   user's tint and curve choices do not enter the hint, only the roll's
 *   noise reduction (the estimate is measured after it). Per frame (cut to its
 * sidecar's box, with its film base and the roll's noise reduction for it, as
 * export does), the channel ranges of the stock-free auto-estimate as ratios to
 * green; the median over the frames; pooled, weighted by frame count, with what
 * <name.look> learned before (--replace: start over). The look's [taste]
 * becomes the roll's. The ratio is the stock's dye-layer balance: one frame's
 * estimate follows its scene, the median over frames (and rolls) does not.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_dng.h"
#include "share.h"

#define SPREAD_WARN 0.05
#define FEW_FRAMES 6

static ft_edit g_roll, g_look; /* ~100 KB each */
static char g_text[65536];

static void usage(void) {
  fprintf(stderr,
          "usage: ftdev stock learn <roll.edit> <frame.dng>... "
          "-o <name.look> [--replace]\n");
}

/* the frame's ratios range[c] / range[green]; 0 ok, 1 error, 2 refused
 * (messages printed) */
static int measure(const char* dng, double ratio[3]) {
  char path[4096];
  char* side;
  long n = 0;
  ft_share_sidecar sc;
  ft_image im, cut;
  const ft_image* one[1];
  ft_recipe r;
  float base[3];
  ft_err e;
  int rc = 1, c;

  ft_init_image(&im);
  ft_init_image(&cut);
  if(snprintf(path, sizeof path, "%s.txt", dng) >= (int)sizeof path) {
    fprintf(stderr, "ftdev: path too long: %.60s...\n", dng);
    return 2;
  }
  side = ftdev_load(path, &n);
  if(!side) {
    fprintf(stderr,
            "ftdev: %s: no sidecar %s (learning needs the "
            "frame box and film base: use ftdev cut)\n",
            dng, path);
    return 2;
  }
  if(ft_parse_share_sidecar(side, (size_t)n, &sc, &e)) {
    fprintf(stderr, "ftdev: %s: %s\n", path, e.message);
    free(side);
    return 2;
  }
  free(side);
  if(sc.film != FT_FILM_NEGATIVE) {
    fprintf(stderr,
            "ftdev: %s: a slide: stock hints are for colour "
            "negatives\n",
            dng);
    return 2;
  }
  if(ft_read_dng(dng, &im, &e) || ft_crop_image(&im, sc.box, &cut, &e)) {
    fprintf(stderr, "ftdev: %s: %s\n", dng, e.message);
    goto out;
  }
  /* measured as export estimates: after the frame's noise reduction */
  {
    ft_taste t;
    ft_look_detail d;
    ft_get_frame_taste(&g_roll, sc.frame, &t, NULL);
    ft_get_look_detail(&t, &d);
    if(ftdev_denoise(&im, &sc, &cut, &d)) goto out;
  }
  for(c = 0; c < 3; c++) base[c] = (float)sc.film_base_density[c];
  if(!sc.has_film_base)
    fprintf(stderr,
            "ftdev: note: %s: no film_base; estimated from the "
            "frame itself\n",
            dng);
  one[0] = &cut;
  if(ft_resolve_recipe(NULL, NULL, FT_RENDER_NEGATIVE,
                       sc.has_film_base ? base : NULL, one, 1, &r, &e)) {
    fprintf(stderr, "ftdev: %s: %s\n", dng, e.message);
    goto out;
  }
  for(c = 0; c < 3; c++) ratio[c] = r.range_density[c] / r.range_density[1];
  printf("%s: frame %d: r/g %.3f, b/g %.3f\n", dng, sc.frame, ratio[0],
         ratio[2]);
  rc = 0;
out:
  ft_free_image(&im);
  ft_free_image(&cut);
  return rc;
}

/* parse path as kind into ed; 0 ok, 1 unreadable, 2 bad (printed) */
static int load(const char* path, ft_look_kind k, ft_edit* ed) {
  long n = 0;
  char* txt = ftdev_load(path, &n);
  ft_err e;
  if(!txt) return 1;
  if(ft_parse_edit(txt, (size_t)n, k, ed, &e)) {
    fprintf(stderr, "ftdev: %s: %s\n", path, e.message);
    free(txt);
    return 2;
  }
  free(txt);
  return 0;
}

int ftdev_stock(int argc, char** argv) {
  const char *roll = NULL, *outp = NULL, *dngs[FT_MAX_FRAMES];
  static double ratio[FT_MAX_FRAMES][3];
  ft_stock old, learned;
  int ndng = 0, replace = 0, has_old = 0, i, r;
  double spread;
  size_t len;
  FILE* f;
  ft_err e;

  if(argc < 1 || strcmp(argv[0], "learn") != 0) {
    usage();
    return 2;
  }
  for(i = 1; i < argc; i++) {
    if(strcmp(argv[i], "-o") == 0 && i + 1 < argc)
      outp = argv[++i];
    else if(strcmp(argv[i], "--replace") == 0)
      replace = 1;
    else if(argv[i][0] == '-') {
      usage();
      return 2;
    } else if(!roll)
      roll = argv[i];
    else if(ndng == FT_MAX_FRAMES) {
      fprintf(stderr, "ftdev: more than %d frames\n", FT_MAX_FRAMES);
      return 2;
    } else
      dngs[ndng++] = argv[i];
  }
  if(!roll || !outp || ndng == 0) {
    usage();
    return 2;
  }
  r = load(roll, FT_EDIT_FILE, &g_roll);
  if(r) {
    if(r == 1) fprintf(stderr, "ftdev: cannot read %s\n", roll);
    return 2;
  }
  /* what the look learned before; else the roll's own b&w weights, but
   * not its ratio (that came from a look: pooling it again would count
   * those frames twice) */
  if(!replace) {
    r = load(outp, FT_LOOK_FILE, &g_look);
    if(r == 2) return 2;
    if(r == 0 && g_look.stock.is_present) {
      old = g_look.stock;
      has_old = 1;
    }
  }
  if(!has_old && g_roll.stock.is_present) {
    old = g_roll.stock;
    old.has_ratio = 0;
    old.frame_count = 0;
    has_old = 1;
  }
  for(i = 0; i < ndng; i++) {
    r = measure(dngs[i], ratio[i]);
    if(r) return r;
  }
  if(ft_learn_stock((const double (*)[3])ratio, ndng, has_old ? &old : NULL,
                    &learned, &spread, &e)) {
    fprintf(stderr, "ftdev: %s\n", e.message);
    return 1;
  }
  ft_init_edit(&g_look);
  g_look.stock = learned;
  g_look.taste = g_roll.taste;
  len = ft_format_edit(&g_look, FT_LOOK_FILE, g_text, sizeof g_text);
  if(len >= sizeof g_text) {
    fprintf(stderr, "ftdev: look text too long\n");
    return 1;
  }
  f = fopen(outp, "w");
  if(f) {
    int bad = fwrite(g_text, 1, len, f) != len;
    if(fclose(f)) bad = 1;
    if(bad) f = NULL;
  }
  if(!f) {
    fprintf(stderr, "ftdev: cannot write %s\n", outp);
    return 1;
  }
  printf("%s: range_ratio %.3f 1 %.3f from %" PRId64
         " frames (this roll: %d, "
         "spread %.3f)\n",
         outp, learned.range_ratio[0], learned.range_ratio[2],
         learned.frame_count, ndng, spread);
  if(spread > SPREAD_WARN)
    fprintf(stderr,
            "ftdev: warning: the frames disagree (spread %.3f > "
            "%.2f): check for frames without a bright "
            "neutral\n",
            spread, SPREAD_WARN);
  if(ndng < FEW_FRAMES)
    fprintf(stderr,
            "ftdev: note: %d frames from this roll; more frames "
            "(and rolls) steady the hint\n",
            ndng);
  return 0;
}

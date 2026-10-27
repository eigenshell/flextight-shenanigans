/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Scan to share as a print (libft/ft_print.h):
 *
 *   ftdev print <frame.dng>... --emulsion <id> [--cmyd c,m,y,d] [-o out.jpg]
 *   ftdev print <frame.dng>... --roll <roll.print> [--job <file.job>]
 *   ftdev emulsions
 *
 * The frames given are the roll: the auto balance compares them (on stats
 * proxies, FT_PRINT_STATS_EDGE px), so print a roll's frames together.
 * Each frame (ftdev cut's <base>_frameNN.dng and its sidecar) is cropped
 * to its box, noise-reduced, printed through the emulsion with its clicks
 * (--cmyd for every frame, or the .print file's per frame), resized to the
 * share size and written as <frame>.jpg (sRGB) with how it was made in
 * <frame>.jpg.txt and the JPEG comment. -o: one frame only.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_dng.h"
#include "ft_emulsion.h"
#include "ft_job.h"
#include "ft_jpeg.h"
#include "ft_pm.h"
#include "ft_print.h"
#include "ft_print_file.h"
#include "ft_render_auto.h"
#include "ft_share_print.h"
#include "render_flags.h"
#include "share.h"

#define PATH_CAP 4096

static ft_print_file g_pf;

typedef struct {
  const char* dng;
  int frame; /* -1: unknown */
  int has_side;
  ft_share_sidecar sc;
  ft_image proxy;
} pframe;

static void print_usage(void) {
  fprintf(stderr,
          "usage: ftdev print <frame.dng>... --emulsion <id> "
          "[--cmyd c,m,y,d] [-o out.jpg]\n"
          "       ftdev print <frame.dng>... --roll <roll.print> "
          "[--job <file.job>]\n"
          "       ftdev emulsions\n");
}

int ftdev_emulsions(int argc, char** argv) {
  static const char* MODE[3] = {"negative", "slide", "b&w"};
  const ft_emulsion* em;
  int i;
  (void)argv;
  if(argc) {
    print_usage();
    return 2;
  }
  printf("emulsions (science revision %d):\n", FT_EMULSION_REV);
  for(i = 0; (em = ft_emulsion_at(i)) != NULL; i++)
    printf("  %-16s %-9s %s\n", em->id, MODE[em->mode], em->name);
  return 0;
}

static void list_ids(void) {
  const ft_emulsion* em;
  int i;
  for(i = 0; (em = ft_emulsion_at(i)) != NULL; i++)
    fprintf(stderr, "%s%s", i ? " " : "", em->id);
}

/* the frame number in "..._frameNN.dng"; -1 if none */
static int frame_of_name(const char* path) {
  const char* p = strstr(path, "_frame");
  const char* last = NULL;
  while(p) {
    last = p;
    p = strstr(p + 1, "_frame");
  }
  if(last) {
    char* end;
    long v = strtol(last + 6, &end, 10);
    if(end != last + 6 && strcmp(end, ".dng") == 0 && v >= 0 && v <= 9999)
      return (int)v;
  }
  return -1;
}

/* the frame's DNG cropped to its box (the sidecar's, else all of it) */
static int load_cut(const pframe* f, ft_image* im, ft_image* cut) {
  int box[4];
  ft_err e;
  if(ft_dng_read(f->dng, im, &e)) {
    fprintf(stderr, "ftdev: %s\n", e.msg);
    return 1;
  }
  if(f->has_side) {
    memcpy(box, f->sc.box, sizeof box);
  } else {
    box[0] = box[1] = 0;
    box[2] = im->w;
    box[3] = im->h;
  }
  if(ft_image_crop(im, box, cut, &e)) {
    fprintf(stderr, "ftdev: %s: %s (sidecar from another DNG?)\n", f->dng,
            e.msg);
    return 1;
  }
  return 0;
}

int ftdev_print(int argc, char** argv) {
  static pframe fs[FT_MAX_FRAMES];
  static ft_print_frame pfr[FT_MAX_FRAMES];
  const char *emulsion = NULL, *cmyd = NULL, *roll_path = NULL;
  const char *job_path = NULL, *outp = NULL;
  const ft_emulsion* em;
  const ft_image* proxies[FT_MAX_FRAMES];
  ft_cmyd k1 = {0, 0, 0, 0};
  ft_print_roll roll;
  float base[3];
  int i, n = 0, rc = 1, nbase = 0;
  double bsum[3][FT_MAX_FRAMES];
  ft_err e;

  for(i = 0; i < argc; i++) {
    const char* a = argv[i];
    if(strcmp(a, "--emulsion") == 0 && i + 1 < argc)
      emulsion = argv[++i];
    else if(strcmp(a, "--cmyd") == 0 && i + 1 < argc)
      cmyd = argv[++i];
    else if(strcmp(a, "--roll") == 0 && i + 1 < argc)
      roll_path = argv[++i];
    else if(strcmp(a, "--job") == 0 && i + 1 < argc)
      job_path = argv[++i];
    else if(strcmp(a, "-o") == 0 && i + 1 < argc)
      outp = argv[++i];
    else if(a[0] == '-') {
      print_usage();
      return 2;
    } else {
      if(n == FT_MAX_FRAMES) {
        fprintf(stderr, "ftdev: more than %d frames\n", FT_MAX_FRAMES);
        return 2;
      }
      memset(&fs[n], 0, sizeof fs[n]);
      fs[n].dng = a;
      ft_image_init(&fs[n].proxy);
      n++;
    }
  }
  if(n == 0) {
    print_usage();
    return 2;
  }
  if(roll_path && (emulsion || cmyd)) {
    fprintf(stderr,
            "ftdev: with --roll the emulsion and the clicks come "
            "from the .print file\n");
    return 2;
  }
  if(!roll_path && !emulsion) {
    fprintf(stderr,
            "ftdev: print needs --emulsion <id> or --roll "
            "<roll.print>\n");
    return 2;
  }
  if(job_path && !roll_path) {
    fprintf(stderr, "ftdev: --job needs --roll\n");
    return 2;
  }
  if(outp && n != 1) {
    fprintf(stderr, "ftdev: -o needs exactly one frame\n");
    return 2;
  }
  if(roll_path) {
    long nt = 0;
    char* txt = ftdev_load(roll_path, &nt);
    if(!txt) {
      fprintf(stderr, "ftdev: cannot read %s\n", roll_path);
      return 1;
    }
    if(ft_print_file_parse(txt, (size_t)nt, &g_pf, &e)) {
      fprintf(stderr, "ftdev: %s: %s\n", roll_path, e.msg);
      free(txt);
      return 2;
    }
    free(txt);
    emulsion = g_pf.emulsion;
  }
  em = ft_emulsion_find(emulsion);
  if(!em) {
    fprintf(stderr, "ftdev: --emulsion: '%s' is not an emulsion (", emulsion);
    list_ids();
    fprintf(stderr, ")\n");
    return 2;
  }
  if(cmyd) {
    char extra;
    if(sscanf(cmyd, "%d,%d,%d,%d%c", &k1.c, &k1.m, &k1.y, &k1.d, &extra) != 4 ||
       ft_print_clicks_ok(em, k1, &e)) {
      fprintf(stderr,
              "ftdev: --cmyd is c,m,y,d (whole numbers, "
              "-%d..%d; b&w: 0,0,0,d)\n",
              FT_PRINT_CLICKS_MAX, FT_PRINT_CLICKS_MAX);
      return 2;
    }
  }

  /* each frame: its sidecar, frame number and stats proxy */
  for(i = 0; i < n; i++) {
    pframe* f = &fs[i];
    char path[PATH_CAP];
    long ns = 0;
    char* side;
    ft_image im, cut;
    if(snprintf(path, sizeof path, "%s.txt", f->dng) >= (int)sizeof path) {
      fprintf(stderr, "ftdev: path too long: %.60s...\n", f->dng);
      rc = 2;
      goto out;
    }
    side = ftdev_load(path, &ns);
    if(side) {
      if(ft_share_sidecar_parse(side, (size_t)ns, &f->sc, &e)) {
        fprintf(stderr, "ftdev: %s: %s\n", path, e.msg);
        free(side);
        goto out;
      }
      free(side);
      f->has_side = 1;
    }
    f->frame =
      f->has_side && f->sc.frame > 0 ? f->sc.frame : frame_of_name(f->dng);
    if(roll_path && f->frame < 0) {
      fprintf(stderr,
              "ftdev: %s: no frame number (no sidecar, no "
              "_frameNN in the name)\n",
              f->dng);
      rc = 2;
      goto out;
    }
    if(f->has_side &&
       (f->sc.film == FT_FILM_SLIDE) != (em->mode == FT_RENDER_SLIDE)) {
      fprintf(stderr, "ftdev: %s is a %s; %s prints %s\n", f->dng,
              f->sc.film == FT_FILM_SLIDE ? "slide" : "negative", em->id,
              em->mode == FT_RENDER_SLIDE ? "slides" : "negatives");
      rc = 2;
      goto out;
    }
    if(f->has_side && f->sc.has_film_base) {
      int c;
      for(c = 0; c < 3; c++) bsum[c][nbase] = f->sc.film_base[c];
      nbase++;
    }
    ft_image_init(&im);
    ft_image_init(&cut);
    if(load_cut(f, &im, &cut)) {
      ft_image_free(&im);
      ft_image_free(&cut);
      goto out;
    }
    ft_image_free(&im);
    if(ft_share_print_proxy(&cut, &f->proxy, &e)) {
      fprintf(stderr, "ftdev: %s\n", e.msg);
      ft_image_free(&cut);
      goto out;
    }
    ft_image_free(&cut);
    proxies[i] = &f->proxy;
  }

  /* the roll: base, [frame N] check, the balance across the frames */
  if(nbase) {
    int c;
    for(c = 0; c < 3; c++) base[c] = (float)ft_print_median(bsum[c], nbase);
  } else {
    if(ft_render_base_from_frames(proxies, n, em->mode, base, &e)) {
      fprintf(stderr, "ftdev: %s\n", e.msg);
      goto out;
    }
    fprintf(stderr,
            "ftdev: note: no film_base in the sidecars; "
            "estimated from the frames\n");
  }
  if(roll_path) {
    int idx[FT_MAX_FRAMES], ni = 0;
    if(job_path) {
      static ft_job job;
      FILE* jf = fopen(job_path, "r");
      if(!jf || ft_job_read(&job, jf, &e)) {
        fprintf(stderr, "ftdev: %s: %s\n", job_path,
                jf ? e.msg : "cannot read");
        if(jf) fclose(jf);
        rc = 2;
        goto out;
      }
      fclose(jf);
      for(i = 0; i < job.nframes; i++) idx[ni++] = job.f[i].index;
    } else {
      for(i = 0; i < n; i++) idx[ni++] = fs[i].frame;
    }
    if(ft_print_file_unknown(&g_pf, idx, ni, &e))
      fprintf(stderr, "ftdev: warning: %s: %s\n", roll_path, e.msg);
  }
  if(ft_print_roll_measure(em, base, proxies, n, pfr, &roll, &e)) {
    fprintf(stderr, "ftdev: %s\n", e.msg);
    goto out;
  }
  if(n < 6 && em->balance == &ft_pm_balance_frontier)
    fprintf(stderr,
            "ftdev: note: %d frame%s: print a roll's frames "
            "together for the roll's colour balance\n",
            n, n == 1 ? "" : "s");

  /* each frame: the print */
  for(i = 0; i < n; i++) {
    pframe* f = &fs[i];
    ft_cmyd k = roll_path ? ft_print_file_clicks(&g_pf, f->frame) : k1;
    int rot = roll_path ? ft_print_file_rotation(&g_pf, f->frame) : 0;
    ft_share_print_info info;
    ft_image im;
    unsigned char* jpg;
    size_t nj;
    char out[PATH_CAP], text[FT_SHARE_TEXT_MAX];
    int r2;
    if(outp) {
      snprintf(out, sizeof out, "%s", outp);
    } else {
      size_t l = strlen(f->dng);
      if(l > 4 && strcmp(f->dng + l - 4, ".dng") == 0) l -= 4;
      if(l + 5 > sizeof out) {
        fprintf(stderr, "ftdev: path too long: %.60s...\n", f->dng);
        rc = 2;
        goto out;
      }
      memcpy(out, f->dng, l);
      memcpy(out + l, ".jpg", 5);
    }
    ft_image_init(&im);
    if(ft_dng_read(f->dng, &im, &e)) {
      fprintf(stderr, "ftdev: %s\n", e.msg);
      goto out;
    }
    if(!f->has_side)
      fprintf(stderr,
              "ftdev: note: no sidecar for %s; printing at "
              "full size\n",
              f->dng);
    printf("print: %s frame %d, %s, cmyd %d %d %d %d\n", f->dng, f->frame,
           em->id, k.c, k.m, k.y, k.d);
    if(ft_share_print_frame(em, &roll, &pfr[i], k, rot, &im,
                            f->has_side ? &f->sc : NULL, f->frame, &jpg, &nj,
                            text, &info, &e)) {
      fprintf(stderr, "ftdev: %s: %s\n", f->dng, e.msg);
      ft_image_free(&im);
      goto out;
    }
    ft_image_free(&im);
    if(info.clamped)
      fprintf(stderr,
              "ftdev: warning: %s: the filtration is at its "
              "limit (50 CC, 6 stops)\n",
              f->dng);
    if(info.nr_notes & FT_NR_NOTE_DISAGREE)
      fprintf(stderr,
              "ftdev: warning: sidecar noise_profile disagrees "
              "with the DNG; using the DNG's\n");
    if(info.nr_notes & FT_NR_NOTE_ESTIMATED)
      fprintf(stderr, "ftdev: warning: no noise profile, estimated\n");
    r2 = ftdev_write_jpeg(out, jpg, nj, info.w, info.h, FT_SPACE_SRGB,
                          FT_GAMUT_SOFT, FT_JPEG_QUALITY, text);
    free(jpg);
    if(r2) {
      rc = r2;
      goto out;
    }
  }
  rc = 0;
out:
  for(i = 0; i < n; i++) ft_image_free(&fs[i].proxy);
  return rc;
}

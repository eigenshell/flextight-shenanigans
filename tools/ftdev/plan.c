/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ftdev plan: a size per frame for a share scan (ft_plan_share_sizes), the
 * web page's planner from a terminal. Rewrites the job (or writes -o). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_holder.h"
#include "ft_job.h"
#include "ft_share.h"
#include "share.h"

static void plan_usage(void) {
  fprintf(stderr,
          "usage: ftdev plan <strip.job> --size 4mp|6mp|px:frames "
          "[--size ...]\n"
          "                  [--hf-half-res on|off] [-o out.job]\n"
          "  frames: job frame numbers, e.g. 1-3,5\n");
}

/* "4mp", "6mp" or px; 0 if none of them */
static double size_of(const char* s, size_t n) {
  char buf[32], *end;
  double v;
  if(n >= sizeof buf) return 0;
  memcpy(buf, s, n);
  buf[n] = 0;
  if(strcmp(buf, "4mp") == 0) return FT_SHARE_4MP_PIXELS;
  if(strcmp(buf, "6mp") == 0) return FT_SHARE_6MP_PIXELS;
  v = strtod(buf, &end);
  return end != buf && !*end && v > 0 ? v : 0;
}

int ftdev_plan(int argc, char** argv) {
  static ft_job j;
  static ft_share_frame sel[FT_MAX_FRAMES];
  double tg[FT_MAX_FRAMES];
  const char *path = NULL, *out = NULL;
  const ft_holder* h;
  int i, k, hf = 0, nsize = 0, ng, g;
  uint32_t n;
  ft_err e;
  FILE* f;

  memset(tg, 0, sizeof tg);
  for(i = 0; i < argc; i++)
    if(strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
      out = argv[++i];
    } else if(strcmp(argv[i], "--hf-half-res") == 0 && i + 1 < argc) {
      i++;
      if(strcmp(argv[i], "on") != 0 && strcmp(argv[i], "off") != 0) {
        plan_usage();
        return 2;
      }
      hf = strcmp(argv[i], "on") == 0;
    } else if(strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
      i++; /* read once the job is loaded */
    } else if(argv[i][0] != '-' && !path) {
      path = argv[i];
    } else {
      plan_usage();
      return 2;
    }
  if(!path) {
    plan_usage();
    return 2;
  }
  f = fopen(path, "r");
  if(!f) {
    fprintf(stderr, "plan: cannot open %s\n", path);
    return 1;
  }
  k = ft_read_job(&j, f, &e);
  fclose(f);
  if(k) {
    fprintf(stderr, "plan: %s: %s\n", path, e.message);
    return 1;
  }
  h = ft_find_holder(j.holder);
  if(!h) {
    fprintf(stderr, "plan: unknown holder '%s'\n", j.holder);
    return 1;
  }
  for(i = 0; i < argc; i++) {
    const char *a, *colon;
    double px;
    if(strcmp(argv[i], "--size") != 0 || i + 1 >= argc) {
      if(strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--hf-half-res") == 0)
        i++;
      continue;
    }
    a = argv[++i];
    colon = strchr(a, ':');
    if(!colon || !colon[1]) {
      fprintf(stderr,
              "plan: --size wants size:frames, e.g. "
              "4mp:1-3,5\n");
      return 2;
    }
    px = size_of(a, (size_t)(colon - a));
    if(px == 0) {
      fprintf(stderr, "plan: bad size '%.*s' (4mp, 6mp or px)\n",
              (int)(colon - a), a);
      return 2;
    }
    if(ft_select_share_frames(&j, colon + 1, sel, &n, &e)) {
      fprintf(stderr, "plan: %s\n", e.message);
      return 1;
    }
    for(k = 0; k < (int)n; k++)
      for(g = 0; g < (int)j.frame_count; g++)
        if(j.frames[g].index == sel[k].index) {
          if(tg[g] != 0) {
            fprintf(stderr, "plan: frame %d has two sizes\n", sel[k].index);
            return 1;
          }
          tg[g] = px;
        }
    nsize++;
  }
  if(!nsize) {
    fprintf(stderr, "plan: no --size\n");
    return 2;
  }
  ng = ft_plan_share_sizes(&j, tg, hf, &ft_lens_default, h->scan_length_mm, &e);
  if(ng < 0 || ft_validate_job(&j, &ft_lens_default, &e)) {
    fprintf(stderr, "plan: %s\n", e.message);
    return 1;
  }
  if(!out) out = path;
  f = fopen(out, "w");
  if(!f || ft_write_job(&j, f) || fclose(f)) {
    fprintf(stderr, "plan: cannot write %s\n", out);
    return 1;
  }
  for(g = 1; g <= ng; g++) {
    const ft_job_frame* first = NULL;
    int nf = 0;
    for(i = 0; i < (int)j.frame_count; i++)
      if(j.frames[i].is_included && j.frames[i].group == g) nf++;
    printf("window %d: frame%s", g, nf > 1 ? "s" : "");
    for(i = 0, k = 0; i < (int)j.frame_count; i++)
      if(j.frames[i].is_included && j.frames[i].group == g) {
        printf("%s %d", k++ ? "," : "", j.frames[i].index);
        if(!first) first = &j.frames[i];
      }
    printf(", %g px, %g ppi, bin %d\n", ft_get_frame_share_pixels(&j, first),
           first->ppi, first->bin);
  }
  printf("%d window(s); next: ftpoc scan --job %s\n", ng, out);
  return 0;
}

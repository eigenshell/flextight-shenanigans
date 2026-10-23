/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A standard preview (<base>.raw, .raw.cal, .raw.txt) through the
 * streaming detector, line by line as the scanner sends it: one line per
 * sealed frame, "frame N type y0 y1 x0 x1 grid at", at = the preview
 * lines in when it was sealed (end: the strip's end sealed it). For
 * tests/test_stream_real.py on real strips (not in git).
 *
 *   stream_dump <base> [neg|slide] */
#include "ft_develop.h"
#include "ft_preview.h"
#include "ft_stream_detect.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *load(const char *path, long *n) {
  FILE *f = fopen(path, "rb");
  void *b;
  if(!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  *n = ftell(f);
  rewind(f);
  b = malloc((size_t)*n + 1);
  if(b && fread(b, 1, (size_t)*n, f) != (size_t)*n) {
    free(b);
    b = NULL;
  }
  fclose(f);
  if(b)
    ((char *)b)[*n] = 0;
  return b;
}

static long key(const char *t, const char *k) {
  const char *p = t;
  size_t kl = strlen(k);
  while(p && *p) {
    if(strncmp(p, k, kl) == 0 && p[kl] == '=')
      return atol(p + kl + 1);
    p = strchr(p, '\n');
    if(p)
      p++;
  }
  return -1;
}

static void put(const ft_sdet *d, int i, const char *at) {
  static const char *T[] = {"full", "half", "panorama", "partial"};
  const ft_sdet_frame *f = &d->f[i];
  printf(
      "frame %d %s %.3f %.3f %.3f %.3f %d %s\n", f->index, T[f->type], f->y0,
      f->y1, f->x0, f->x1, f->grid, at
  );
}

int main(int argc, char **argv) {
  static ft_sdet d;
  char path[1024], at[32];
  long nr, nc, nt, x, px;
  unsigned short *raw, *cal;
  char *txt;
  ft_develop_opts o;
  ft_axes ax;
  ft_err e;
  float *t, base[3];
  int nl, l, i, done = 0;
  ft_film film = argc > 2 && strcmp(argv[2], "slide") == 0 ? FT_FILM_SLIDE
                                                           : FT_FILM_NEGATIVE;

  if(argc < 2) {
    fprintf(stderr, "usage: stream_dump <base> [neg|slide]\n");
    return 2;
  }
  snprintf(path, sizeof path, "%s.raw", argv[1]);
  raw = load(path, &nr);
  snprintf(path, sizeof path, "%s.raw.cal", argv[1]);
  cal = load(path, &nc);
  snprintf(path, sizeof path, "%s.raw.txt", argv[1]);
  txt = load(path, &nt);
  if(!raw || !cal || !txt) {
    fprintf(stderr, "stream_dump: %s: files missing\n", argv[1]);
    return 1;
  }
  px = key(txt, "px");
  x = key(txt, "x");
  if(px != FT_PREVIEW_PX || key(txt, "bin") != 12) {
    fprintf(stderr, "stream_dump: not a standard preview\n");
    return 1;
  }
  memset(&o, 0, sizeof o);
  o.x = (int)x;
  o.bin = 12;
  t = ft_develop_t(
      raw, (int)(nr / (px * 6)), (int)px, cal, (int)(nc / (px * 6)), NULL, 0,
      &o, &nl, &e
  );
  ft_preview_axes(ft_preview_find("standard"), x, &ax);
  if(!t || ft_sdet_init(&d, &ax, (int)px, film, ft_holder_find("24x217"), &e)) {
    fprintf(stderr, "stream_dump: %s\n", e.msg);
    return 1;
  }
  for(l = 0; l < nl; l++) {
    ft_sdet_line(&d, t + (size_t)l * px * 3);
    for(; done < d.nframes; done++) {
      snprintf(at, sizeof at, "%d", l + 1);
      put(&d, done, at);
    }
  }
  ft_sdet_end(&d);
  for(i = done; i < d.nframes; i++)
    put(&d, i, "end");
  if(ft_sdet_base(&d, base))
    printf("base %.4f %.4f %.4f\n", base[0], base[1], base[2]);
  printf("lines %d\n", nl);
  ft_sdet_free(&d);
  free(t);
  return 0;
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A standard preview (<base>.raw, .raw.cal, .raw.txt) through the
 * streaming detector, line by line as the scanner sends it: one line per
 * sealed frame, "frame N type y0 y1 x0 x1 grid at", at = the preview
 * lines in when it was sealed (end: the strip's end sealed it). For
 * tests/test_stream_real.py on real strips (not in git).
 *
 *   stream_dump <base> [neg|slide] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_develop.h"
#include "ft_preview.h"
#include "ft_stream_detect.h"

static void* load(const char* path, long* n) {
  FILE* f = fopen(path, "rb");
  void* b;
  if(!f) return NULL;
  fseek(f, 0, SEEK_END);
  *n = ftell(f);
  rewind(f);
  b = malloc((size_t)*n + 1);
  if(b && fread(b, 1, (size_t)*n, f) != (size_t)*n) {
    free(b);
    b = NULL;
  }
  fclose(f);
  if(b) ((char*)b)[*n] = 0;
  return b;
}

static long key(const char* t, const char* k) {
  const char* p = t;
  size_t kl = strlen(k);
  while(p && *p) {
    if(strncmp(p, k, kl) == 0 && p[kl] == '=') return atol(p + kl + 1);
    p = strchr(p, '\n');
    if(p) p++;
  }
  return -1;
}

static void put(const ft_stream_detector* d, int i, const char* at) {
  static const char* T[] = {"full", "half", "panorama", "partial"};
  const ft_stream_frame* f = &d->frames[i];
  printf("frame %d %s %.3f %.3f %.3f %.3f %d %s\n", f->index, T[f->type],
         f->start_mm, f->end_mm, f->across_start_mm, f->across_end_mm,
         f->is_from_grid, at);
}

int main(int argc, char** argv) {
  static ft_stream_detector d;
  char path[1024], at[32];
  long nr, nc, nt, x, px;
  unsigned short *raw, *cal;
  char* txt;
  ft_develop_options o;
  ft_axes ax;
  ft_err e;
  float *t, base[3];
  uint32_t nl, i, done = 0;
  int l;
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
  if(px != FT_PREVIEW_PIXEL_COUNT || key(txt, "bin") != 12) {
    fprintf(stderr, "stream_dump: not a standard preview\n");
    return 1;
  }
  memset(&o, 0, sizeof o);
  o.first_pixel = (int)x;
  o.bin = 12;
  t = ft_develop_transmittance(raw, (int)(nr / (px * 6)), (int)px, cal,
                               (int)(nc / (px * 6)), NULL, 0, &o, &nl, &e);
  ft_get_preview_axes(ft_find_preview("standard"), x, &ax);
  if(!t || ft_init_stream_detector(&d, &ax, (int)px, film,
                                   ft_find_holder("24x217"), &e)) {
    fprintf(stderr, "stream_dump: %s\n", e.message);
    return 1;
  }
  for(l = 0; l < (int)nl; l++) {
    ft_detect_stream_line(&d, t + (size_t)l * px * 3);
    for(; done < d.frame_count; done++) {
      snprintf(at, sizeof at, "%d", l + 1);
      put(&d, done, at);
    }
  }
  ft_end_stream_detection(&d);
  for(i = done; i < d.frame_count; i++) put(&d, i, "end");
  if(ft_get_stream_base(&d, base))
    printf("base %.4f %.4f %.4f\n", base[0], base[1], base[2]);
  printf("lines %d\n", nl);
  ft_free_stream_detector(&d);
  free(t);
  return 0;
}

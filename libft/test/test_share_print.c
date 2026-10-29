/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ft_share_print: the float format, the print text, the share tail and
 * one frame end to end on a synthetic negative (ftdev print's outputs
 * are pinned in tests/test_print.py). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_emulsion.h"
#include "ft_jpeg.h"
#include "ft_share_print.h"
#include "test/ftest.h"

/* width and height from the JPEG's SOF0 */
static void sof(const unsigned char* b, size_t n, int* w, int* h) {
  size_t i;
  *w = *h = 0;
  for(i = 0; i + 9 < n; i++)
    if(b[i] == 0xFF && b[i + 1] == 0xC0) {
      *h = b[i + 5] << 8 | b[i + 6];
      *w = b[i + 7] << 8 | b[i + 8];
      return;
    }
}

/* does b[0..n) contain t? */
static int has(const unsigned char* b, size_t n, const char* t) {
  size_t i, l = strlen(t);
  for(i = 0; i + l <= n; i++)
    if(memcmp(b + i, t, l) == 0) return 1;
  return 0;
}

int main(void) {
  const ft_emulsion* em = ft_find_emulsion("generic-neg");
  static char text[FT_SHARE_TEXT_BYTES], t2[FT_SHARE_TEXT_BYTES];
  char s[32];
  ft_image im, proxy;
  const ft_image* px[1];
  ft_print_frame pf;
  ft_print_roll roll;
  ft_share_sidecar sc;
  ft_share_print_info info;
  ft_cmyd k = {0, 2, 0, -1};
  float base[3] = {0.2f, 0.4f, 0.6f};
  unsigned char *jpg, *j2;
  size_t n, n2;
  ft_err e;
  int x, y, w, h;

  CHECK(strcmp(ft_format_float(0.5f, s, sizeof s), "0.5") == 0);
  CHECK(strcmp(ft_format_float(0.1f, s, sizeof s), "0.1") == 0);
  CHECK(strcmp(ft_format_float(1.0f / 3, s, sizeof s), "0.33333334") == 0);
  CHECK(strcmp(ft_format_float(1234567.0f, s, sizeof s), "1234567") == 0);

  /* a 300 x 200 negative-ish frame: orange base, a soft gradient */
  CHECK(em != NULL);
  ft_init_image(&im);
  CHECK(ft_allocate_image(&im, 300, 200, &e) == 0);
  for(y = 0; y < 200; y++)
    for(x = 0; x < 300; x++) {
      float* p = im.pixels + ((size_t)y * 300 + x) * 3;
      float g = 0.15f + 0.4f * x / 300.0f + 0.1f * y / 200.0f;
      p[0] = 0.6f * g;
      p[1] = 0.4f * g;
      p[2] = 0.25f * g;
    }
  im.ppi = 1500;
  ft_init_image(&proxy);
  CHECK(ft_make_share_proxy(&im, &proxy, &e) == 0);
  CHECK(proxy.width <= FT_PRINT_STATS_EDGE_PIXELS && proxy.width == 300);
  px[0] = &proxy;
  CHECK(ft_measure_print_roll(em, base, px, 1, &pf, &roll, &e) == 0);

  /* no sidecar: the whole image at full size */
  CHECK(ft_print_share_frame(em, &roll, &pf, k, 0, &im, NULL, 4, &jpg, &n, text,
                             &info, &e) == 0);
  sof(jpg, n, &w, &h);
  CHECK(w == 300 && h == 200 && info.width == 300 && info.height == 200);
  CHECK(strncmp(text, "emulsion=generic-neg\nscience=", 29) == 0);
  CHECK(strstr(text, "\nframe=4\ncmyd=0 2 0 -1\n") != NULL);
  CHECK(info.noise_notes == FT_NOISE_NOTE_ESTIMATED);
  /* the comment is the text */
  CHECK(has(jpg, n, text));
  /* the text alone: the same, snprintf's contract */
  {
    ft_recipe r;
    ft_print_auto a;
    int len;
    CHECK(ft_make_print_recipe(em, &roll, &pf, k, &r, &a, NULL, &e) == 0);
    len = ft_format_share_print_text(t2, sizeof t2, em, 4, k, &a, &r,
                                     &ft_share_detail);
    CHECK(len == (int)strlen(text) && strcmp(t2, text) == 0);
    CHECK(ft_format_share_print_text(NULL, 0, em, 4, k, &a, &r,
                                     &ft_share_detail) == len);
    CHECK(ft_format_share_print_text(t2, sizeof t2, em, -1, k, &a, &r,
                                     &ft_share_detail) == len - 8);
  }

  /* a sidecar: its box, its export scale; same pixels as the tail */
  memset(&sc, 0, sizeof sc);
  sc.box[0] = 20;
  sc.box[1] = 10;
  sc.box[2] = 240;
  sc.box[3] = 160;
  sc.export_scale = 0.5;
  CHECK(ft_print_share_frame(em, &roll, &pf, k, 0, &im, &sc, 4, &j2, &n2, t2,
                             &info, &e) == 0);
  sof(j2, n2, &w, &h);
  CHECK(w == 120 && h == 80 && info.width == 120);
  free(j2);
  /* a turn: 90 and 270 swap the sides, the text says so */
  CHECK(ft_print_share_frame(em, &roll, &pf, k, 90, &im, &sc, 4, &j2, &n2, t2,
                             &info, &e) == 0);
  sof(j2, n2, &w, &h);
  CHECK(w == 80 && h == 120 && info.width == 80 && info.height == 120);
  CHECK(strstr(t2, "\nrotate=90\n") != NULL && has(j2, n2, t2));
  free(j2);
  CHECK(ft_print_share_frame(em, &roll, &pf, k, 45, &im, &sc, 4, &j2, &n2, t2,
                             &info, &e) == -1);
  CHECK(strcmp(e.message, "rotate: 45 is not 0, 90, 180 or 270") == 0);
  CHECK(j2 == NULL && n2 == 0);
  /* the pixels: a quarter turn clockwise, and back */
  {
    ft_image a, b, c;
    int x, y, same = 1;
    ft_init_image(&a);
    ft_init_image(&b);
    ft_init_image(&c);
    CHECK(ft_allocate_image(&a, 3, 2, &e) == 0);
    for(x = 0; x < 18; x++) a.pixels[x] = (float)x;
    CHECK(
      ft_rotate_image(&a, 90, &b, &e) == 0 && b.width == 2 && b.height == 3);
    /* the left column, bottom up, is the new top row */
    CHECK(b.pixels[0] == a.pixels[9] && b.pixels[3] == a.pixels[0]);
    CHECK(
      ft_rotate_image(&b, 270, &c, &e) == 0 && c.width == 3 && c.height == 2);
    for(y = 0; y < 18; y++) same &= c.pixels[y] == a.pixels[y];
    CHECK(same);
    ft_free_image(&c);
    CHECK(ft_rotate_image(&a, 180, &c, &e) == 0);
    CHECK(c.pixels[0] == a.pixels[15] && c.pixels[15] == a.pixels[0]);
    ft_free_image(&a);
    ft_free_image(&b);
    ft_free_image(&c);
  }
  sc.box[2] = 400;
  CHECK(ft_print_share_frame(em, &roll, &pf, k, 0, &im, &sc, 4, &j2, &n2, t2,
                             &info, &e) == -1);
  CHECK(strcmp(e.message, "box 20,10 400x160 outside the 300x200 image") == 0);
  CHECK(j2 == NULL && n2 == 0);

  /* per-frame balance: measured from the frame alone (a roll of one),
   * whatever else is on the strip; a roll's balance of the same frame
   * with another differs */
  {
    ft_image im2, proxy2;
    const ft_image* two[2];
    ft_print_frame pf1, pf2[2];
    ft_print_roll r1, r2;
    unsigned char *ja, *jb;
    size_t na, nb;
    ft_init_image(&im2);
    ft_init_image(&proxy2);
    CHECK(ft_allocate_image(&im2, 300, 200, &e) == 0);
    for(x = 0; x < 300 * 200 * 3; x++) /* denser, greener */
      im2.pixels[x] = im.pixels[x] * (x % 3 == 1 ? 0.5f : 0.3f);
    CHECK(ft_make_share_proxy(&im2, &proxy2, &e) == 0);
    CHECK(ft_measure_share_balance(em, base, &proxy, &r1, &pf1, &e) == 0);
    CHECK(r1.frame_count == 1 &&
          r1.has_reference == roll.has_reference &&
          r1.reference_density == roll.reference_density &&
          pf1.mean_density[1] == pf.mean_density[1]);
    two[0] = &proxy;
    two[1] = &proxy2;
    CHECK(ft_measure_print_roll(em, base, two, 2, pf2, &r2, &e) == 0);
    CHECK(ft_print_share_frame(em, &r1, &pf1, k, 0, &im, NULL, 4, &ja, &na, t2,
                               &info, &e) == 0);
    CHECK(ft_print_share_frame(em, &r2, &pf2[0], k, 0, &im, NULL, 4, &jb, &nb,
                               t2, &info, &e) == 0);
    CHECK(na == n && memcmp(ja, jpg, n) == 0);
    CHECK(na != nb || memcmp(ja, jb, na) != 0);
    free(ja);
    free(jb);
    ft_free_image(&proxy2);
    ft_free_image(&im2);
  }

  free(jpg);
  ft_free_image(&proxy);
  ft_free_image(&im);
  return TEST_DONE();
}

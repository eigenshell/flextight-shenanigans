/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ft_jpeg: write, then decode with libjpeg. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* jpeglib.h uses FILE and size_t without including <stdio.h> itself,
 * so it has to come after the standard headers. Keep clang-format from
 * sorting it to the top. */
/* clang-format off */
#include <jpeglib.h>
/* clang-format on */
#include "ft_icc.h"
#include "ft_jpeg.h"
#include "test/ftest.h"

enum { W = 320, H = 200 };

int main(void) {
  static unsigned char rgb[W * H * 3], back[W * H * 3];
  unsigned char icc[FT_ICC_MAX];
  const char* path = "test/jpeg_test_out.jpg";
  size_t n;
  ft_err e;
  int x, y;

  /* left half: a ramp; right half: flat mid grey 128 and a red patch */
  for(y = 0; y < H; y++)
    for(x = 0; x < W; x++) {
      unsigned char* p = rgb + (y * W + x) * 3;
      if(x < W / 2) {
        p[0] = (unsigned char)(x * 255 / (W / 2));
        p[1] = (unsigned char)(y * 255 / H);
        p[2] = 60;
      } else if(y < H / 2) {
        p[0] = p[1] = p[2] = 128;
      } else {
        p[0] = 200;
        p[1] = 30;
        p[2] = 40;
      }
    }
  n = ft_icc_build(FT_SPACE_DISPLAY_P3, icc, sizeof icc);
  CHECK(n > 0);
  CHECK(ft_jpeg_write(path, rgb, W, H, FT_JPEG_QUALITY, 1411.2, icc, n,
                      "look=hp5\ngrade=2\n", &e) == 0);

  {
    struct jpeg_decompress_struct d;
    struct jpeg_error_mgr em;
    unsigned char* prof = NULL;
    unsigned int plen = 0;
    FILE* f = fopen(path, "rb");
    CHECK(f != NULL);
    d.err = jpeg_std_error(&em);
    jpeg_create_decompress(&d);
    jpeg_stdio_src(&d, f);
    jpeg_save_markers(&d, JPEG_APP0 + 2, 0xFFFF);
    jpeg_save_markers(&d, JPEG_COM, 0xFFFF);
    jpeg_read_header(&d, TRUE);
    {
      jpeg_saved_marker_ptr m;
      int ncom = 0;
      for(m = d.marker_list; m; m = m->next)
        if(m->marker == JPEG_COM) {
          ncom++;
          CHECK(m->data_length == 17 &&
                memcmp(m->data, "look=hp5\ngrade=2\n", 17) == 0);
        }
      CHECK(ncom == 1);
    }
    CHECK(d.image_width == W && d.image_height == H);
    CHECK(d.num_components == 3 && d.jpeg_color_space == JCS_YCbCr);
    /* 4:4:4 */
    CHECK(d.comp_info[0].h_samp_factor == 1 &&
          d.comp_info[0].v_samp_factor == 1 &&
          d.comp_info[1].h_samp_factor == 1);
    CHECK(d.saw_JFIF_marker &&
          d.density_unit == 1 &&
          d.X_density == 1411 &&
          d.Y_density == 1411);
    CHECK(jpeg_read_icc_profile(&d, &prof, &plen));
    CHECK(plen == n && memcmp(prof, icc, n) == 0);
    free(prof);
    d.out_color_space = JCS_RGB;
    jpeg_start_decompress(&d);
    while(d.output_scanline < d.output_height) {
      JSAMPROW row = back + (size_t)d.output_scanline * W * 3;
      jpeg_read_scanlines(&d, &row, 1);
    }
    jpeg_finish_decompress(&d);
    jpeg_destroy_decompress(&d);
    fclose(f);
  }
  /* flat patches survive within 1 (inside, away from edges) */
  for(y = 10; y < H / 2 - 10; y++)
    for(x = W / 2 + 10; x < W - 10; x++) {
      unsigned char* p = back + (y * W + x) * 3;
      CHECK(
        abs(p[0] - 128) <= 1 && abs(p[1] - 128) <= 1 && abs(p[2] - 128) <= 1);
    }
  for(y = H / 2 + 10; y < H - 10; y++)
    for(x = W / 2 + 10; x < W - 10; x++) {
      unsigned char* p = back + (y * W + x) * 3;
      CHECK(abs(p[0] - 200) <= 2 && abs(p[1] - 30) <= 2 && abs(p[2] - 40) <= 2);
    }

  /* no profile, no density */
  CHECK(ft_jpeg_write(path, rgb, W, H, 75, 0, NULL, 0, NULL, &e) == 0);
  {
    /* no comment: no COM marker */
    long fn = 0;
    unsigned char* all;
    FILE* f = fopen(path, "rb");
    CHECK(f != NULL);
    fseek(f, 0, SEEK_END);
    fn = ftell(f);
    rewind(f);
    all = malloc((size_t)fn);
    CHECK(all && fread(all, 1, (size_t)fn, f) == (size_t)fn);
    fclose(f);
    for(x = 0; x + 1 < fn; x++) CHECK(!(all[x] == 0xFF && all[x + 1] == 0xFE));
    free(all);
  }
  /* refusals */
  CHECK(ft_jpeg_write(path, rgb, W, H, 0, 0, NULL, 0, NULL, &e) == -1);
  CHECK(ft_jpeg_write(path, rgb, 0, H, 90, 0, NULL, 0, NULL, &e) == -1);
  CHECK(ft_jpeg_write("test/no_such_dir/x.jpg", rgb, W, H, 90, 0, NULL, 0, NULL,
                      &e) == -1);
  CHECK(strstr(e.msg, "no_such_dir") != NULL);
  remove(path);
  return TEST_DONE();
}

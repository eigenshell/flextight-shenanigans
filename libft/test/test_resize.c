/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ft_output.h"
#include "ft_resize.h"
#include "test/ftest.h"

enum { W = 400, H = 300 };
static float in[W * H * 3], out[W * H * 3], scratch[W * H * 3];

int main(void) {
  ft_err e;
  int ow, oh, x, y, c;

  /* sizes */
  ft_resize_dims(2126, 1417, 2000.0 / 2126, &ow, &oh);
  CHECK(ow == 2000 && oh == 1333);
  ft_resize_dims(10, 10, 0.01, &ow, &oh);
  CHECK(ow == 1 && oh == 1);
  CHECK(ft_resize_scratch_bytes(W, H, 133, 100) ==
        (size_t)H * 133 * 3 * sizeof(float));

  /* a flat field stays exactly flat, edges included */
  for (x = 0; x < W * H * 3; x++) in[x] = 0.3f;
  CHECK(ft_resize(in, W, H, out, 133, 100, scratch, &e) == 0);
  for (x = 0; x < 133 * 100 * 3; x++) CHECK_NEAR(out[x], 0.3, 1e-6);

  /* one-pixel black/white lines keep their mean 0.5 in linear light;
   * the same resize on sRGB-encoded values would come out far darker */
  for (y = 0; y < H; y++)
    for (x = 0; x < W; x++)
      for (c = 0; c < 3; c++) in[(y * W + x) * 3 + c] = (float)((x + y) % 2);
  CHECK(ft_resize(in, W, H, out, 133, 100, scratch, &e) == 0);
  for (y = 3; y < 97; y++)
    for (x = 3; x < 130; x++) CHECK_NEAR(out[(y * 133 + x) * 3 + 1], 0.5, 0.01);
  {
    /* encoded 0/1 are the same 0/1: the gamma-domain result is 0.5
     * encoded, i.e. 0.214 linear, not 0.5 */
    double lin = ft_space_decode(FT_SPACE_SRGB, out[(50 * 133 + 60) * 3]);
    CHECK(lin < 0.25);
  }

  /* a linear ramp stays linear inside (Lanczos reproduces lines) */
  for (y = 0; y < H; y++)
    for (x = 0; x < W; x++)
      for (c = 0; c < 3; c++)
        in[(y * W + x) * 3 + c] = (float)(x + 2 * y) / (W + 2 * H);
  CHECK(ft_resize(in, W, H, out, 200, 150, scratch, &e) == 0);
  for (y = 5; y < 145; y++)
    for (x = 5; x < 195; x++)
      CHECK_NEAR(
        out[(y * 200 + x) * 3 + 2],
        ((x + 0.5) * 2 - 0.5 + 2 * ((y + 0.5) * 2 - 0.5)) / (W + 2 * H), 1e-5);

  /* channels stay separate */
  for (x = 0; x < W * H; x++) {
    in[3 * x] = 1;
    in[3 * x + 1] = 0;
    in[3 * x + 2] = 0.5f;
  }
  CHECK(ft_resize(in, W, H, out, 7, 5, scratch, &e) == 0);
  for (x = 0; x < 35; x++) {
    CHECK_NEAR(out[3 * x], 1, 1e-6);
    CHECK_NEAR(out[3 * x + 1], 0, 1e-6);
    CHECK_NEAR(out[3 * x + 2], 0.5, 1e-6);
  }

  /* scale 1 copies; upscaling and empty sizes are refused */
  CHECK(ft_resize(in, W, H, out, W, H, scratch, &e) == 0);
  CHECK(memcmp(in, out, sizeof in) == 0);
  CHECK(ft_resize(in, W, H, out, W + 1, H, scratch, &e) == -1);
  CHECK(strstr(e.msg, "upscale") != NULL);
  CHECK(ft_resize(in, W, H, out, 0, H, scratch, &e) == -1);
  return TEST_DONE();
}

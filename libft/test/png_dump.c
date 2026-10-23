/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Writes a known 3x2 PNG to argv[1] for tests/test_libft.py (zlib check). */
#include "ft_png.h"
#include <stdio.h>

int main(int argc, char **argv) {
  static const unsigned char rgb[18] = {
      255, 0, 0, 0, 255, 0, 0, 0, 255, 10, 20, 30, 40, 50, 60, 70, 80, 90,
  };
  ft_err e;
  if(argc < 2 || ft_png_write_rgb8(argv[1], rgb, 3, 2, &e)) {
    fprintf(
        stderr, "png_dump: %s\n", argc < 2 ? "usage: png_dump out.png" : e.msg
    );
    return 1;
  }
  return 0;
}

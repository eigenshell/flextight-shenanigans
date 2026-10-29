/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ft_write_jpeg: the encoder's buffer into a file (Sancho's side:
 * stdio). */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "ft_jpeg.h"

int32_t ft_write_jpeg(const char* path, const uint8_t* pixels, uint32_t width,
                      uint32_t height, int32_t quality, double ppi,
                      const uint8_t* icc_profile, size_t icc_profile_length,
                      const char* comment, ft_err* error) {
  uint8_t* jpeg;
  size_t jpeg_length;
  if(ft_encode_jpeg(pixels, width, height, quality, ppi, icc_profile,
                    icc_profile_length, comment, &jpeg, &jpeg_length,
                    error) != 0) {
    return -1;
  }
  FILE* file = fopen(path, "wb");
  if(file == NULL) {
    free(jpeg);
    return ft_fail(error, "jpeg: cannot write %s", path);
  }
  bool is_written = fwrite(jpeg, 1, jpeg_length, file) == jpeg_length;
  /* close in any case, and notice a failing close */
  if(fclose(file) != 0) is_written = false;
  free(jpeg);
  if(is_written == false) {
    /* do not leave half a JPEG behind */
    remove(path);
    return ft_fail(error, "jpeg: write error on %s", path);
  }
  return 0;
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Writes a synthetic share window scan for tests/test_ftdev.py:
 * <base>.raw, .raw.cal and .raw.txt, as ftpoc scan --job does for a share
 * job. One full frame, 1500 ppi bin 1, at holder 100..136 mm; the preview
 * put it at 101 (it crept 1 mm); window 99..139 mm; a patch of bare film
 * base inside the frame (renders as a neutral dark grey). */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "test/synth.h"

static int dump(const char* path, const void* p, size_t n) {
  FILE* f = fopen(path, "wb");
  int ok = f && fwrite(p, 1, n, f) == n;
  if (f && fclose(f)) ok = 0;
  return ok ? 0 : -1;
}

int main(int argc, char** argv) {
  synth_win w;
  synth_win_scan s;
  char path[512];
  FILE* f;
  int rc = 0;

  if (argc < 2) {
    fprintf(stderr, "usage: window_dump <base>\n");
    return 2;
  }
  memset(&w, 0, sizeof w);
  w.ppi = 1500;
  w.bin = 1;
  w.y0_mm = 99;
  w.len_mm = 40;
  w.x0_mm = 1.0;
  w.w_mm = 31;
  w.nfr = 1;
  w.fy0[0] = 100;
  w.flen[0] = 36;
  w.npatch = 1;
  w.patch[0].y0 = 105;
  w.patch[0].y1 = 110;
  w.patch[0].x0 = 10;
  w.patch[0].x1 = 14;
  synth_window(&w, &s);
  if (!s.raw) {
    fprintf(stderr, "window_dump: out of memory\n");
    return 1;
  }
  snprintf(path, sizeof path, "%s.raw", argv[1]);
  rc |= dump(path, s.raw, (size_t)s.lines * s.px * 6);
  snprintf(path, sizeof path, "%s.raw.cal", argv[1]);
  rc |= dump(path, s.cal, (size_t)s.cal_lines * s.px * 6);
  snprintf(path, sizeof path, "%s.raw.txt", argv[1]);
  f = fopen(path, "w");
  if (f) {
    fprintf(f,
            "preset=crop\nppi=1500\npx=%d\nx=%ld\nlines=%d\ny=%ld\n"
            "bin=1\nkept=%d\nfilm=negative\nholder=24x217\n"
            "share=2000\nhf_half_res=0\n"
            "film_base=%.4f %.4f %.4f\n"
            "share_frame=1 full 101.000 36.000 4.100 24.000\n",
            s.px, s.x, s.lines, s.y, s.lines, -log10(s.base[0]),
            -log10(s.base[1]), -log10(s.base[2]));
    rc |= fclose(f) ? -1 : 0;
  } else {
    rc = -1;
  }
  synth_window_free(&s);
  if (rc) fprintf(stderr, "window_dump: cannot write %s*\n", argv[1]);
  return rc ? 1 : 0;
}

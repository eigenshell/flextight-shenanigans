/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Streaming develop against the whole-image ft_develop (the oracle):
 * bit-identical transmittance at every bin, with Faul Pixs and user
 * defects, on noisy synthetic scans. */
#include "ft_develop.h"
#include "ft_stream.h"
#include "test/ftest.h"
#include <stdlib.h>
#include <string.h>

enum { P = 37, L = 61, CL = 250 };

static unsigned rng = 12345;
static unsigned rnd(void) {
  rng = rng * 1103515245u + 12345u;
  return rng >> 8;
}

int main(void) {
  static unsigned short raw[L * P * 3], cal[CL * P * 3];
  static const int bins[] = {1, 2, 3, 4, 6, 12};
  static float out[P * 3];
  int bi, l, i, c;
  ft_err e;

  /* per-pixel dark and white with spread, noisy scan levels */
  for(l = 0; l < CL; l++)
    for(i = 0; i < P * 3; i++)
      cal[l * P * 3 + i] =
          (unsigned short)(l < 200 ? 280 + (i * 7) % 40 + rnd() % 5
                                   : 3000 + (i * 131) % 3000 + rnd() % 9);
  for(i = 0; i < L * P * 3; i++)
    raw[i] = (unsigned short)(250 + rnd() % 6000); /* incl. clipping */

  for(bi = 0; bi < 6; bi++) {
    int bin = bins[bi], k = 12 / bin, x = 9, n = 0;
    char faul[200];
    ft_develop_opts o;
    ft_defects d;
    ft_image im;
    ft_stream *s;
    int mismatch = 0;

    /* Faul Pixs: output px 5 in R, px 6..7 in B (a run); a user defect
     * on output px 20 in G. CCD pixel of output px j: (x + j) * bin
     * + 144 */
    snprintf(
        faul, sizeof faul,
        "Faul Pixs\n100\t0\n%d\t+999\n100\t0\n100\t0\n%d\t+999\n"
        "%d\t+999\n",
        (x + 5) * bin + 144, (x + 6) * bin + 144, (x + 7) * bin + 144
    );
    d.n = 1;
    d.ccd[0] = (x + 20) * bin + 144;
    d.ch[0] = 2;
    memset(&o, 0, sizeof o);
    o.x = x;
    o.bin = bin;
    o.defects = &d;

    ft_image_init(&im);
    CHECK(ft_develop(raw, L, P, cal, CL, faul, strlen(faul), &o, &im, &e) == 0);
    CHECK(im.w == L - 2 * k && im.h == P);

    s = ft_stream_open(P, cal, CL, faul, strlen(faul), &o, &e);
    CHECK(s != NULL);
    CHECK(ft_stream_k(s) == k);
    for(l = 0; l < L; l++) {
      int rc = ft_stream_push(s, raw + (size_t)l * P * 3, out, &e);
      CHECK(rc == (l >= 2 * k));
      if(rc != 1)
        continue;
      /* ft_develop's image is transposed: row = px, column = line */
      for(c = 0; c < P; c++)
        for(i = 0; i < 3; i++)
          mismatch += out[c * 3 + i] != im.rgb[((size_t)c * im.w + n) * 3 + i];
      n++;
    }
    CHECK(n == im.w);
    CHECK(mismatch == 0);
    ft_stream_close(s);
    ft_image_free(&im);
  }

  /* refusals */
  {
    ft_develop_opts o;
    ft_defects many;
    memset(&o, 0, sizeof o);
    o.bin = 0;
    CHECK(ft_stream_open(P, cal, CL, NULL, 0, &o, &e) == NULL);
    o.bin = 3;
    CHECK(ft_stream_open(P, cal, 5, NULL, 0, &o, &e) == NULL); /* cal */
    many.n = FT_DEFECT_MAX;
    for(i = 0; i < FT_DEFECT_MAX; i++) {
      many.ccd[i] = 1000 + i;
      many.ch[i] = 1;
    }
    o.defects = &many; /* 64 user + 1 Faul in R: too many */
    CHECK(
        ft_stream_open(
            P, cal, CL, "Faul Pixs\n100\t0\n999\t+999\n", 25, &o, &e
        ) == NULL
    );
  }
  return TEST_DONE();
}

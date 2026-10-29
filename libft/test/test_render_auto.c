/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdint.h>
#include <string.h>

#include "ft_render.h"
#include "ft_render_auto.h"
#include "test/ftest.h"

static ft_render cr;

static uint32_t rng = 99;
static double urand(void) {
  rng = rng * 1664525u + 1013904223u;
  return ((rng >> 8) + 0.5) / 16777216.0;
}
static double gauss(void) {
  return sqrt(-2 * log(urand())) * cos(6.283185307179586 * urand());
}

/* one pixel of holder metal as the scanner reads it: T ~ 0.001 +- 0.0005 */
static float metal(void) {
  double t = 0.001 + 0.0005 * gauss();
  return (float)(t > 0 ? t : 0);
}

/* linear Rec.2020 -> CIELAB (D65) */
static void lab(const double rgb[3], double out[3]) {
  static const double M[9] = {0.6369580, 0.1446169, 0.1688810,
                              0.2627002, 0.6779981, 0.0593017,
                              0.0000000, 0.0280727, 1.0609851};
  static const double W[3] = {0.95047, 1.0, 1.08883};
  double f[3];
  int i;
  for(i = 0; i < 3; i++) {
    double v =
      (M[3 * i] * rgb[0] + M[3 * i + 1] * rgb[1] + M[3 * i + 2] * rgb[2]) /
      W[i];
    f[i] = v > 216.0 / 24389 ? cbrt(v) : (24389.0 / 27 * v + 16) / 116;
  }
  out[0] = 116 * f[1] - 16;
  out[1] = 500 * (f[0] - f[1]);
  out[2] = 200 * (f[1] - f[2]);
}

enum { W = 960, H = 200, STEPS = 12 };
static const float BASE[3] = {0.25f, 0.6f, 0.9f};  /* orange mask */
static const double GAMMA[3] = {0.55, 0.62, 0.70}; /* per dye layer */

/* A colour negative of a scene: a grey ramp (12 steps, exposure 0.01..1
 * across 70 % of the width) and colour patches (right 30 %), through
 * per-channel characteristic curves and noise like the scanner's (3 DN
 * dark noise on a ~6000 DN open beam, plus shot noise). Patches are 56 px
 * wide or more, like real highlight areas at scan resolution. */
static void negative_scaled(ft_image* im, double scale) {
  int x, y, c;
  for(y = 0; y < H; y++)
    for(x = 0; x < W; x++) {
      double ex[3];
      if(x < W * 7 / 10) {
        int s = x * STEPS / (W * 7 / 10);
        ex[0] = ex[1] = ex[2] = scale * pow(10, -2 + 2.0 * s / (STEPS - 1));
      } else {
        int p = y * 3 / H;
        ex[0] = scale * (p == 0 ? 0.6 : 0.05);
        ex[1] = scale * (p == 1 ? 0.6 : 0.05);
        ex[2] = scale * (p == 2 ? 0.6 : 0.05);
      }
      for(c = 0; c < 3; c++) {
        double d = BASE[c] + GAMMA[c] * log10(ex[c] / 0.002);
        double t = pow(10, -d), sig = sqrt(0.0005 * 0.0005 + t / 6000);
        im->pixels[3 * (y * W + x) + c] = (float)(t + sig * gauss());
      }
    }
}

static void negative(ft_image* im) { negative_scaled(im, 1.0); }

/* mean output of grey step s */
static void step_mean(const ft_image* im, int s, double out[3]) {
  int x0 = s * (W * 7 / 10) / STEPS + 3;
  int x1 = (s + 1) * (W * 7 / 10) / STEPS - 3;
  int x, y, c, n = 0;
  out[0] = out[1] = out[2] = 0;
  for(y = 10; y < H - 10; y++)
    for(x = x0; x < x1; x++, n++)
      for(c = 0; c < 3; c++) out[c] += im->pixels[3 * (y * W + x) + c];
  for(c = 0; c < 3; c++) out[c] /= n;
}

int main(void) {
  const ft_image* one[1];
  ft_image im;
  ft_recipe r;
  ft_err e;
  int s, c;

  ft_init_image(&im);
  CHECK(ft_allocate_image(&im, W, H, &e) == 0);
  negative(&im);
  one[0] = &im;

  /* auto on a known base: ranges follow the dye gammas, greys neutral */
  ft_init_recipe(&r, FT_RENDER_NEGATIVE);
  ft_set_recipe_base(&r, BASE);
  r.tint[0] = 7; /* auto resets tint */
  CHECK(ft_estimate_recipe(one, 1, &r, &e) == 0);
  CHECK(r.tint[0] == 0);
  for(c = 0; c < 3; c++) /* 2.7 decades */
    CHECK_NEAR(r.range_density[c], GAMMA[c] * log10(1 / 0.002), 0.06);
  CHECK(r.exposure > 0 && r.exposure < 6);
  /* default paper (0..2 D, grade 1.5): the highlight lands 0.03 D below
   * paper white, pinned independently of the implementation */
  CHECK_NEAR(r.exposure,
             -(log(0.015 / 0.985) / 6 + 0.5) * r.range_density[1] / log10(2),
             1e-5);
  /* the closed form on its own gives the same exposure, bit for bit,
   * and follows the paper's grade */
  CHECK(ft_estimate_auto_exposure(&r) == r.exposure);
  {
    ft_recipe g = r;
    g.grade = 3;
    CHECK_NEAR(ft_estimate_auto_exposure(&g),
               -(log(0.015 / 0.985) / 12 + 0.5) * r.range_density[1] / log10(2),
               1e-5);
    g.grade = 0;
    CHECK(ft_estimate_auto_exposure(&g) == 0);
  }
  CHECK(ft_compile_render(&r, &cr, &e) == 0);
  ft_render_image(&cr, &im);
  {
    double prev = -1;
    for(s = 0; s < STEPS; s++) {
      double m[3], l[3];
      step_mean(&im, s, m);
      lab(m, l);
      CHECK(fabs(l[1]) < 1 && fabs(l[2]) < 1); /* neutral */
      CHECK(l[0] > prev);                      /* brighter with exposure */
      prev = l[0];
    }
    CHECK(prev > 90); /* white near paper white */
  }

  /* base from the frame itself: a strip of unexposed film on the left */
  negative(&im);
  {
    float d[3];
    int x, y;
    for(y = 0; y < H; y++)
      for(x = 0; x < W / 50 + 8; x++)
        for(c = 0; c < 3; c++)
          im.pixels[3 * (y * W + x) + c] = (float)pow(10, -BASE[c]);
    CHECK(ft_estimate_base_from_frames(one, 1, FT_RENDER_NEGATIVE, d, &e) == 0);
    for(c = 0; c < 3; c++) CHECK_NEAR(d[c], BASE[c], 0.002);
  }

  /* slide: the densest film is the black; auto leaves exposure at 0 */
  {
    int i;
    float d[3];
    for(i = 0; i < W * H * 3; i++) im.pixels[i] = 0.3f;
    for(i = 0; i < W * 20 * 3; i++) /* 20 black rows */
      im.pixels[W * 3 * 90 + i] = 0.001f;
    CHECK(ft_estimate_base_from_frames(one, 1, FT_RENDER_SLIDE, d, &e) == 0);
    CHECK_NEAR(d[0], 3.0, 0.002);
    /* opaque metal (T at the floor) must not become the slide black */
    for(i = 0; i < H; i++)
      memset(im.pixels + 3 * (i * W + 21), 0, 40 * 3 * sizeof(float));
    CHECK(ft_estimate_base_from_frames(one, 1, FT_RENDER_SLIDE, d, &e) == 0);
    CHECK_NEAR(d[0], 3.0, 0.002);
    ft_init_recipe(&r, FT_RENDER_SLIDE);
    r.exposure = 2;
    CHECK(ft_estimate_recipe(one, 1, &r, &e) == 0 && r.exposure == 0);
  }

  /* b&w: one luminance range in all three slots */
  negative(&im);
  ft_init_recipe(&r, FT_RENDER_BW);
  ft_set_recipe_base(&r, BASE);
  CHECK(ft_estimate_recipe(one, 1, &r, &e) == 0);
  CHECK(r.range_density[0] == r.range_density[1] &&
        r.range_density[1] == r.range_density[2]);
  { /* the white step's luminance density above the folded base */
    double y = 0;
    for(c = 0; c < 3; c++)
      y += r.bw_weights[c] * pow(10, -(BASE[c] + GAMMA[c] * log10(1 / 0.002)));
    CHECK_NEAR(r.range_density[1], -log10(y) - r.base_density[0], 0.06);
  }

  /* two frames pool: the same frame twice gives the same answer */
  {
    const ft_image* two[2];
    ft_recipe a = r;
    two[0] = two[1] = &im;
    CHECK(ft_estimate_recipe(two, 2, &a, &e) == 0);
    CHECK_NEAR(a.range_density[1], r.range_density[1], 0.005);
  }

  /* noisy holder metal from x = 102 on, off the 4-px block grid (x0 = 4,
   * so the block at 100 is half metal, half film): that block is half as
   * transmissive, 0.3 D denser than the uniform film (1.0 D over base),
   * 2 % of the blocks. It must not be taken for the highlight. */
  {
    ft_image mt;
    const ft_image* mp[1];
    ft_recipe t;
    int x, y;
    ft_init_image(&mt);
    CHECK(ft_allocate_image(&mt, 200, 100, &e) == 0);
    for(y = 0; y < 100; y++)
      for(x = 0; x < 200; x++)
        for(c = 0; c < 3; c++)
          mt.pixels[3 * (y * 200 + x) + c] =
            x >= 102 ? metal() : (float)pow(10, -(BASE[c] + 1.0));
    mp[0] = &mt;
    ft_init_recipe(&t, FT_RENDER_NEGATIVE);
    ft_set_recipe_base(&t, BASE);
    CHECK(ft_estimate_recipe(mp, 1, &t, &e) == 0);
    for(c = 0; c < 3; c++) CHECK_NEAR(t.range_density[c], 1.0, 0.01);
    ft_free_image(&mt);
  }

  /* a thin highlight with no usable right neighbour (1.0 D lighter): the
   * filtered histogram is empty, auto falls back to the unfiltered one */
  {
    /* the stripe sits on the 4-px block grid (x0 = 4, 100 = 4 + 24 * 4) */
    ft_image st;
    const ft_image* sp[1];
    ft_recipe t;
    int x, y;
    ft_init_image(&st);
    CHECK(ft_allocate_image(&st, 200, 100, &e) == 0);
    for(y = 0; y < 100; y++)
      for(x = 0; x < 200; x++)
        for(c = 0; c < 3; c++)
          st.pixels[3 * (y * 200 + x) + c] =
            (float)pow(10, -(BASE[c] + (x >= 100 && x < 104 ? 2.0 : 1.0)));
    sp[0] = &st;
    ft_init_recipe(&t, FT_RENDER_NEGATIVE);
    ft_set_recipe_base(&t, BASE);
    CHECK(ft_estimate_recipe(sp, 1, &t, &e) == 0);
    for(c = 0; c < 3; c++) CHECK_NEAR(t.range_density[c], 2.0, 0.01);
    ft_free_image(&st);
  }

  /* different frames pool: B is a dim frame (exposures x 0.1, so its
   * film is thinner). Its own highlight is ~0.55 D (ch 0) below A's, but
   * the brightest 0.5 % of the pooled blocks all come from A, so the
   * pooled range equals A's alone. A frame without pixels adds nothing. */
  {
    ft_image dim, nul;
    const ft_image *pair[2], *withnul[2], *dp[1];
    ft_recipe ra, rb, rp, rn;
    negative(&im);
    ft_init_image(&dim);
    CHECK(ft_allocate_image(&dim, W, H, &e) == 0);
    negative_scaled(&dim, 0.1);
    ft_init_image(&nul);
    nul.width = W;
    nul.height = H; /* rgb stays NULL */
    ft_init_recipe(&ra, FT_RENDER_NEGATIVE);
    ft_set_recipe_base(&ra, BASE);
    rb = rp = rn = ra;
    pair[0] = &im;
    pair[1] = &dim;
    withnul[0] = &im;
    withnul[1] = &nul;
    dp[0] = &dim;
    CHECK(ft_estimate_recipe(dp, 1, &rb, &e) == 0);
    CHECK(ft_estimate_recipe(one, 1, &ra, &e) == 0);
    CHECK(ft_estimate_recipe(pair, 2, &rp, &e) == 0);
    CHECK(ft_estimate_recipe(withnul, 2, &rn, &e) == 0);
    for(c = 0; c < 3; c++) {
      CHECK(rb.range_density[c] < ra.range_density[c] - 0.3);
      CHECK_NEAR(rp.range_density[c], ra.range_density[c], 0.02);
      CHECK(rn.range_density[c] == ra.range_density[c]);
    }
    CHECK(rn.exposure == ra.exposure);
    ft_free_image(&dim);
  }

  /* a noisy holder band (6 % of the width, T ~ 0.001) must not drive
   * the range: the highlight is still the white step */
  negative(&im);
  {
    int x, y;
    for(y = 0; y < H; y++)
      for(x = 0; x < 62; x++)
        for(c = 0; c < 3; c++) im.pixels[3 * (y * W + x) + c] = metal();
    ft_init_recipe(&r, FT_RENDER_NEGATIVE);
    ft_set_recipe_base(&r, BASE);
    CHECK(ft_estimate_recipe(one, 1, &r, &e) == 0);
    for(c = 0; c < 3; c++)
      CHECK_NEAR(r.range_density[c], GAMMA[c] * log10(1 / 0.002), 0.06);
    CHECK(ft_compile_render(&r, &cr, &e) == 0);
  }

  /* a clear band (T = 1.02: a sprocket hole, the open beam) is not the
   * thinnest film. The band starts and ends off the 4-px block grid
   * (x0 = 19). */
  negative(&im);
  {
    float d[3];
    int x, y;
    for(y = 0; y < H; y++) {
      for(x = 0; x < W / 50 + 8; x++)
        for(c = 0; c < 3; c++)
          im.pixels[3 * (y * W + x) + c] = (float)pow(10, -BASE[c]);
      for(x = 805; x < 862; x++)
        for(c = 0; c < 3; c++) im.pixels[3 * (y * W + x) + c] = 1.02f;
    }
    CHECK(ft_estimate_base_from_frames(one, 1, FT_RENDER_NEGATIVE, d, &e) == 0);
    for(c = 0; c < 3; c++) {
      CHECK_NEAR(d[c], BASE[c], 0.002);
      CHECK(d[c] >= 0);
    }
  }

  /* thin paper (black - white < 0.03 D) and grade 0: a finite exposure */
  negative(&im);
  ft_init_recipe(&r, FT_RENDER_NEGATIVE);
  ft_set_recipe_base(&r, BASE);
  r.paper_black = 0.02f;
  r.paper_white = 0;
  CHECK(ft_estimate_recipe(one, 1, &r, &e) == 0);
  CHECK(isfinite(r.exposure) && r.exposure >= -6 && r.exposure <= 6);
  ft_init_recipe(&r, FT_RENDER_NEGATIVE);
  ft_set_recipe_base(&r, BASE);
  r.grade = 0;
  CHECK(ft_estimate_recipe(one, 1, &r, &e) == 0 && r.exposure == 0);

  /* b&w with a dense highlight: grey film at 1.0 D with a 2.55 D area
   * (12 % of the frame), scanner noise (dark 0.0005 + shot t / 6000).
   * Film that dense reads T ~ 0.0028; it must not be taken for metal. */
  {
    int x, y;
    for(y = 0; y < H; y++)
      for(x = 0; x < W; x++) {
        double t = x >= 400 && x < 515 ? pow(10, -2.55) : 0.1;
        double sig = sqrt(0.0005 * 0.0005 + t / 6000);
        for(c = 0; c < 3; c++)
          im.pixels[3 * (y * W + x) + c] = (float)(t + sig * gauss());
      }
    ft_init_recipe(&r, FT_RENDER_BW);
    r.base_density[0] = r.base_density[1] = r.base_density[2] = 0.2f;
    CHECK(ft_estimate_recipe(one, 1, &r, &e) == 0);
    CHECK_NEAR(r.range_density[1], 2.55 - 0.2, 0.06);
  }

  /* slide with a noisy 3.5 D black (rows 92..107, the rest T = 0.3) */
  {
    float d[3];
    int x, y;
    for(y = 0; y < H; y++)
      for(x = 0; x < W; x++)
        for(c = 0; c < 3; c++)
          im.pixels[3 * (y * W + x) + c] =
            y >= 92 && y < 108 ? (float)(pow(10, -3.5) + 4e-5 * gauss()) : 0.3f;
    CHECK(ft_estimate_base_from_frames(one, 1, FT_RENDER_SLIDE, d, &e) == 0);
    for(c = 0; c < 3; c++) CHECK_NEAR(d[c], 3.5, 0.05);
  }

  /* a band of +inf red (a corrupt readout) does not move the red base */
  negative(&im);
  {
    float d[3];
    int x, y;
    for(y = 0; y < H; y++) {
      for(x = 0; x < W / 50 + 8; x++)
        for(c = 0; c < 3; c++)
          im.pixels[3 * (y * W + x) + c] = (float)pow(10, -BASE[c]);
      for(x = 100; x < 160; x++) im.pixels[3 * (y * W + x)] = INFINITY;
    }
    CHECK(ft_estimate_base_from_frames(one, 1, FT_RENDER_NEGATIVE, d, &e) == 0);
    for(c = 0; c < 3; c++) CHECK_NEAR(d[c], BASE[c], 0.002);
  }

  /* no pixels: an error, not a crash */
  {
    ft_image empty;
    const ft_image* z[1];
    float d[3];
    ft_init_image(&empty);
    z[0] = &empty;
    CHECK(ft_estimate_base_from_frames(z, 1, FT_RENDER_NEGATIVE, d, &e) == -1);
    CHECK(ft_estimate_recipe(z, 1, &r, &e) == -1);
  }
  ft_free_image(&im);
  return TEST_DONE();
}

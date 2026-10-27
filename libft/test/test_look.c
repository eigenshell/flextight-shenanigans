/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdint.h>
#include <string.h>

#include "ft_look.h"
#include "ft_render_auto.h"
#include "test/ftest.h"

static ft_edit ed, ed2, look; /* ~100 KB each: not on the stack */
static char buf[65536], buf2[65536];

/* parse text as kind; the error message, or "" on success */
static const char* err_of(const char* text, ft_look_kind k) {
  static ft_err e;
  if(ft_edit_parse(text, strlen(text), k, &ed2, &e) == 0) return "";
  return e.msg;
}

static uint32_t rng = 7;
static double urand(void) {
  rng = rng * 1664525u + 1013904223u;
  return ((rng >> 8) + 0.5) / 16777216.0;
}
static double gauss(void) {
  return sqrt(-2 * log(urand())) * cos(6.283185307179586 * urand());
}

enum { W = 480, H = 120, STEPS = 12 };
static const float BASE[3] = {0.25f, 0.6f, 0.9f};
static const double GAMMA[3] = {0.55, 0.62, 0.70};

/* a colour negative: a 12-step grey ramp and three colour patches, with
 * scanner-like noise (as test_render_auto.c) */
static void negative(ft_image* im) {
  int x, y, c;
  for(y = 0; y < H; y++)
    for(x = 0; x < W; x++) {
      double ex[3];
      if(x < W * 7 / 10) {
        int s = x * STEPS / (W * 7 / 10);
        ex[0] = ex[1] = ex[2] = pow(10, -2 + 2.0 * s / (STEPS - 1));
      } else {
        int p = y * 3 / H;
        ex[0] = p == 0 ? 0.6 : 0.05;
        ex[1] = p == 1 ? 0.6 : 0.05;
        ex[2] = p == 2 ? 0.6 : 0.05;
      }
      for(c = 0; c < 3; c++) {
        double d = BASE[c] + GAMMA[c] * log10(ex[c] / 0.002);
        double t = pow(10, -d), sig = sqrt(0.0005 * 0.0005 + t / 6000);
        im->rgb[3 * (y * W + x) + c] = (float)(t + sig * gauss());
      }
    }
}

static void text_tests(void) {
  ft_err e;
  size_t n;
  const char* look_in =
    "# portra, learned\n"
    "[stock]\n"
    "stock_version = 1\n"
    "range_ratio = 1.08 1 0.93   # pooled\n"
    "n_frames = 36\n"
    "[taste]\n"
    "grade = 0.9\n"
    "curve =   soft\n"
    "sharpen = .4\n"
    "exposure = +0.3\n"
    "tint = 2 0 -3\n";
  const char* look_out =
    "[stock]\n"
    "stock_version = 1\n"
    "range_ratio = 1.08 1 0.93\n"
    "n_frames = 36\n"
    "\n"
    "[taste]\n"
    "exposure = 0.3\n"
    "tint = 2 0 -3\n"
    "grade = 0.9\n"
    "curve = soft\n"
    "sharpen = 0.4\n";
  const char* edit_in =
    "from = portra400\n"
    "[taste]\n"
    "grade = 1.5\n"
    "nr_grain = 0.333333333\n"
    "[frame 7]\n"
    "grade = 1.2\n"
    "curve = 0,0  0.5,0.6 1,1\n"
    "[frame 3]\r\n"
    "exposure = -0.25\n";
  const char* edit_out =
    "from = portra400\n"
    "\n"
    "[taste]\n"
    "grade = 1.5\n"
    "nr_grain = 0.333333\n"
    "\n"
    "[frame 7]\n"
    "grade = 1.2\n"
    "curve = 0,0 0.5,0.6 1,1\n"
    "\n"
    "[frame 3]\n"
    "exposure = -0.25\n";

  /* a look: parsed, written in table order with 6 decimals at most */
  CHECK(ft_edit_parse(look_in, strlen(look_in), FT_LOOK_FILE, &look, &e) == 0);
  CHECK(look.stock.present &&
        look.stock.version == 1 &&
        look.stock.has_ratio &&
        !look.stock.has_bw);
  CHECK_NEAR(look.stock.range_ratio[0], 1.08, 0);
  CHECK(look.stock.n_frames == 36);
  CHECK_NEAR(look.taste.exposure, 0.3, 0);
  CHECK(strcmp(look.taste.curve[0], "soft") == 0);
  n = ft_edit_write(&look, FT_LOOK_FILE, buf, sizeof buf);
  CHECK(n == strlen(look_out) && strcmp(buf, look_out) == 0);
  /* byte-stable: write(parse(write)) == write */
  CHECK(ft_edit_parse(buf, n, FT_LOOK_FILE, &ed2, &e) == 0);
  CHECK(ft_edit_write(&ed2, FT_LOOK_FILE, buf2, sizeof buf2) == n &&
        strcmp(buf, buf2) == 0);
  /* a look is a valid edit (copy it to start a roll) */
  CHECK(ft_edit_parse(buf, n, FT_EDIT_FILE, &ed2, &e) == 0);
  CHECK(ft_edit_write(&ed2, FT_EDIT_FILE, buf2, sizeof buf2) == n &&
        strcmp(buf, buf2) == 0);

  /* an edit: frames in file order, curves with single spaces, CRLF ok */
  CHECK(ft_edit_parse(edit_in, strlen(edit_in), FT_EDIT_FILE, &ed, &e) == 0);
  CHECK(strcmp(ed.from, "portra400") == 0 && !ed.stock.present);
  CHECK(ed.nframes == 2 && ed.f[0].index == 7 && ed.f[1].index == 3);
  n = ft_edit_write(&ed, FT_EDIT_FILE, buf, sizeof buf);
  CHECK(n == strlen(edit_out) && strcmp(buf, edit_out) == 0);
  CHECK(ft_edit_parse(buf, n, FT_EDIT_FILE, &ed2, &e) == 0);
  CHECK(ft_edit_write(&ed2, FT_EDIT_FILE, buf2, sizeof buf2) == n &&
        strcmp(buf, buf2) == 0);
  /* as a look: no from, no frames */
  CHECK(ft_edit_write(&ed, FT_LOOK_FILE, buf2, sizeof buf2) ==
        strlen("[taste]\ngrade = 1.5\nnr_grain = 0.333333\n"));
  /* a short buffer: truncated, NUL-terminated, the full length back */
  CHECK(ft_edit_write(&ed, FT_EDIT_FILE, buf2, 10) == strlen(edit_out));
  CHECK(strcmp(buf2, "from = po") == 0);
  /* numbers: no "-0", 6 decimals */
  CHECK(ft_edit_parse("[taste]\nexposure = -0.0000001\ntint = 0.0000005 "
                      "-1 1e-6\n",
                      999, FT_LOOK_FILE, &ed2, &e) == 0);
  ft_edit_write(&ed2, FT_LOOK_FILE, buf2, sizeof buf2);
  CHECK(strcmp(buf2,
               "[taste]\nexposure = 0\ntint = 0.000001 -1 "
               "0.000001\n") == 0);
  /* an empty text is an empty look */
  CHECK(ft_edit_parse("", 0, FT_LOOK_FILE, &ed2, &e) == 0);
  CHECK(ft_edit_write(&ed2, FT_LOOK_FILE, buf2, sizeof buf2) == 8 &&
        strcmp(buf2, "[taste]\n") == 0);
}

static void error_tests(void) {
  CHECK(strcmp(err_of("[taste]\n\ngrade = 9\n", FT_LOOK_FILE),
               "line 3: grade: 9 is out of range (0.2..5)") == 0);
  CHECK(strcmp(err_of("[taste]\ngamma = 2\n", FT_LOOK_FILE),
               "line 2: gamma: unknown key") == 0);
  CHECK(strcmp(err_of("grade = 1\n", FT_EDIT_FILE),
               "line 1: grade: outside a section ([stock], [taste], "
               "[frame N])") == 0);
  CHECK(strcmp(err_of("from = x\n", FT_LOOK_FILE),
               "line 1: from: outside a section ([stock], [taste])") == 0);
  CHECK(strcmp(err_of("[frame 2]\n", FT_LOOK_FILE),
               "line 1: [frame N] belongs in a .edit, not a .look") == 0);
  CHECK(strcmp(err_of("[stock]\nn_frames = 3\n[taste]\n", FT_LOOK_FILE),
               "line 1: [stock] has no stock_version") == 0);
  CHECK(strcmp(err_of("[taste]\n[stock]\n", FT_LOOK_FILE),
               "line 2: [stock] has no stock_version") == 0);
  CHECK(strcmp(err_of("[stock]\nstock_version = 2\n", FT_LOOK_FILE),
               "line 2: stock_version: 2 needs a newer libft (this one "
               "reads 1)") == 0);
  CHECK(strcmp(err_of("[stock]\nstock_version = 1\nrange_ratio = 1.1 0.9 "
                      "1\n",
                      FT_LOOK_FILE),
               "line 3: range_ratio: green must be 1 (the ratios are "
               "relative to green)") == 0);
  CHECK(strcmp(err_of("[stock]\nstock_version = 1\nrange_ratio = 9 1 "
                      "1\n",
                      FT_LOOK_FILE),
               "line 3: range_ratio: 9 is out of range (0.2..5)") == 0);
  CHECK(strcmp(err_of("[stock]\nstock_version = 1\nbw_weights = 0.3 0.3 "
                      "0.3\n",
                      FT_LOOK_FILE),
               "line 3: bw_weights: sum to 0.9, not 1") == 0);
  CHECK(strcmp(err_of("[stock]\nstock_version = 1\ngrade = 1\n", FT_LOOK_FILE),
               "line 3: grade: a taste key: put it in [taste]") == 0);
  CHECK(strcmp(err_of("[frame 2]\nn_frames = 3\n", FT_EDIT_FILE),
               "line 2: n_frames: a [stock] key (roll-wide): put it in "
               "[stock]") == 0);
  CHECK(strcmp(err_of("[taste]\ngrade = 1\ngrade = 2\n", FT_LOOK_FILE),
               "line 3: grade: set twice") == 0);
  CHECK(strcmp(err_of("[stock]\nstock_version = 1\nstock_version = 1\n",
                      FT_LOOK_FILE),
               "line 3: stock_version: set twice") == 0);
  CHECK(strcmp(err_of("[frame 2]\n[taste]\n[frame 2]\n", FT_EDIT_FILE),
               "line 3: [frame 2] twice") == 0);
  CHECK(strcmp(err_of("[taste]\n[taste]\n", FT_LOOK_FILE),
               "line 2: [taste] twice") == 0);
  CHECK(strcmp(err_of("[frame two]\n", FT_EDIT_FILE),
               "line 1: '[frame two]': the frame is a number 0..9999") == 0);
  CHECK(strcmp(err_of("[looks]\n", FT_LOOK_FILE),
               "line 1: unknown section [looks]") == 0);
  CHECK(strcmp(err_of("[fr]\n", FT_EDIT_FILE),
               "line 1: unknown section [fr]") == 0);
  CHECK(strcmp(err_of("[frame]\n", FT_EDIT_FILE),
               "line 1: unknown section [frame]") == 0);
  CHECK(strcmp(err_of("[taste\n", FT_LOOK_FILE),
               "line 1: '[taste' is not a [section]") == 0);
  CHECK(strcmp(err_of("[taste]\ngrade 2\n", FT_LOOK_FILE),
               "line 2: expected key = value") == 0);
  CHECK(strcmp(err_of("[taste]\ntint = 1 2\n", FT_LOOK_FILE),
               "line 2: tint: '1 2' is not three numbers (r g b)") == 0);
  CHECK(strcmp(err_of("[taste]\ntint = 1 2 60\n", FT_LOOK_FILE),
               "line 2: tint: 60 is out of range (-50..50)") == 0);
  CHECK(strcmp(err_of("[taste]\nexposure = 1e400\n", FT_LOOK_FILE),
               "line 2: exposure: '1e400' is not a number") == 0);
  CHECK(strcmp(err_of("[taste]\nsharpen = nan\n", FT_LOOK_FILE),
               "line 2: sharpen: 'nan' is not a number") == 0);
  {
    const char* want =
      "line 2: curve: not a preset (linear soft "
      "medium strong fade) and not points: ";
    CHECK(strncmp(err_of("[taste]\ncurve = wiggly\n", FT_LOOK_FILE), want,
                  strlen(want)) == 0);
  }
  CHECK(strcmp(err_of("from = a\nfrom = b\n", FT_EDIT_FILE),
               "line 2: from: set twice") == 0);
  CHECK(strcmp(err_of("from =\n", FT_EDIT_FILE),
               "line 1: from: a name of 1..63 characters") == 0);
  /* every frame section counts against FT_MAX_FRAMES */
  {
    static char many[8192];
    size_t at = 0;
    int i;
    for(i = 0; i <= FT_MAX_FRAMES; i++)
      at += (size_t)sprintf(many + at, "[frame %d]\n", i);
    CHECK(strcmp(err_of(many, FT_EDIT_FILE), "line 65: more than 64 frames") ==
          0);
  }
  /* comments: '#' always starts one, even inside a value */
  CHECK(strcmp(err_of("[taste]  # the look\ncurve = soft#x\n", FT_LOOK_FILE),
               "") == 0);
  CHECK(strcmp(ed2.taste.curve[0], "soft") == 0);
}

static void op_tests(void) {
  ft_err e;
  ft_taste t;
  unsigned ff;
  const char* edit_in =
    "from = portra400\n[taste]\ngrade = 1.5\nexposure = 0.2\n"
    "[frame 7]\ngrade = 1.2\ncurve = strong\n[frame 3]\n"
    "exposure = -0.25\n";
  CHECK(ft_edit_parse(edit_in, strlen(edit_in), FT_EDIT_FILE, &ed, &e) == 0);

  /* set: a new frame; a bad value creates nothing */
  CHECK(ft_edit_set(&ed, 5, "tint", "0 -2 0", &e) == 0);
  CHECK(ed.nframes == 3 && ed.f[2].index == 5);
  CHECK(ft_edit_set(&ed, 9, "grade", "7", &e) == -1);
  CHECK(strcmp(e.msg, "grade: 7 is out of range (0.2..5)") == 0);
  CHECK(ed.nframes == 3);
  CHECK(ft_edit_set(&ed, FT_EDIT_ROLL, "sharpen", "0.8", &e) == 0);
  CHECK(ed.taste.set & FT_T_SHARPEN);
  CHECK(ft_edit_set(&ed, -5, "grade", "1", &e) == -1);
  CHECK(strcmp(e.msg, "frame -5: not a frame number (0..9999)") == 0);

  /* the effective taste: frame keys replace the roll's */
  ft_edit_taste(&ed, 7, &t, &ff);
  CHECK_NEAR(t.grade, 1.2, 0);
  CHECK_NEAR(t.exposure, 0.2, 0); /* the roll's */
  CHECK(strcmp(t.curve[0], "strong") == 0);
  CHECK(ff == (FT_T_GRADE | FT_T_CURVE));
  CHECK(t.set == (FT_T_GRADE | FT_T_EXPOSURE | FT_T_CURVE | FT_T_SHARPEN));
  ft_edit_taste(&ed, 3, &t, &ff);
  CHECK_NEAR(t.exposure, -0.25, 0); /* replaced, not added */
  CHECK(ff == FT_T_EXPOSURE);
  ft_edit_taste(&ed, 42, &t, &ff); /* no overrides */
  CHECK(ff == 0 && t.set == ed.taste.set);

  /* clear: a frame with nothing left is dropped */
  CHECK(ft_edit_clear(&ed, 5, "tint", &e) == 0);
  CHECK(ed.nframes == 2);
  CHECK(ft_edit_clear(&ed, 5, "tint", &e) == 0); /* nothing to clear */
  CHECK(ft_edit_clear(&ed, 7, "gamma", &e) == -1);
  CHECK(strcmp(e.msg, "gamma: unknown key") == 0);
  CHECK(ft_edit_clear(&ed, FT_EDIT_ROLL, "grade", &e) == 0);
  CHECK(!(ed.taste.set & FT_T_GRADE));
  CHECK(ft_edit_clear(&ed, 7, NULL, &e) == 0);
  CHECK(ed.nframes == 1 && ed.f[0].index == 3);

  /* copy: overrides replaced; from a frame without any: dst dropped */
  CHECK(ft_edit_copy(&ed, 3, 9, &e) == 0);
  CHECK(ed.nframes == 2 && ed.f[1].index == 9);
  CHECK_NEAR(ed.f[1].o.exposure, -0.25, 0);
  CHECK(ft_edit_copy(&ed, 4, 9, &e) == 0);
  CHECK(ed.nframes == 1);
  CHECK(ft_edit_copy(&ed, 3, 1, &e) == 0); /* inserted before 3 */
  CHECK(ed.nframes == 2 && ed.f[0].index == 1 && ed.f[1].index == 3);
  CHECK_NEAR(ed.f[0].o.exposure, -0.25, 0);

  /* apply a look: stock and taste replaced, frames kept */
  CHECK(ft_edit_parse("[stock]\nstock_version = 1\nrange_ratio = 1.1 1 "
                      "0.9\n[taste]\ngrade = 2\n",
                      999, FT_LOOK_FILE, &look, &e) == 0);
  ft_edit_apply_look(&ed, &look, "hp5");
  CHECK(strcmp(ed.from, "hp5") == 0 && ed.stock.has_ratio);
  CHECK(ed.taste.set == FT_T_GRADE && ed.nframes == 2);
  ft_edit_write(&ed, FT_EDIT_FILE, buf, sizeof buf);
  CHECK(strcmp(buf,
               "from = hp5\n\n[stock]\nstock_version = 1\n"
               "range_ratio = 1.1 1 0.9\n\n[taste]\ngrade = 2\n\n"
               "[frame 1]\nexposure = -0.25\n\n[frame 3]\n"
               "exposure = -0.25\n") == 0);

  /* the key table, in write order */
  {
    unsigned bit = 0;
    int i;
    CHECK(strcmp(ft_taste_key(0, &bit), "exposure") == 0 &&
          bit == FT_T_EXPOSURE);
    CHECK(strcmp(ft_taste_key(12, &bit), "sharpen") == 0 &&
          bit == FT_T_SHARPEN);
    for(i = 0; ft_taste_key(i, &bit); i++) CHECK(bit == 1u << i);
    CHECK(i == 13 && ft_taste_key(-1, NULL) == NULL);
  }

  /* detail settings: the export defaults unless set */
  {
    ft_look_detail d;
    ft_look_detail_of(NULL, &d);
    CHECK(d.nr_sensor == 1 &&
          d.nr_chroma == 2 &&
          d.nr_grain == 0 &&
          d.sharpen == 0.5);
    memset(&t, 0, sizeof t);
    CHECK(ft_taste_set(&t, "nr_grain", "1.5", &e) == 0);
    ft_look_detail_of(&t, &d);
    CHECK(d.nr_grain == 1.5 && d.sharpen == 0.5);
  }
}

static void recipe_tests(void) {
  ft_image im;
  const ft_image* one[1];
  ft_recipe r0, r;
  ft_stock s;
  ft_taste t;
  ft_err e;
  float d[3];
  int c;

  ft_image_init(&im);
  CHECK(ft_image_alloc(&im, W, H, &e) == 0);
  negative(&im);
  one[0] = &im;

  /* nothing set: exactly the plain auto-estimate */
  ft_recipe_default(&r0, FT_RENDER_NEG);
  ft_recipe_set_base(&r0, BASE);
  CHECK(ft_render_auto(one, 1, &r0, &e) == 0);
  CHECK(ft_look_recipe(NULL, NULL, FT_RENDER_NEG, BASE, one, 1, &r, &e) == 0);
  CHECK(memcmp(&r, &r0, sizeof r) == 0);
  memset(&t, 0, sizeof t);
  memset(&s, 0, sizeof s);
  CHECK(ft_look_recipe(&s, &t, FT_RENDER_NEG, BASE, one, 1, &r, &e) == 0);
  CHECK(memcmp(&r, &r0, sizeof r) == 0);

  /* offsets: exposure on top of the auto value, tint from 0 */
  CHECK(ft_taste_set(&t, "exposure", "0.5", &e) == 0);
  CHECK(ft_taste_set(&t, "tint", "4 0 -3", &e) == 0);
  CHECK(ft_look_recipe(NULL, &t, FT_RENDER_NEG, BASE, one, 1, &r, &e) == 0);
  CHECK(r.exposure == (float)(r0.exposure + 0.5));
  CHECK(r.tint[0] == 4 && r.tint[1] == 0 && r.tint[2] == -3);
  CHECK(memcmp(r.range, r0.range, sizeof r.range) == 0);

  /* absolute fields before the estimate: grade moves the auto exposure */
  memset(&t, 0, sizeof t);
  CHECK(ft_taste_set(&t, "grade", "3", &e) == 0);
  CHECK(ft_taste_set(&t, "curve", "strong", &e) == 0);
  CHECK(ft_taste_set(&t, "curve_b", "0,0 0.5,0.45 1,1", &e) == 0);
  CHECK(ft_look_recipe(NULL, &t, FT_RENDER_NEG, BASE, one, 1, &r, &e) == 0);
  {
    ft_recipe g;
    ft_curve cv;
    ft_recipe_default(&g, FT_RENDER_NEG);
    g.grade = 3;
    ft_recipe_set_base(&g, BASE);
    CHECK(ft_render_auto(one, 1, &g, &e) == 0);
    CHECK(r.grade == 3 && r.exposure == g.exposure);
    CHECK(r.exposure != r0.exposure);
    CHECK(ft_curve_preset("strong", &cv) == 0);
    CHECK(memcmp(&r.master, &cv, sizeof cv) == 0);
    CHECK(r.ch[2].n == 3 && r.ch[2].y[1] == 0.45f && r.ch[0].n == 2);
  }

  /* the stock steers the channel ranges; green and exposure stay */
  memset(&s, 0, sizeof s);
  s.has_ratio = 1;
  s.range_ratio[0] = 1.1;
  s.range_ratio[1] = 1;
  s.range_ratio[2] = 0.9;
  CHECK(ft_look_recipe(&s, NULL, FT_RENDER_NEG, BASE, one, 1, &r, &e) == 0);
  CHECK(r.range[1] == r0.range[1] && r.exposure == r0.exposure);
  CHECK(r.range[0] == (float)(r0.range[1] * 1.1));
  CHECK(r.range[2] == (float)(r0.range[1] * 0.9));
  /* ... for negatives only */
  {
    ft_recipe sl;
    float black[3] = {3.0f, 3.0f, 3.0f};
    CHECK(ft_look_recipe(&s, NULL, FT_RENDER_SLIDE, black, one, 1, &sl, &e) ==
          0);
    CHECK(sl.range[0] == 1 && sl.range[2] == 1);
  }

  /* b&w: the stock's weights are used for the luminance */
  s.has_bw = 1;
  s.bw_weights[0] = 0.5;
  s.bw_weights[1] = 0.3;
  s.bw_weights[2] = 0.2;
  CHECK(ft_look_recipe(&s, NULL, FT_RENDER_BW, BASE, one, 1, &r, &e) == 0);
  CHECK(r.bw_weights[0] == 0.5f && r.bw_weights[2] == 0.2f);
  /* b&w ignores a look's tint (a colour look stays usable) */
  memset(&t, 0, sizeof t);
  CHECK(ft_taste_set(&t, "tint", "1 0 0", &e) == 0);
  CHECK(ft_look_recipe(NULL, &t, FT_RENDER_BW, BASE, one, 1, &r, &e) == 0);
  CHECK(r.tint[0] == 0 && r.tint[1] == 0 && r.tint[2] == 0);

  /* no base: estimated from the frame, as ft_render_base_from_frames */
  CHECK(ft_render_base_from_frames(one, 1, FT_RENDER_NEG, d, &e) == 0);
  CHECK(ft_look_recipe(NULL, NULL, FT_RENDER_NEG, NULL, one, 1, &r, &e) == 0);
  for(c = 0; c < 3; c++) CHECK(r.base[c] == d[c]);

  /* the offset is clamped to +-6 stops */
  memset(&t, 0, sizeof t);
  CHECK(ft_taste_set(&t, "exposure", "6", &e) == 0);
  CHECK(ft_look_recipe(NULL, &t, FT_RENDER_NEG, BASE, one, 1, &r, &e) == 0);
  CHECK(r0.exposure > 0 && r.exposure == 6);

  /* the resolved recipe is validated: paper white above black */
  memset(&t, 0, sizeof t);
  CHECK(ft_taste_set(&t, "paper_white", "3", &e) == 0);
  CHECK(ft_taste_set(&t, "paper_black", "1", &e) == 0);
  CHECK(ft_look_recipe(NULL, &t, FT_RENDER_NEG, BASE, one, 1, &r, &e) == -1);
  CHECK(strncmp(e.msg, "recipe: paper white 3, black 1", 30) == 0);
  ft_image_free(&im);
}

static void learn_tests(void) {
  double q[4][3] = {
    {1.1, 1, 0.9}, {1.0, 1, 0.95}, {1.2, 1, 0.85}, {1.05, 1, 0.92}};
  ft_stock old, out;
  double sp;
  ft_err e;

  /* the median per channel; spread = the widest channel's */
  CHECK(ft_stock_learn((const double (*)[3])q, 3, NULL, &out, &sp, &e) == 0);
  CHECK(out.present && out.version == 1 && out.has_ratio && !out.has_bw);
  CHECK_NEAR(out.range_ratio[0], 1.1, 1e-12);
  CHECK(out.range_ratio[1] == 1);
  CHECK_NEAR(out.range_ratio[2], 0.9, 1e-12);
  CHECK(out.n_frames == 3);
  CHECK_NEAR(sp, 0.2, 1e-12);
  /* even count: the mean of the middle two */
  CHECK(ft_stock_learn((const double (*)[3])q, 4, NULL, &out, NULL, &e) == 0);
  CHECK_NEAR(out.range_ratio[0], (1.05 + 1.1) / 2, 1e-12);
  CHECK_NEAR(out.range_ratio[2], (0.9 + 0.92) / 2, 1e-12);
  /* the input is not reordered */
  CHECK(q[0][0] == 1.1 && q[2][0] == 1.2);

  /* pooled with what the look learned before, weighted by frames; its
   * b&w weights stay */
  memset(&old, 0, sizeof old);
  old.present = 1;
  old.version = 1;
  old.has_ratio = 1;
  old.range_ratio[0] = old.range_ratio[1] = old.range_ratio[2] = 1;
  old.n_frames = 6;
  old.has_bw = 1;
  old.bw_weights[1] = 1;
  CHECK(ft_stock_learn((const double (*)[3])q, 3, &old, &out, NULL, &e) == 0);
  CHECK_NEAR(out.range_ratio[0], (6 * 1.0 + 3 * 1.1) / 9, 1e-12);
  CHECK_NEAR(out.range_ratio[2], (6 * 1.0 + 3 * 0.9) / 9, 1e-12);
  CHECK(out.n_frames == 9 && out.has_bw && out.bw_weights[1] == 1);
  /* an old stock without a ratio: just the new median */
  old.has_ratio = 0;
  old.n_frames = 0;
  CHECK(ft_stock_learn((const double (*)[3])q, 3, &old, &out, NULL, &e) == 0);
  CHECK_NEAR(out.range_ratio[0], 1.1, 1e-12);
  CHECK(out.n_frames == 3 && out.has_bw);

  /* errors */
  CHECK(ft_stock_learn((const double (*)[3])q, 0, NULL, &out, NULL, &e) == -1);
  CHECK(strcmp(e.msg, "learn: no frames") == 0);
  q[1][2] = 7;
  CHECK(ft_stock_learn((const double (*)[3])q, 3, NULL, &out, NULL, &e) == -1);
  CHECK(strcmp(e.msg, "learn: frame 2: ratio 7 out of range (0.2..5)") == 0);
}

int main(void) {
  text_tests();
  error_tests();
  op_tests();
  recipe_tests();
  learn_tests();
  return TEST_DONE();
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include "ft_print_file.h"
#include "test/ftest.h"

static ft_print_file pf, pf2;
static char buf[8192], buf2[8192];

static int parse(const char* t, ft_err* e) {
  return ft_print_file_parse(t, strlen(t), &pf, e);
}

static void fails(const char* t, const char* msg) {
  ft_err e;
  CHECK(parse(t, &e) == -1);
  if (strcmp(e.msg, msg) != 0)
    fprintf(stderr, "got: %s\nwant: %s\n", e.msg, msg);
  CHECK(strcmp(e.msg, msg) == 0);
}

int main(void) {
  static const char* TEXT =
    "# roll 12, Portra\n"
    "  emulsion=generic-neg\n"
    "[frame 7]\n"
    "cmyd = -1 0 0 +3   # warmer, darker\n"
    "\n"
    "[ frame 3 ]\n"
    "cmyd = 0 2 0 -1\n";
  static const char* CANON =
    "emulsion = generic-neg\n"
    "\n"
    "[frame 3]\n"
    "cmyd = 0 2 0 -1\n"
    "\n"
    "[frame 7]\n"
    "cmyd = -1 0 0 3\n";
  ft_cmyd k;
  ft_err e;
  size_t n;

  /* parse, sorted, canonical text; byte-stable after that */
  CHECK(parse(TEXT, &e) == 0);
  CHECK(strcmp(pf.emulsion, "generic-neg") == 0);
  CHECK(pf.nframes == 2 && pf.f[0].index == 3 && pf.f[1].index == 7);
  k = ft_print_file_clicks(&pf, 7);
  CHECK(k.c == -1 && k.m == 0 && k.y == 0 && k.d == 3);
  k = ft_print_file_clicks(&pf, 5);
  CHECK(k.c == 0 && k.m == 0 && k.y == 0 && k.d == 0);
  n = ft_print_file_write(&pf, buf, sizeof buf);
  CHECK(n == strlen(CANON) && strcmp(buf, CANON) == 0);
  CHECK(ft_print_file_parse(buf, n, &pf2, &e) == 0);
  CHECK(ft_print_file_write(&pf2, buf2, sizeof buf2) == n);
  CHECK(strcmp(buf, buf2) == 0);
  /* truncation: the length still comes back, the text is cut */
  CHECK(ft_print_file_write(&pf, buf, 10) == n);
  CHECK(strlen(buf) == 9);
  /* no frames */
  CHECK(parse("emulsion = generic-bw\n", &e) == 0);
  CHECK(ft_print_file_write(&pf, buf, sizeof buf) == 22);
  CHECK(strcmp(buf, "emulsion = generic-bw\n") == 0);

  /* edits */
  ft_print_file_init(&pf, "generic-neg");
  CHECK(pf.nframes == 0);
  CHECK(ft_print_file_set(&pf, 9, (ft_cmyd){0, 0, 1, 0}, &e) == 0);
  CHECK(ft_print_file_set(&pf, 2, (ft_cmyd){1, 0, 0, -2}, &e) == 0);
  CHECK(pf.nframes == 2 && pf.f[0].index == 2 && pf.f[1].index == 9);
  CHECK(ft_print_file_set(&pf, 2, (ft_cmyd){0, 0, 0, 5}, &e) == 0);
  CHECK(ft_print_file_clicks(&pf, 2).d == 5 &&
        ft_print_file_clicks(&pf, 2).c == 0);
  CHECK(ft_print_file_set(&pf, 9, (ft_cmyd){0, 0, 0, 0}, &e) == 0);
  CHECK(pf.nframes == 1 && pf.f[0].index == 2); /* auto: no section */
  CHECK(ft_print_file_set(&pf, 4, (ft_cmyd){0, 0, 0, 100}, &e) == -1);
  CHECK(strcmp(e.msg, "cmyd: D 100 is out of range (-99..99)") == 0);
  CHECK(ft_print_file_set(&pf, 10000, (ft_cmyd){0, 0, 0, 1}, &e) == -1);
  CHECK(strcmp(e.msg, "frame 10000 (0..9999)") == 0);
  ft_print_file_init(&pf, "generic-bw");
  CHECK(ft_print_file_set(&pf, 1, (ft_cmyd){0, 1, 0, 0}, &e) == -1);
  CHECK(strcmp(e.msg,
               "cmyd: generic-bw is black and white: C, M and Y "
               "must be 0") == 0);
  ft_print_file_init(&pf, "nope");
  CHECK(ft_print_file_set(&pf, 1, (ft_cmyd){0, 0, 0, 1}, &e) == -1);
  CHECK(strcmp(e.msg, "emulsion: 'nope' is not an emulsion") == 0);

  /* [frame N] not in the job: a warning, never an error */
  {
    static const int JOB[3] = {1, 2, 3};
    CHECK(parse("emulsion = generic-neg\n[frame 3]\ncmyd = 1 0 0 0\n"
                "[frame 7]\ncmyd = 0 0 0 1\n[frame 9]\ncmyd = 0 0 0 1\n",
                &e) == 0);
    CHECK(ft_print_file_unknown(&pf, JOB, 3, &e) == 2);
    CHECK(strcmp(e.msg,
                 "[frame 7] and 1 more are not in the job: "
                 "ignored") == 0);
    CHECK(ft_print_file_set(&pf, 9, (ft_cmyd){0, 0, 0, 0}, &e) == 0);
    CHECK(ft_print_file_unknown(&pf, JOB, 3, &e) == 1);
    CHECK(strcmp(e.msg, "[frame 7] is not in the job: ignored") == 0);
    CHECK(ft_print_file_unknown(&pf, JOB, 2, &e) == 2);
    /* the section survives a round trip */
    n = ft_print_file_write(&pf, buf, sizeof buf);
    CHECK(strstr(buf, "[frame 7]\ncmyd = 0 0 0 1\n") != NULL);
    strcpy(e.msg, "untouched");
    CHECK(ft_print_file_unknown(&pf, JOB, 0, &e) == 2);
    CHECK(parse("emulsion = generic-neg\n", &e) == 0);
    strcpy(e.msg, "untouched");
    CHECK(ft_print_file_unknown(&pf, JOB, 3, &e) == 0);
    CHECK(strcmp(e.msg, "untouched") == 0);
  }

  /* rotate: its own line, no cmyd = 0 0 0 0 beside it; a section
   * stays while it says anything */
  {
    static const char* ROT =
      "emulsion = generic-neg\n"
      "\n"
      "[frame 2]\n"
      "rotate = 90\n"
      "\n"
      "[frame 5]\n"
      "cmyd = 1 0 0 0\n"
      "rotate = 270\n";
    CHECK(parse("emulsion = generic-neg\n[frame 5]\nrotate = 270\n"
                "cmyd = 1 0 0 0\n[frame 2]\ncmyd = 0 0 0 0\n"
                "rotate = 90\n",
                &e) == 0);
    CHECK(ft_print_file_rotation(&pf, 2) == 90 &&
          ft_print_file_rotation(&pf, 5) == 270 &&
          ft_print_file_rotation(&pf, 3) == 0);
    n = ft_print_file_write(&pf, buf, sizeof buf);
    CHECK(strcmp(buf, ROT) == 0);
    CHECK(ft_print_file_parse(buf, n, &pf2, &e) == 0);
    CHECK(ft_print_file_write(&pf2, buf2, sizeof buf2) == n &&
          strcmp(buf, buf2) == 0);
    /* clicks back to auto keep a turned frame's section */
    CHECK(ft_print_file_set(&pf, 5, (ft_cmyd){0, 0, 0, 0}, &e) == 0);
    CHECK(pf.nframes == 2 && ft_print_file_rotation(&pf, 5) == 270);
    /* and the turn back to 0 drops it */
    CHECK(ft_print_file_set_rotation(&pf, 5, 0, &e) == 0);
    CHECK(pf.nframes == 1 && pf.f[0].index == 2);
    CHECK(ft_print_file_set_rotation(&pf, 8, 0, &e) == 0);
    CHECK(pf.nframes == 1); /* nothing to say: none */
    CHECK(ft_print_file_set_rotation(&pf, 8, 180, &e) == 0);
    CHECK(pf.nframes == 2 &&
          pf.f[1].index == 8 &&
          ft_print_file_clicks(&pf, 8).d == 0);
    CHECK(ft_print_file_set_rotation(&pf, 8, 45, &e) == -1);
    CHECK(strcmp(e.msg, "rotate: 45 is not 0, 90, 180 or 270") == 0);
    CHECK(ft_print_file_set_rotation(&pf, -1, 90, &e) == -1);
    CHECK(strcmp(e.msg, "frame -1 (0..9999)") == 0);
    /* rotate = 0 is never written */
    CHECK(parse("emulsion = generic-neg\n[frame 1]\nrotate = 0\n", &e) == 0);
    ft_print_file_write(&pf, buf, sizeof buf);
    CHECK(strcmp(buf,
                 "emulsion = generic-neg\n\n[frame 1]\n"
                 "cmyd = 0 0 0 0\n") == 0);
  }

  /* every error, pinned */
  fails("emulsion = generic-neg\n[frame 2]\nrotate = 45\n",
        "line 3: rotate: '45' is not 0, 90, 180 or 270");
  fails("emulsion = generic-neg\n[frame 2]\nrotate = -90\n",
        "line 3: rotate: '-90' is not 0, 90, 180 or 270");
  fails("emulsion = generic-neg\n[frame 2]\nrotate = 90\nrotate = 90\n",
        "line 4: rotate: set twice");
  fails("[frame 1]\ncmyd = 0 0 0 1\n", "emulsion: missing (emulsion = <id>)");
  fails("emulsion = portra400\n",
        "line 1: emulsion: 'portra400' is not an emulsion (generic-neg "
        "generic-slide generic-bw)");
  fails("emulsion = generic-neg\nemulsion = generic-neg\n",
        "line 2: emulsion: set twice");
  fails("emulsion = generic-neg\nexposure = 1\n",
        "line 2: unknown key 'exposure'");
  fails("emulsion = generic-neg\n[frame 2]\ntint = 1\n",
        "line 3: unknown key 'tint' in [frame 2]");
  fails("emulsion = generic-neg\njust words\n", "line 2: expected key = value");
  fails("emulsion = generic-neg\n[taste]\n", "line 2: unknown section [taste]");
  fails("emulsion = generic-neg\n[frame 2\n",
        "line 2: '[frame 2' is not a [section]");
  fails("emulsion = generic-neg\n[frame x]\n",
        "line 2: '[frame x]': the frame is a number (0..9999)");
  fails("emulsion = generic-neg\n[frame -1]\n",
        "line 2: '[frame -1]': the frame is a number (0..9999)");
  fails("emulsion = generic-neg\n[frame 2]\n[frame 2]\n",
        "line 3: [frame 2] twice");
  fails("emulsion = generic-neg\n[frame 2]\ncmyd = 1 2 3\n",
        "line 3: cmyd: '1 2 3' is not four whole numbers (c m y d, "
        "-99..99)");
  fails("emulsion = generic-neg\n[frame 2]\ncmyd = 1 2 3 4.5\n",
        "line 3: cmyd: '1 2 3 4.5' is not four whole numbers (c m y d, "
        "-99..99)");
  fails("emulsion = generic-neg\n[frame 2]\ncmyd = 0 0 0 100\n",
        "line 3: cmyd: '0 0 0 100' is not four whole numbers (c m y d, "
        "-99..99)");
  fails(
    "emulsion = generic-neg\n[frame 2]\ncmyd = 1 0 0 0\ncmyd = 1 0 0 "
    "0\n",
    "line 4: cmyd: set twice");
  fails("emulsion = generic-bw\n[frame 5]\ncmyd = 0 1 0 0\n",
        "line 2: cmyd: generic-bw is black and white: C, M and Y must "
        "be 0");
  fails("[frame 5]\nemulsion = generic-neg\n",
        "line 2: unknown key 'emulsion' in [frame 5]");
  {
    char big[600];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    fails(big, "line 1: too long");
  }
  /* more frames than a job holds */
  {
    size_t at = 0;
    int i;
    at += (size_t)sprintf(buf + at, "emulsion = generic-neg\n");
    for (i = 0; i <= FT_MAX_FRAMES; i++)
      at += (size_t)sprintf(buf + at, "[frame %d]\n", i);
    fails(buf, "line 66: more than 64 frames");
  }
  /* names: a section may hold only a name; written first, byte-stable;
   * set and cleared; file-safe characters only */
  {
    static const char* NAMED =
      "emulsion = generic-neg\n\n[frame 2]\nname = beach_01\n"
      "\n[frame 4]\nname = dog\ncmyd = 1 0 0 0\nrotate = 90\n";
    ft_err e;
    CHECK(parse(NAMED, &e) == 0);
    CHECK(strcmp(ft_print_file_name(&pf, 2), "beach_01") == 0);
    CHECK(strcmp(ft_print_file_name(&pf, 3), "") == 0);
    CHECK(ft_print_file_write(&pf, buf, sizeof buf) == strlen(NAMED));
    CHECK(strcmp(buf, NAMED) == 0);
    CHECK(ft_print_file_set_name(&pf, 2, "", &e) == 0);
    CHECK(pf.nframes == 1); /* frame 2 said nothing */
    CHECK(ft_print_file_set_name(&pf, 7, "x-7.a", &e) == 0);
    CHECK(ft_print_file_set_rotation(&pf, 4, 0, &e) == 0);
    ft_print_file_write(&pf, buf, sizeof buf);
    CHECK(strcmp(buf,
                 "emulsion = generic-neg\n\n[frame 4]\nname = dog\n"
                 "cmyd = 1 0 0 0\n\n[frame 7]\nname = x-7.a\n") == 0);
    CHECK(ft_print_file_set_name(&pf, 7, "a b", &e) == -1);
    CHECK(strcmp(e.msg,
                 "name: 'a b' is not 1..64 of A-Z a-z 0-9 . _ - "
                 "(not starting with .)") == 0);
    CHECK(ft_print_file_set_name(&pf, 7, ".x", &e) == -1);
  }
  fails("emulsion = generic-neg\n[frame 2]\nname = a/b\n",
        "line 3: name: 'a/b' is not 1..64 of A-Z a-z 0-9 . _ - (not "
        "starting with .)");
  fails("emulsion = generic-neg\n[frame 2]\nname = a\nname = b\n",
        "line 4: name: set twice");
  return TEST_DONE();
}

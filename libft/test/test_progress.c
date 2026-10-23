/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_progress.h"
#include "test/ftest.h"
#include <string.h>

/* p -> text == want, and want parses back to the same fields */
static void both(const ft_progress *p, const char *want) {
  char buf[400];
  ft_progress q;
  size_t n = ft_progress_fmt(p, buf, sizeof buf);
  CHECK(n == strlen(want) && strcmp(buf, want) == 0);
  CHECK(ft_progress_parse(want, strlen(want), &q) == 0);
  CHECK(q.type == p->type);
  switch(p->type) {
  case FT_PG_BEGIN: CHECK(q.kind == p->kind && q.n == p->n); break;
  case FT_PG_UNIT:
    CHECK(q.kind == p->kind && q.n == p->n && q.label == p->label);
    break;
  case FT_PG_LINES: CHECK(q.k == p->k && q.total == p->total); break;
  case FT_PG_FILE: CHECK(strcmp(q.path, p->path) == 0); break;
  case FT_PG_END: CHECK(q.status == p->status); break;
  case FT_PG_FOCUS:
    CHECK(q.k == p->k && q.label == p->label && q.off == p->off);
    break;
  case FT_PG_SHARP:
    CHECK(
        q.off == p->off && q.sharp > p->sharp - 5e-5 &&
        q.sharp < p->sharp + 5e-5
    );
    break;
  case FT_PG_FOCUSED: CHECK(q.off == p->off && q.how == p->how); break;
  }
}

static int bad(const char *s) {
  ft_progress q;
  return ft_progress_parse(s, strlen(s), &q) == -1;
}

int main(void) {
  ft_progress p;
  char buf[400];

  /* the wire format: pinned, the web server and page read it */
  memset(&p, 0, sizeof p);
  p.type = FT_PG_BEGIN;
  p.kind = FT_PG_WINDOW;
  p.n = 3;
  both(&p, "progress begin window 3\n");
  p.kind = FT_PG_PREVIEW;
  p.n = 1;
  both(&p, "progress begin preview 1\n");
  p.kind = FT_PG_FRAME;
  p.n = 12;
  both(&p, "progress begin frame 12\n");
  p.kind = FT_PG_PASS;
  p.n = 2;
  both(&p, "progress begin pass 2\n");
  p.kind = FT_PG_UPLOAD;
  p.n = 1; /* the microcode, before a run */
  both(&p, "progress begin upload 1\n");
  p.kind = (ft_pg_kind)(FT_PG_UPLOAD + 1);
  CHECK(ft_progress_fmt(&p, buf, sizeof buf) == FT_PG_BAD);
  p.type = FT_PG_UNIT;
  p.kind = FT_PG_WINDOW;
  p.n = 2;
  p.label = 5;
  both(&p, "progress unit window 2 5\n");
  p.type = FT_PG_LINES;
  p.k = 812;
  p.total = 1890;
  both(&p, "progress lines 812/1890\n");
  p.type = FT_PG_FILE;
  strcpy(p.path, "C:\\scans\\my strip\\strip_win05.raw");
  both(&p, "progress file C:\\scans\\my strip\\strip_win05.raw\n");
  p.type = FT_PG_END;
  p.status = FT_PG_OK;
  both(&p, "progress end ok\n");
  p.status = FT_PG_STOPPED;
  both(&p, "progress end stopped\n");
  p.status = FT_PG_FAILED;
  both(&p, "progress end failed\n");
  p.status = FT_PG_REFUSED;
  both(&p, "progress end refused\n");
  p.type = FT_PG_FOCUS;
  p.k = 3;
  p.label = 0;
  both(&p, "progress focus 3\n");
  p.label = 2;
  p.off = 500; /* crop 3: frame 2, offset +500 */
  both(&p, "progress focus 3 frame 2 at +500\n");
  p.off = -1250;
  both(&p, "progress focus 3 frame 2 at -1250\n");
  p.off = 0;
  both(&p, "progress focus 3 frame 2 at +0\n");
  p.k = 0; /* crops count from 1 */
  CHECK(ft_progress_fmt(&p, buf, sizeof buf) == FT_PG_BAD);
  memset(&p, 0, sizeof p);
  p.type = FT_PG_SHARP;
  p.off = -500;
  p.sharp = 0.0412;
  both(&p, "progress sharp -500 0.0412\n");
  p.off = 750;
  p.sharp = 1.5;
  both(&p, "progress sharp +750 1.5000\n");
  p.sharp = -1; /* sharpness is >= 0 */
  CHECK(ft_progress_fmt(&p, buf, sizeof buf) == FT_PG_BAD);
  memset(&p, 0, sizeof p);
  p.type = FT_PG_FOCUSED;
  p.off = 620;
  p.how = FT_PG_MEASURED;
  both(&p, "progress focused +620 measured\n");
  p.how = FT_PG_CHECKED;
  both(&p, "progress focused +620 checked\n");
  p.off = -300;
  p.how = FT_PG_CARRIED;
  both(&p, "progress focused -300 carried\n");
  p.off = 0;
  p.how = FT_PG_NONE;
  both(&p, "progress focused +0 none\n");

  /* parse: without the '\n', a '\r\n' ending, exactly n bytes */
  {
    ft_progress q;
    CHECK(
        ft_progress_parse("progress lines 5/9", 18, &q) == 0 &&
        q.type == FT_PG_LINES && q.k == 5 && q.total == 9
    );
    CHECK(
        ft_progress_parse("progress lines 5/9\r\n", 20, &q) == 0 && q.total == 9
    );
    CHECK(ft_progress_parse("progress lines 5/99", 18, &q) == 0 && q.total == 9);
  }
  /* anything else is not a progress line */
  CHECK(bad("frame 3: REFUSED (reason above)\n"));
  CHECK(bad("progress\n"));
  CHECK(bad("progress begin\n"));
  CHECK(bad("progress begin window\n"));
  CHECK(bad("progress begin window -1\n"));
  CHECK(bad("progress begin window 3x\n"));
  CHECK(bad("progress begin film 3\n"));
  CHECK(bad("progress unit window 2\n"));
  CHECK(bad("progress unit window 0 5\n"));
  CHECK(bad("progress lines 5\n"));
  CHECK(bad("progress lines 5/\n"));
  CHECK(bad("progress lines 10/9\n"));
  CHECK(bad("progress lines 99999999999999999999/1\n"));
  CHECK(bad("progress file \n"));
  CHECK(bad("progress end maybe\n"));
  CHECK(bad("progress end ok now\n"));
  CHECK(bad("progress  end ok\n"));
  CHECK(bad("Progress end ok\n"));
  CHECK(bad("progress focus\n"));
  CHECK(bad("progress focus 0\n"));
  CHECK(bad("progress focus 2 of 9\n"));
  CHECK(bad("progress focus -1\n"));
  CHECK(bad("progress focus 2 frame 0 at +5\n"));
  CHECK(bad("progress focus 2 frame 3\n"));
  CHECK(bad("progress focus 2 frame 3 at\n"));
  CHECK(bad("progress focus 2 frame 3 at 5x\n"));
  CHECK(bad("progress focus 2 frame 3 at +\n"));
  CHECK(bad("progress sharp +5\n"));
  CHECK(bad("progress sharp +5 x\n"));
  CHECK(bad("progress sharp +5 0.\n"));
  CHECK(bad("progress sharp +5 -0.1\n"));
  CHECK(bad("progress sharp +5 0.1 more\n"));
  CHECK(bad("progress focused +5\n"));
  CHECK(bad("progress focused +5 guessed\n"));
  CHECK(bad("progress focused 5x measured\n"));
  /* a path the line cannot carry: nothing written */
  memset(&p, 0, sizeof p);
  p.type = FT_PG_FILE;
  strcpy(p.path, "a\nb");
  CHECK(ft_progress_fmt(&p, buf, sizeof buf) == FT_PG_BAD && buf[0] == 0);
  p.path[0] = 0;
  CHECK(ft_progress_fmt(&p, buf, sizeof buf) == FT_PG_BAD);
  /* truncation: the length needed, NUL-terminated */
  p.type = FT_PG_END;
  p.status = FT_PG_OK;
  CHECK(ft_progress_fmt(&p, buf, 5) == 16 && strcmp(buf, "prog") == 0);
  return TEST_DONE();
}

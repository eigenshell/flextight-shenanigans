/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_print_file.h"
#include "ft_emulsion.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_INDEX 9999

static int space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
         c == '\v';
}

static char *trim(char *s) {
  size_t n;
  while(space(*s))
    s++;
  n = strlen(s);
  while(n > 0 && space(s[n - 1]))
    s[--n] = 0;
  return s;
}

/* the first c in s, or NULL (strchr is not on the purity list) */
static char *find_char(char *s, char c) {
  for(; *s; s++)
    if(*s == c)
      return s;
  return NULL;
}

/* k whole numbers in -lim..lim separated by spaces, nothing else */
static int wholes(const char *s, int k, double lim, int *out) {
  int i;
  for(i = 0; i < k; i++) {
    char *end;
    double v = strtod(s, &end);
    if(end == s || !(v == floor(v)) || v < -lim || v > lim)
      return -1;
    out[i] = (int)v;
    s = end;
  }
  while(space(*s))
    s++;
  return *s ? -1 : 0;
}

void ft_print_file_init(ft_print_file *pf, const char *emulsion) {
  memset(pf, 0, sizeof *pf);
  if(strlen(emulsion) < sizeof pf->emulsion)
    memcpy(pf->emulsion, emulsion, strlen(emulsion) + 1);
}

static int find(const ft_print_file *pf, int index) {
  int i;
  for(i = 0; i < pf->nframes; i++)
    if(pf->f[i].index == index)
      return i;
  return -1;
}

/* a new section, in order; -1 if full */
static int insert(ft_print_file *pf, int index) {
  int at = 0;
  if(pf->nframes >= FT_MAX_FRAMES)
    return -1;
  while(at < pf->nframes && pf->f[at].index < index)
    at++;
  memmove(
      &pf->f[at + 1], &pf->f[at], (size_t)(pf->nframes - at) * sizeof pf->f[0]
  );
  memset(&pf->f[at], 0, sizeof pf->f[at]);
  pf->f[at].index = index;
  pf->nframes++;
  return at;
}

static int rotation_ok(int deg) {
  return deg == 0 || deg == 90 || deg == 180 || deg == 270;
}

/* a frame name: [A-Za-z0-9._-], 1..64, no leading '.' (ftwebd's file
 * names) */
static int name_ok(const char *s) {
  size_t i, n = strlen(s);
  if(n < 1 || n >= FT_PRINT_NAME || s[0] == '.')
    return 0;
  for(i = 0; i < n; i++) {
    char c = s[i];
    if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
      return 0;
  }
  return 1;
}
#define NAME_RULE "is not 1..64 of A-Z a-z 0-9 . _ - (not starting with .)"

static int auto_clicks(ft_cmyd k) {
  return !k.c && !k.m && !k.y && !k.d;
}

/* frame i's section goes when it says nothing */
static void drop_if_empty(ft_print_file *pf, int i) {
  if(i < 0 || !auto_clicks(pf->f[i].k) || pf->f[i].rotate || pf->f[i].name[0])
    return;
  memmove(
      &pf->f[i], &pf->f[i + 1], (size_t)(pf->nframes - i - 1) * sizeof pf->f[0]
  );
  pf->nframes--;
}

static int list_ids(ft_err *e, int ln, const char *v) {
  char ids[160];
  size_t n = 0;
  int i;
  const ft_emulsion *em;
  ids[0] = 0;
  for(i = 0; (em = ft_emulsion_at(i)) != NULL; i++) {
    size_t l = strlen(em->id);
    if(n + l + 2 >= sizeof ids)
      break;
    if(n)
      ids[n++] = ' ';
    memcpy(ids + n, em->id, l + 1);
    n += l;
  }
  return ft_fail(
      e, "line %d: emulsion: '%.40s' is not an emulsion (%s)", ln, v, ids
  );
}

int ft_print_file_parse(
    const char *text, size_t n, ft_print_file *pf, ft_err *e
) {
  const ft_emulsion *em = NULL;
  int ln = 0, cur = -1, had_cmyd = 0, had_rot = 0, had_name = 0, i;
  size_t at = 0;
  char line[256];
  ft_err in;

  memset(pf, 0, sizeof *pf);
  while(at < n && text[at]) {
    size_t len = 0;
    char *s, *key, *val, *eq;
    ln++;
    while(at < n && text[at] && text[at] != '\n') {
      if(len + 1 >= sizeof line)
        return ft_fail(e, "line %d: too long", ln);
      line[len++] = text[at++];
    }
    if(at < n && text[at] == '\n')
      at++;
    line[len] = 0;
    s = find_char(line, '#'); /* '#' always starts a comment */
    if(s)
      *s = 0;
    s = trim(line);
    if(!*s)
      continue;
    if(*s == '[') {
      int idx;
      size_t l = strlen(s);
      if(s[l - 1] != ']')
        return ft_fail(e, "line %d: '%.40s' is not a [section]", ln, s);
      s[l - 1] = 0;
      s = trim(s + 1);
      if(strlen(s) < 6 || memcmp(s, "frame", 5) != 0 || !space(s[5]))
        return ft_fail(e, "line %d: unknown section [%.40s]", ln, s);
      if(wholes(s + 5, 1, MAX_INDEX, &idx) || idx < 0)
        return ft_fail(
            e,
            "line %d: '[%.40s]': the frame is a number "
            "(0..%d)",
            ln, s, MAX_INDEX
        );
      if(find(pf, idx) >= 0)
        return ft_fail(e, "line %d: [frame %d] twice", ln, idx);
      cur = insert(pf, idx);
      if(cur < 0)
        return ft_fail(e, "line %d: more than %d frames", ln, FT_MAX_FRAMES);
      pf->f[cur].line = ln;
      had_cmyd = had_rot = had_name = 0;
      continue;
    }
    eq = find_char(s, '=');
    if(!eq)
      return ft_fail(e, "line %d: expected key = value", ln);
    *eq = 0;
    key = trim(s);
    val = trim(eq + 1);
    if(cur < 0) {
      if(strcmp(key, "emulsion") != 0)
        return ft_fail(e, "line %d: unknown key '%.40s'", ln, key);
      if(em)
        return ft_fail(e, "line %d: emulsion: set twice", ln);
      em = ft_emulsion_find(val);
      if(!em)
        return list_ids(e, ln, val);
      memcpy(pf->emulsion, em->id, strlen(em->id) + 1);
    } else if(strcmp(key, "name") == 0) {
      if(had_name)
        return ft_fail(e, "line %d: name: set twice", ln);
      if(!name_ok(val))
        return ft_fail(e, "line %d: name: '%.40s' " NAME_RULE, ln, val);
      memcpy(pf->f[cur].name, val, strlen(val) + 1);
      had_name = 1;
    } else if(strcmp(key, "rotate") == 0) {
      int v;
      if(had_rot)
        return ft_fail(e, "line %d: rotate: set twice", ln);
      if(wholes(val, 1, 270, &v) || !rotation_ok(v))
        return ft_fail(
            e,
            "line %d: rotate: '%.40s' is not 0, 90, "
            "180 or 270",
            ln, val
        );
      pf->f[cur].rotate = v;
      had_rot = 1;
    } else {
      int v[4];
      if(strcmp(key, "cmyd") != 0)
        return ft_fail(
            e,
            "line %d: unknown key '%.40s' in [frame "
            "%d]",
            ln, key, pf->f[cur].index
        );
      if(had_cmyd)
        return ft_fail(e, "line %d: cmyd: set twice", ln);
      if(wholes(val, 4, FT_PRINT_CLICKS_MAX, v))
        return ft_fail(
            e,
            "line %d: cmyd: '%.40s' is not four whole "
            "numbers (c m y d, -%d..%d)",
            ln, val, FT_PRINT_CLICKS_MAX, FT_PRINT_CLICKS_MAX
        );
      pf->f[cur].k.c = v[0];
      pf->f[cur].k.m = v[1];
      pf->f[cur].k.y = v[2];
      pf->f[cur].k.d = v[3];
      had_cmyd = 1;
    }
  }
  if(!em)
    return ft_fail(e, "emulsion: missing (emulsion = <id>)");
  for(i = 0; i < pf->nframes; i++)
    if(ft_print_clicks_ok(em, pf->f[i].k, &in))
      return ft_fail(e, "line %d: %.180s", pf->f[i].line, in.msg);
  return 0;
}

typedef struct {
  char *b;
  size_t cap, n;
} wbuf;

static void put(wbuf *w, const char *s) {
  for(; *s; s++, w->n++)
    if(w->n + 1 < w->cap)
      w->b[w->n] = *s;
  if(w->cap)
    w->b[w->n < w->cap ? w->n : w->cap - 1] = 0;
}

static void put_int(wbuf *w, int v) {
  char d[16], o[16];
  int nd = 0, n = 0;
  unsigned u = v < 0 ? 0u - (unsigned)v : (unsigned)v;
  if(v < 0)
    o[n++] = '-';
  do {
    d[nd++] = (char)('0' + u % 10);
    u /= 10;
  } while(u);
  while(nd)
    o[n++] = d[--nd];
  o[n] = 0;
  put(w, o);
}

size_t ft_print_file_write(const ft_print_file *pf, char *buf, size_t cap) {
  wbuf w;
  int i;
  w.b = buf;
  w.cap = cap;
  w.n = 0;
  if(cap)
    buf[0] = 0;
  put(&w, "emulsion = ");
  put(&w, pf->emulsion);
  put(&w, "\n");
  for(i = 0; i < pf->nframes; i++) {
    const ft_cmyd *k = &pf->f[i].k;
    put(&w, "\n[frame ");
    put_int(&w, pf->f[i].index);
    put(&w, "]\n");
    if(pf->f[i].name[0]) {
      put(&w, "name = ");
      put(&w, pf->f[i].name);
      put(&w, "\n");
    }
    if(!auto_clicks(*k) || (!pf->f[i].rotate && !pf->f[i].name[0])) {
      put(&w, "cmyd = ");
      put_int(&w, k->c);
      put(&w, " ");
      put_int(&w, k->m);
      put(&w, " ");
      put_int(&w, k->y);
      put(&w, " ");
      put_int(&w, k->d);
      put(&w, "\n");
    }
    if(pf->f[i].rotate) {
      put(&w, "rotate = ");
      put_int(&w, pf->f[i].rotate);
      put(&w, "\n");
    }
  }
  return w.n;
}

ft_cmyd ft_print_file_clicks(const ft_print_file *pf, int frame) {
  static const ft_cmyd NONE = {0, 0, 0, 0};
  int i = find(pf, frame);
  return i < 0 ? NONE : pf->f[i].k;
}

int ft_print_file_set(ft_print_file *pf, int frame, ft_cmyd k, ft_err *e) {
  const ft_emulsion *em = ft_emulsion_find(pf->emulsion);
  int i;
  if(!em)
    return ft_fail(e, "emulsion: '%.40s' is not an emulsion", pf->emulsion);
  if(frame < 0 || frame > MAX_INDEX)
    return ft_fail(e, "frame %d (0..%d)", frame, MAX_INDEX);
  if(ft_print_clicks_ok(em, k, e))
    return -1;
  i = find(pf, frame);
  if(i < 0 && auto_clicks(k))
    return 0;
  if(i < 0 && (i = insert(pf, frame)) < 0)
    return ft_fail(e, "more than %d frames", FT_MAX_FRAMES);
  pf->f[i].k = k;
  drop_if_empty(pf, i);
  return 0;
}

int ft_print_file_rotation(const ft_print_file *pf, int frame) {
  int i = find(pf, frame);
  return i < 0 ? 0 : pf->f[i].rotate;
}

int ft_print_file_set_rotation(
    ft_print_file *pf, int frame, int deg, ft_err *e
) {
  int i;
  if(frame < 0 || frame > MAX_INDEX)
    return ft_fail(e, "frame %d (0..%d)", frame, MAX_INDEX);
  if(!rotation_ok(deg))
    return ft_fail(e, "rotate: %d is not 0, 90, 180 or 270", deg);
  i = find(pf, frame);
  if(i < 0 && !deg)
    return 0;
  if(i < 0 && (i = insert(pf, frame)) < 0)
    return ft_fail(e, "more than %d frames", FT_MAX_FRAMES);
  pf->f[i].rotate = deg;
  drop_if_empty(pf, i);
  return 0;
}

const char *ft_print_file_name(const ft_print_file *pf, int frame) {
  int i = find(pf, frame);
  return i < 0 ? "" : pf->f[i].name;
}

int ft_print_file_set_name(
    ft_print_file *pf, int frame, const char *name, ft_err *e
) {
  int i;
  if(frame < 0 || frame > MAX_INDEX)
    return ft_fail(e, "frame %d (0..%d)", frame, MAX_INDEX);
  if(name[0] && !name_ok(name))
    return ft_fail(e, "name: '%.40s' " NAME_RULE, name);
  i = find(pf, frame);
  if(i < 0 && !name[0])
    return 0;
  if(i < 0 && (i = insert(pf, frame)) < 0)
    return ft_fail(e, "more than %d frames", FT_MAX_FRAMES);
  memcpy(pf->f[i].name, name, strlen(name) + 1);
  drop_if_empty(pf, i);
  return 0;
}

int ft_print_file_unknown(
    const ft_print_file *pf, const int *frames, int n, ft_err *w
) {
  int i, j, count = 0, first = -1;
  for(i = 0; i < pf->nframes; i++) {
    for(j = 0; j < n && frames[j] != pf->f[i].index; j++)
      ;
    if(j == n) {
      if(first < 0)
        first = pf->f[i].index;
      count++;
    }
  }
  if(count == 1)
    ft_fail(w, "[frame %d] is not in the job: ignored", first);
  else if(count > 1)
    ft_fail(
        w, "[frame %d] and %d more are not in the job: ignored", first,
        count - 1
    );
  return count;
}

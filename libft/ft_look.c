/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_look.h"
#include "ft_curve.h"
#include "ft_render_auto.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { K_NUM, K_TRIPLE, K_CURVE };

typedef struct {
  const char *name;
  unsigned bit;
  int kind;
  double lo, hi;
} tkey;

/* table order = write order = the FT_T_* bit order */
static const tkey TASTE[] = {
    {"exposure", FT_T_EXPOSURE, K_NUM, -6, 6},
    {"tint", FT_T_TINT, K_TRIPLE, -50, 50},
    {"grade", FT_T_GRADE, K_NUM, 0.2, 5},
    {"paper_black", FT_T_PAPER_BLACK, K_NUM, 0, 4},
    {"paper_white", FT_T_PAPER_WHITE, K_NUM, 0, 4},
    {"curve", FT_T_CURVE, K_CURVE, 0, 0},
    {"curve_r", FT_T_CURVE_R, K_CURVE, 0, 0},
    {"curve_g", FT_T_CURVE_G, K_CURVE, 0, 0},
    {"curve_b", FT_T_CURVE_B, K_CURVE, 0, 0},
    {"nr_sensor", FT_T_NR_SENSOR, K_NUM, 0, 4},
    {"nr_chroma", FT_T_NR_CHROMA, K_NUM, 0, 8},
    {"nr_grain", FT_T_NR_GRAIN, K_NUM, 0, 4},
    {"sharpen", FT_T_SHARPEN, K_NUM, 0, 3},
};
#define N_TASTE (int)(sizeof TASTE / sizeof TASTE[0])

static const char *STOCK_KEYS[] = {
    "stock_version", "range_ratio", "bw_weights", "n_frames"
};
#define N_STOCK 4
#define SK_VERSION 1u

/* the export defaults (ftdev export) */
#define DEF_NR_SENSOR 1.0
#define DEF_NR_CHROMA 2.0
#define DEF_NR_GRAIN 0.0
#define DEF_SHARPEN 0.5

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

static int rest_blank(const char *s) {
  while(space(*s))
    s++;
  return *s == 0;
}

/* one finite number and nothing else */
static int num1(const char *s, double *v) {
  char *end;
  *v = strtod(s, &end);
  return end != s && rest_blank(end) && isfinite(*v) ? 0 : -1;
}

/* three finite numbers separated by spaces and nothing else */
static int num3(const char *s, double v[3]) {
  int i;
  for(i = 0; i < 3; i++) {
    char *end;
    v[i] = strtod(s, &end);
    if(end == s || !isfinite(v[i]))
      return -1;
    s = end;
  }
  return rest_blank(s) ? 0 : -1;
}

/* a whole number in lo..hi */
static int whole(const char *s, double lo, double hi, long *out) {
  double v;
  if(num1(s, &v) || v != floor(v) || v < lo || v > hi)
    return -1;
  *out = (long)v;
  return 0;
}

static int taste_index(const char *key) {
  int i;
  for(i = 0; i < N_TASTE; i++)
    if(strcmp(TASTE[i].name, key) == 0)
      return i;
  return -1;
}

static int stock_index(const char *key) {
  int i;
  for(i = 0; i < N_STOCK; i++)
    if(strcmp(STOCK_KEYS[i], key) == 0)
      return i;
  return -1;
}

static double *num_field(ft_taste *t, unsigned bit) {
  switch(bit) {
  case FT_T_EXPOSURE: return &t->exposure;
  case FT_T_GRADE: return &t->grade;
  case FT_T_PAPER_BLACK: return &t->paper_black;
  case FT_T_PAPER_WHITE: return &t->paper_white;
  case FT_T_NR_SENSOR: return &t->nr_sensor;
  case FT_T_NR_CHROMA: return &t->nr_chroma;
  case FT_T_NR_GRAIN: return &t->nr_grain;
  default: return &t->sharpen;
  }
}

/* index into ft_taste.curve of a curve key's bit */
static int curve_slot(unsigned bit) {
  return bit == FT_T_CURVE     ? 0
         : bit == FT_T_CURVE_R ? 1
         : bit == FT_T_CURVE_G ? 2
                               : 3;
}

/* text -> curve: a preset name or points; -1 with e naming the presets */
static int curve_of(const char *key, const char *txt, ft_curve *c, ft_err *e) {
  ft_err in;
  const char *m;
  if(ft_curve_preset(txt, c) == 0)
    return 0;
  if(ft_curve_parse(txt, c, &in) == 0)
    return 0;
  m = memcmp(in.msg, "curve: ", 7) == 0 ? in.msg + 7 : in.msg;
  return ft_fail(
      e,
      "%s: not a preset (linear soft medium strong fade) "
      "and not points: %.100s",
      key, m
  );
}

void ft_edit_init(ft_edit *ed) {
  memset(ed, 0, sizeof *ed);
}

const char *ft_taste_key(int i, unsigned *bit) {
  if(i < 0 || i >= N_TASTE)
    return NULL;
  if(bit)
    *bit = TASTE[i].bit;
  return TASTE[i].name;
}

int ft_taste_set(ft_taste *t, const char *key, const char *value, ft_err *e) {
  int i = taste_index(key), c;
  const tkey *k;
  double v[3];
  if(i < 0)
    return stock_index(key) >= 0
               ? ft_fail(
                     e,
                     "%.40s: a [stock] key (roll-wide): put it in "
                     "[stock]",
                     key
                 )
               : ft_fail(e, "%.40s: unknown key", key);
  k = &TASTE[i];
  if(k->kind == K_NUM) {
    if(num1(value, &v[0]))
      return ft_fail(e, "%s: '%.40s' is not a number", k->name, value);
    if(!(v[0] >= k->lo && v[0] <= k->hi))
      return ft_fail(
          e, "%s: %.40s is out of range (%g..%g)", k->name, value, k->lo, k->hi
      );
    *num_field(t, k->bit) = v[0];
  } else if(k->kind == K_TRIPLE) {
    if(num3(value, v))
      return ft_fail(
          e, "%s: '%.40s' is not three numbers (r g b)", k->name, value
      );
    for(c = 0; c < 3; c++)
      if(!(v[c] >= k->lo && v[c] <= k->hi))
        return ft_fail(
            e, "%s: %g is out of range (%g..%g)", k->name, v[c], k->lo, k->hi
        );
    memcpy(t->tint, v, sizeof v);
  } else {
    /* stored with single spaces, so it writes back the same */
    char norm[FT_LOOK_CURVE_TXT];
    size_t n = 0;
    const char *s = value;
    ft_curve cv;
    while(*s) {
      if(space(*s)) {
        while(space(*s))
          s++;
        if(*s && n > 0) {
          if(n + 1 >= sizeof norm)
            return ft_fail(e, "%s: too long", k->name);
          norm[n++] = ' ';
        }
        continue;
      }
      if(n + 1 >= sizeof norm)
        return ft_fail(e, "%s: too long", k->name);
      norm[n++] = *s++;
    }
    norm[n] = 0;
    if(curve_of(k->name, norm, &cv, e))
      return -1;
    memcpy(t->curve[curve_slot(k->bit)], norm, n + 1);
  }
  t->set |= k->bit;
  return 0;
}

static int stock_set(
    ft_stock *s, unsigned *seen, const char *key, const char *value, ft_err *e
) {
  int i = stock_index(key), c;
  double v[3];
  long l;
  if(i < 0)
    return taste_index(key) >= 0
               ? ft_fail(e, "%.40s: a taste key: put it in [taste]", key)
               : ft_fail(e, "%.40s: unknown key", key);
  if(*seen & (1u << i))
    return ft_fail(e, "%s: set twice", key);
  *seen |= 1u << i;
  switch(i) {
  case 0:
    if(whole(value, 1, 1e6, &l))
      return ft_fail(e, "stock_version: '%.40s' is not a version", value);
    if(l > FT_STOCK_VERSION)
      return ft_fail(
          e,
          "stock_version: %ld needs a newer libft (this "
          "one reads %d)",
          l, FT_STOCK_VERSION
      );
    s->version = (int)l;
    break;
  case 1:
    if(num3(value, v))
      return ft_fail(
          e,
          "range_ratio: '%.40s' is not three numbers "
          "(r g b)",
          value
      );
    for(c = 0; c < 3; c++)
      if(!(v[c] >= 0.2 && v[c] <= 5))
        return ft_fail(
            e,
            "range_ratio: %g is out of range "
            "(0.2..5)",
            v[c]
        );
    if(v[1] != 1)
      return ft_fail(
          e, "range_ratio: green must be 1 (the ratios "
             "are relative to green)"
      );
    memcpy(s->range_ratio, v, sizeof v);
    s->has_ratio = 1;
    break;
  case 2:
    if(num3(value, v))
      return ft_fail(
          e,
          "bw_weights: '%.40s' is not three numbers "
          "(r g b)",
          value
      );
    for(c = 0; c < 3; c++)
      if(!(v[c] >= 0 && v[c] <= 1))
        return ft_fail(e, "bw_weights: %g is out of range (0..1)", v[c]);
    if(fabs(v[0] + v[1] + v[2] - 1) > 1e-3)
      return ft_fail(e, "bw_weights: sum to %g, not 1", v[0] + v[1] + v[2]);
    memcpy(s->bw_weights, v, sizeof v);
    s->has_bw = 1;
    break;
  default:
    if(whole(value, 0, 1e9, &l))
      return ft_fail(e, "n_frames: '%.40s' is not a count", value);
    s->n_frames = l;
    break;
  }
  return 0;
}

/* the slot of frame index, or -1 */
static int frame_find(const ft_edit *ed, int index) {
  int i;
  for(i = 0; i < ed->nframes; i++)
    if(ed->f[i].index == index)
      return i;
  return -1;
}

/* the slot of frame index, created (in index order) if missing; -1 full */
static int frame_slot(ft_edit *ed, int index) {
  int i = frame_find(ed, index), at;
  if(i >= 0)
    return i;
  if(ed->nframes == FT_MAX_FRAMES)
    return -1;
  for(at = ed->nframes; at > 0 && ed->f[at - 1].index > index; at--)
    ed->f[at] = ed->f[at - 1];
  memset(&ed->f[at], 0, sizeof ed->f[at]);
  ed->f[at].index = index;
  ed->nframes++;
  return at;
}

/* the line's error: "line N: " + the inner message */
static int at_line(ft_err *e, int ln, const ft_err *in) {
  return ft_fail(e, "line %d: %.180s", ln, in->msg);
}

int ft_edit_parse(
    const char *text, size_t n, ft_look_kind k, ft_edit *ed, ft_err *e
) {
  enum { S_TOP, S_STOCK, S_TASTE, S_FRAME } sec = S_TOP;
  ft_taste *cur = NULL;
  unsigned stock_seen = 0;
  int ln = 0, stock_line = 0, had_taste = 0, had_from = 0;
  size_t at = 0;
  char line[512];
  ft_err in;

  ft_edit_init(ed);
  while(at < n && text[at]) {
    size_t len = 0, i;
    char *s, *key, *val, *eq = NULL;
    ln++;
    while(at < n && text[at] && text[at] != '\n') {
      if(len + 1 >= sizeof line)
        return ft_fail(e, "line %d: too long", ln);
      line[len++] = text[at++];
    }
    if(at < n && text[at] == '\n')
      at++;
    line[len] = 0;
    for(i = 0; i < len; i++)
      if(line[i] == '#') {
        line[i] = 0; /* '#' always starts a comment */
        break;
      }
    s = trim(line);
    if(!*s)
      continue;
    if(*s == '[') {
      size_t sl = strlen(s);
      char *name;
      if(sec == S_STOCK && !(stock_seen & SK_VERSION))
        return ft_fail(e, "line %d: [stock] has no stock_version", stock_line);
      if(s[sl - 1] != ']')
        return ft_fail(e, "line %d: '%.40s' is not a [section]", ln, s);
      s[sl - 1] = 0;
      name = trim(s + 1);
      if(strcmp(name, "stock") == 0) {
        if(ed->stock.present)
          return ft_fail(e, "line %d: [stock] twice", ln);
        ed->stock.present = 1;
        stock_line = ln;
        sec = S_STOCK;
      } else if(strcmp(name, "taste") == 0) {
        if(had_taste)
          return ft_fail(e, "line %d: [taste] twice", ln);
        had_taste = 1;
        cur = &ed->taste;
        sec = S_TASTE;
      } else if(
          strlen(name) > 5 && memcmp(name, "frame", 5) == 0 && space(name[5])
      ) {
        long idx;
        int slot;
        if(k == FT_LOOK_FILE)
          return ft_fail(
              e,
              "line %d: [frame N] belongs in a "
              ".edit, not a .look",
              ln
          );
        if(whole(name + 6, 0, 9999, &idx))
          return ft_fail(
              e,
              "line %d: '[%.40s]': the frame is a "
              "number 0..9999",
              ln, name
          );
        if(frame_find(ed, (int)idx) >= 0)
          return ft_fail(e, "line %d: [frame %ld] twice", ln, idx);
        if(ed->nframes == FT_MAX_FRAMES)
          return ft_fail(e, "line %d: more than %d frames", ln, FT_MAX_FRAMES);
        /* in file order, so a parsed text writes back the same */
        slot = ed->nframes++;
        ed->f[slot].index = (int)idx;
        cur = &ed->f[slot].o;
        sec = S_FRAME;
      } else {
        return ft_fail(e, "line %d: unknown section [%.40s]", ln, name);
      }
      continue;
    }
    for(i = 0; s[i]; i++)
      if(s[i] == '=') {
        eq = s + i;
        break;
      }
    if(!eq)
      return ft_fail(e, "line %d: expected key = value", ln);
    *eq = 0;
    key = trim(s);
    val = trim(eq + 1);
    if(sec == S_TOP) {
      if(k == FT_EDIT_FILE && strcmp(key, "from") == 0) {
        size_t vl = strlen(val);
        if(had_from)
          return ft_fail(e, "line %d: from: set twice", ln);
        if(vl == 0 || vl >= FT_LOOK_NAME)
          return ft_fail(
              e,
              "line %d: from: a name of 1..%d "
              "characters",
              ln, FT_LOOK_NAME - 1
          );
        memcpy(ed->from, val, vl + 1);
        had_from = 1;
        continue;
      }
      return ft_fail(
          e,
          "line %d: %.40s: outside a section ([stock], "
          "[taste]%s)",
          ln, key, k == FT_EDIT_FILE ? ", [frame N]" : ""
      );
    }
    if(sec == S_STOCK) {
      if(stock_set(&ed->stock, &stock_seen, key, val, &in))
        return at_line(e, ln, &in);
      continue;
    }
    {
      int ti = taste_index(key);
      if(ti >= 0 && (cur->set & TASTE[ti].bit))
        return ft_fail(e, "line %d: %s: set twice", ln, key);
      if(ft_taste_set(cur, key, val, &in))
        return at_line(e, ln, &in);
    }
  }
  if(sec == S_STOCK && !(stock_seen & SK_VERSION))
    return ft_fail(e, "line %d: [stock] has no stock_version", stock_line);
  return 0;
}

/* --- writing --- */

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

/* v rounded to 6 decimals, trailing zeros dropped: "0.9", "-1.25", "3" */
static void put_num(wbuf *w, double v) {
  char o[48];
  int n = 0, k;
  double s = v * 1e6;
  unsigned long long q, ip;
  unsigned long fp;
  int neg = s < 0;
  s = floor(fabs(s) + 0.5);
  q = (unsigned long long)s;
  if(q == 0)
    neg = 0; /* no "-0" */
  ip = q / 1000000u;
  fp = (unsigned long)(q % 1000000u);
  if(neg)
    o[n++] = '-';
  {
    char d[24];
    int nd = 0;
    do {
      d[nd++] = (char)('0' + ip % 10);
      ip /= 10;
    } while(ip);
    while(nd)
      o[n++] = d[--nd];
  }
  if(fp) {
    o[n++] = '.';
    for(k = 100000; k >= 1 && fp; k /= 10) {
      o[n++] = (char)('0' + fp / k);
      fp %= k;
    }
  }
  o[n] = 0;
  put(w, o);
}

static void put_nums(wbuf *w, const double *v, int n) {
  int i;
  for(i = 0; i < n; i++) {
    if(i)
      put(w, " ");
    put_num(w, v[i]);
  }
}

static void put_taste(wbuf *w, const ft_taste *t) {
  int i;
  for(i = 0; i < N_TASTE; i++) {
    const tkey *k = &TASTE[i];
    if(!(t->set & k->bit))
      continue;
    put(w, k->name);
    put(w, " = ");
    if(k->kind == K_NUM)
      put_num(w, *num_field((ft_taste *)t, k->bit));
    else if(k->kind == K_TRIPLE)
      put_nums(w, t->tint, 3);
    else
      put(w, t->curve[curve_slot(k->bit)]);
    put(w, "\n");
  }
}

static void section(wbuf *w, const char *head) {
  if(w->n)
    put(w, "\n");
  put(w, head);
}

size_t ft_edit_write(const ft_edit *ed, ft_look_kind k, char *buf, size_t cap) {
  wbuf w;
  int i;
  w.b = buf;
  w.cap = cap;
  w.n = 0;
  if(cap)
    buf[0] = 0;
  if(k == FT_EDIT_FILE && ed->from[0]) {
    put(&w, "from = ");
    put(&w, ed->from);
    put(&w, "\n");
  }
  if(ed->stock.present) {
    const ft_stock *s = &ed->stock;
    section(&w, "[stock]\nstock_version = ");
    put_num(&w, s->version);
    put(&w, "\n");
    if(s->has_ratio) {
      put(&w, "range_ratio = ");
      put_nums(&w, s->range_ratio, 3);
      put(&w, "\n");
    }
    if(s->has_bw) {
      put(&w, "bw_weights = ");
      put_nums(&w, s->bw_weights, 3);
      put(&w, "\n");
    }
    if(s->n_frames) {
      put(&w, "n_frames = ");
      put_num(&w, (double)s->n_frames);
      put(&w, "\n");
    }
  }
  section(&w, "[taste]\n");
  put_taste(&w, &ed->taste);
  if(k == FT_EDIT_FILE)
    for(i = 0; i < ed->nframes; i++) {
      char head[32];
      wbuf h;
      h.b = head;
      h.cap = sizeof head;
      h.n = 0;
      put(&h, "[frame ");
      put_num(&h, ed->f[i].index);
      put(&h, "]\n");
      section(&w, head);
      put_taste(&w, &ed->f[i].o);
    }
  return w.n;
}

/* --- edit operations --- */

/* the taste of a scope; NULL (e filled) if the frame cannot be added */
static ft_taste *scope(ft_edit *ed, int frame, int create, ft_err *e) {
  int i;
  if(frame == FT_EDIT_ROLL)
    return &ed->taste;
  if(frame < 0 || frame > 9999) {
    ft_fail(e, "frame %d: not a frame number (0..9999)", frame);
    return NULL;
  }
  i = create ? frame_slot(ed, frame) : frame_find(ed, frame);
  if(i < 0) {
    if(create)
      ft_fail(e, "frame %d: more than %d frames", frame, FT_MAX_FRAMES);
    return NULL;
  }
  return &ed->f[i].o;
}

int ft_edit_set(
    ft_edit *ed, int frame, const char *key, const char *value, ft_err *e
) {
  ft_taste tmp, *t;
  if(frame != FT_EDIT_ROLL && (frame < 0 || frame > 9999))
    return ft_fail(e, "frame %d: not a frame number (0..9999)", frame);
  /* validate before a frame slot is created */
  memset(&tmp, 0, sizeof tmp);
  if(ft_taste_set(&tmp, key, value, e))
    return -1;
  t = scope(ed, frame, 1, e);
  if(!t)
    return -1;
  return ft_taste_set(t, key, value, e);
}

static void drop_frame(ft_edit *ed, int slot) {
  int i;
  for(i = slot; i + 1 < ed->nframes; i++)
    ed->f[i] = ed->f[i + 1];
  ed->nframes--;
}

int ft_edit_clear(ft_edit *ed, int frame, const char *key, ft_err *e) {
  unsigned bit = ~0u;
  ft_taste *t;
  if(key) {
    int i = taste_index(key);
    if(i < 0)
      return ft_fail(e, "%.40s: unknown key", key);
    bit = TASTE[i].bit;
  }
  if(frame != FT_EDIT_ROLL && (frame < 0 || frame > 9999))
    return ft_fail(e, "frame %d: not a frame number (0..9999)", frame);
  t = scope(ed, frame, 0, e);
  if(!t)
    return 0; /* a frame with nothing to clear */
  t->set &= ~bit;
  if(frame != FT_EDIT_ROLL && t->set == 0)
    drop_frame(ed, frame_find(ed, frame));
  return 0;
}

int ft_edit_copy(ft_edit *ed, int src, int dst, ft_err *e) {
  int s, d;
  if(src < 0 || src > 9999 || dst < 0 || dst > 9999)
    return ft_fail(e, "copy: frames are numbers 0..9999");
  if(src == dst)
    return 0;
  s = frame_find(ed, src);
  if(s < 0) {
    d = frame_find(ed, dst);
    if(d >= 0)
      drop_frame(ed, d);
    return 0;
  }
  d = frame_slot(ed, dst);
  if(d < 0)
    return ft_fail(e, "frame %d: more than %d frames", dst, FT_MAX_FRAMES);
  s = frame_find(ed, src); /* the insert may have moved it */
  ed->f[d].o = ed->f[s].o;
  return 0;
}

void ft_edit_apply_look(ft_edit *ed, const ft_edit *look, const char *name) {
  size_t n = name ? strlen(name) : 0;
  if(n >= FT_LOOK_NAME)
    n = FT_LOOK_NAME - 1;
  ed->stock = look->stock;
  ed->taste = look->taste;
  memcpy(ed->from, name ? name : "", n);
  ed->from[n] = 0;
}

/* --- resolving --- */

void ft_edit_taste(
    const ft_edit *ed, int frame, ft_taste *out, unsigned *from_frame
) {
  int s = frame == FT_EDIT_ROLL ? -1 : frame_find(ed, frame), i;
  *out = ed->taste;
  if(from_frame)
    *from_frame = 0;
  if(s < 0)
    return;
  for(i = 0; i < N_TASTE; i++) {
    const ft_taste *o = &ed->f[s].o;
    unsigned b = TASTE[i].bit;
    if(!(o->set & b))
      continue;
    if(TASTE[i].kind == K_NUM)
      *num_field(out, b) = *num_field((ft_taste *)o, b);
    else if(TASTE[i].kind == K_TRIPLE)
      memcpy(out->tint, o->tint, sizeof out->tint);
    else
      memcpy(
          out->curve[curve_slot(b)], o->curve[curve_slot(b)], FT_LOOK_CURVE_TXT
      );
    out->set |= b;
    if(from_frame)
      *from_frame |= b;
  }
}

void ft_look_detail_of(const ft_taste *t, ft_look_detail *d) {
  d->nr_sensor = t && (t->set & FT_T_NR_SENSOR) ? t->nr_sensor : DEF_NR_SENSOR;
  d->nr_chroma = t && (t->set & FT_T_NR_CHROMA) ? t->nr_chroma : DEF_NR_CHROMA;
  d->nr_grain = t && (t->set & FT_T_NR_GRAIN) ? t->nr_grain : DEF_NR_GRAIN;
  d->sharpen = t && (t->set & FT_T_SHARPEN) ? t->sharpen : DEF_SHARPEN;
}

static float clampf(double v, double lo, double hi) {
  return (float)(v < lo ? lo : v > hi ? hi : v);
}

int ft_look_recipe(
    const ft_stock *s, const ft_taste *t, ft_render_mode m, const float *base,
    const ft_image *const *im, int n, ft_recipe *r, ft_err *e
) {
  float d[3];
  int c;
  ft_recipe_default(r, m);
  if(s && s->has_bw && m == FT_RENDER_BW)
    for(c = 0; c < 3; c++)
      r->bw_weights[c] = (float)s->bw_weights[c];
  /* absolute fields first: the auto exposure depends on grade and
   * paper */
  if(t) {
    if(t->set & FT_T_GRADE)
      r->grade = (float)t->grade;
    if(t->set & FT_T_PAPER_BLACK)
      r->paper_black = (float)t->paper_black;
    if(t->set & FT_T_PAPER_WHITE)
      r->paper_white = (float)t->paper_white;
    for(c = 0; c < 4; c++)
      if((t->set & (FT_T_CURVE << c)) &&
         curve_of(
             TASTE[5 + c].name, t->curve[c], c ? &r->ch[c - 1] : &r->master, e
         ))
        return -1;
  }
  if(base)
    memcpy(d, base, sizeof d);
  else if(ft_render_base_from_frames(im, n, m, d, e))
    return -1;
  ft_recipe_set_base(r, d);
  if(ft_render_auto(im, n, r, e))
    return -1;
  /* the stock's channel balance; green (and so the exposure) stays */
  if(s && s->has_ratio && m == FT_RENDER_NEG) {
    r->range[0] = clampf(r->range[1] * s->range_ratio[0], 0.05, 4.5);
    r->range[2] = clampf(r->range[1] * s->range_ratio[2], 0.05, 4.5);
  }
  if(t && (t->set & FT_T_EXPOSURE))
    r->exposure = clampf(r->exposure + t->exposure, -6, 6);
  /* b&w has one channel: a look's tint is ignored, not an error */
  if(t && (t->set & FT_T_TINT) && m != FT_RENDER_BW)
    for(c = 0; c < 3; c++)
      r->tint[c] = (float)t->tint[c];
  return ft_recipe_validate(r, e);
}

/* --- learning --- */

static double median(double *v, int n) {
  int i, j;
  for(i = 1; i < n; i++) { /* insertion sort */
    double x = v[i];
    for(j = i; j > 0 && v[j - 1] > x; j--)
      v[j] = v[j - 1];
    v[j] = x;
  }
  return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

int ft_stock_learn(
    const double (*ratio)[3], int n, const ft_stock *old, ft_stock *out,
    double *spread, ft_err *e
) {
  double v[FT_MAX_FRAMES], med[3] = {1, 1, 1}, sp = 0;
  int i, c;
  if(n < 1)
    return ft_fail(e, "learn: no frames");
  if(n > FT_MAX_FRAMES)
    return ft_fail(e, "learn: more than %d frames", FT_MAX_FRAMES);
  for(i = 0; i < n; i++)
    for(c = 0; c < 3; c += 2)
      if(!(ratio[i][c] >= 0.2 && ratio[i][c] <= 5))
        return ft_fail(
            e,
            "learn: frame %d: ratio %g out of range "
            "(0.2..5)",
            i + 1, ratio[i][c]
        );
  for(c = 0; c < 3; c += 2) {
    double lo, hi;
    for(i = 0; i < n; i++)
      v[i] = ratio[i][c];
    med[c] = median(v, n);
    lo = v[0];
    hi = v[n - 1];
    if(hi - lo > sp)
      sp = hi - lo;
  }
  if(old)
    *out = *old;
  else
    memset(out, 0, sizeof *out);
  out->present = 1;
  out->version = FT_STOCK_VERSION;
  if(old && old->has_ratio && old->n_frames > 0) {
    double no = (double)old->n_frames;
    for(c = 0; c < 3; c += 2)
      out->range_ratio[c] = (no * old->range_ratio[c] + n * med[c]) / (no + n);
    out->n_frames = old->n_frames + n;
  } else {
    out->range_ratio[0] = med[0];
    out->range_ratio[2] = med[2];
    out->n_frames = n;
  }
  out->range_ratio[1] = 1;
  out->has_ratio = 1;
  if(spread)
    *spread = sp;
  return 0;
}

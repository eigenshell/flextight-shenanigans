/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ft_detect.h"
#include "ft_geom.h"
#include "ft_holder.h"
#include "ft_noise_model.h"
#include "ft_preview.h"
#include "ft_share_cut.h"

static const char *TYPES[] = { "full", "half", "panorama", "partial" };

static int type_of(const char *s, ft_frame_type *t)
{
    int i;
    for (i = 0; i < 4; i++)
        if (strcmp(s, TYPES[i]) == 0) {
            *t = (ft_frame_type)i;
            return 0;
        }
    return -1;
}

/* next "key=value" line of text[0..n): key and value copied (truncated
 * to their buffers); returns the offset after the line, n at the end */
static size_t next_kv(const char *text, size_t n, size_t at, char *k,
                      size_t kc, char *v, size_t vc)
{
    size_t e = at, i = 0, j = 0, p;
    while (e < n && text[e] != '\n')
        e++;
    k[0] = v[0] = 0;
    for (p = at; p < e && text[p] != '='; p++)
        if (i + 1 < kc)
            k[i++] = text[p];
    k[i] = 0;
    if (p < e)
        for (p++; p < e && text[p] != '\r'; p++)
            if (j + 1 < vc)
                v[j++] = text[p];
    v[j] = 0;
    return e < n ? e + 1 : n;
}

static int fnum(const char *v, double *out)
{
    char *end;
    *out = strtod(v, &end);
    return end != v && *end == 0 && isfinite(*out) ? 0 : -1;
}

static int lnum(const char *v, long *out)
{
    char *end;
    *out = strtol(v, &end, 10);
    return end != v && *end == 0 ? 0 : -1;
}

int ft_share_meta_parse(const char *text, size_t n, ft_share_meta *m,
                        ft_err *e)
{
    char k[32], v[160];
    size_t at = 0;
    unsigned seen = 0;          /* ppi px x y bin share */
    memset(m, 0, sizeof *m);
    m->film = FT_FILM_NEGATIVE;
    strcpy(m->holder, "24x217");
    while (at < n) {
        long l;
        int bad = 0;
        at = next_kv(text, n, at, k, sizeof k, v, sizeof v);
        if (!k[0])
            continue;
        if (strcmp(k, "ppi") == 0) {
            bad = fnum(v, &m->ppi) || !(m->ppi >= 525 && m->ppi <= 6300);
            seen |= 1;
        } else if (strcmp(k, "px") == 0) {
            bad = lnum(v, &l) || l < 1 || l > 20000;
            m->px = (int)l;
            seen |= 2;
        } else if (strcmp(k, "x") == 0) {
            bad = lnum(v, &m->x) || m->x < 0;
            seen |= 4;
        } else if (strcmp(k, "y") == 0) {
            bad = lnum(v, &m->y) || m->y < 0;
            seen |= 8;
        } else if (strcmp(k, "bin") == 0) {
            bad = lnum(v, &l) || l < 1 || l > 12 || 12 % l;
            m->bin = (int)l;
            seen |= 16;
        } else if (strcmp(k, "share") == 0) {
            bad = fnum(v, &m->share) || !(m->share >= 100 && m->share <= 20000);
            seen |= 32;
        } else if (strcmp(k, "hf_half_res") == 0) {
            bad = lnum(v, &l) || (l != 0 && l != 1);
            m->hf_half_res = (int)l;
        } else if (strcmp(k, "film") == 0) {
            if (strcmp(v, "negative") == 0) m->film = FT_FILM_NEGATIVE;
            else if (strcmp(v, "slide") == 0) m->film = FT_FILM_SLIDE;
            else bad = 1;
        } else if (strcmp(k, "holder") == 0) {
            bad = !v[0] || strlen(v) >= sizeof m->holder;
            if (!bad)
                strcpy(m->holder, v);
        } else if (strcmp(k, "film_base") == 0) {
            double *b = m->film_base;
            char extra;
            bad = sscanf(v, "%lf %lf %lf %c", &b[0], &b[1], &b[2], &extra) != 3
                  || !(b[0] >= 0 && b[0] <= 4.5 && b[1] >= 0 && b[1] <= 4.5 &&
                       b[2] >= 0 && b[2] <= 4.5);
            m->has_film_base = !bad;
        } else if (strcmp(k, "share_frame") == 0) {
            ft_share_frame *f = &m->f[m->nframes];
            char tn[16], extra;
            int idx, j;
            if (m->nframes == FT_MAX_FRAMES)
                return ft_fail(e, "share meta: more than %d share_frame",
                               FT_MAX_FRAMES);
            bad = sscanf(v, "%d %15s %lf %lf %lf %lf %c", &idx, tn, &f->y0,
                         &f->len, &f->x0, &f->width, &extra) != 6 ||
                  type_of(tn, &f->type) || idx < 1 || !(f->len > 0) ||
                  !(f->width > 0) || !isfinite(f->y0) || !isfinite(f->x0);
            f->index = idx;
            if (!bad) {
                for (j = 0; j < m->nframes; j++)
                    if (m->f[j].index == idx)
                        return ft_fail(e, "share meta: frame %d listed twice",
                                       idx);
                m->nframes++;
            }
        }
        if (bad)
            return ft_fail(e, "share meta: bad %s=%s", k, v);
    }
    if ((seen & 63) != 63)
        return ft_fail(e, "share meta: missing %s",
                       !(seen & 1) ? "ppi" : !(seen & 2) ? "px" :
                       !(seen & 4) ? "x" : !(seen & 8) ? "y" :
                       !(seen & 16) ? "bin" : "share");
    if (!m->nframes)
        return ft_fail(e, "share meta: no share_frame");
    if (!ft_holder_find(m->holder))
        return ft_fail(e, "share meta: unknown holder %s", m->holder);
    return 0;
}

/* the detected frame overlapping p most along, if it is p: overlap of at
 * least half the shorter one, length within 25 %, shift within the
 * margin (more than that: creep we did not scan for) */
static int match(const ft_layout *L, const ft_share_frame *p,
                 const unsigned char *used)
{
    int i, best = -1;
    double bo = 0;
    for (i = 0; i < L->nframes; i++) {
        const ft_det_frame *d = &L->f[i];
        double len = d->y1 - d->y0;
        double o = (d->y1 < p->y0 + p->len ? d->y1 : p->y0 + p->len) -
                   (d->y0 > p->y0 ? d->y0 : p->y0);
        if (used[i] || o <= bo)
            continue;
        if (o < 0.5 * (len < p->len ? len : p->len) ||
            fabs(len - p->len) > 0.25 * p->len ||
            fabs(d->y0 - p->y0) > FT_SHARE_MARGIN_MM)
            continue;
        bo = o;
        best = i;
    }
    return best;
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static long round_l(double v)
{
    return (long)floor(v + 0.5);
}

int ft_share_cut(const unsigned short *raw, int lines,
                 const unsigned short *cal, int cal_lines,
                 const char *faul, size_t faul_n, const ft_defects *d,
                 const ft_share_meta *m, ft_share_cut_result *r, ft_err *e)
{
    ft_layout L;
    unsigned char used[FT_MAX_FRAMES];
    double shifts[FT_MAX_FRAMES], med = 0, scale = m->ppi * m->bin;
    double y0[FT_MAX_FRAMES], y1[FT_MAX_FRAMES], x0[FT_MAX_FRAMES],
           x1[FT_MAX_FRAMES];
    int found[FT_MAX_FRAMES], nshift = 0, i, nl, f, k, rc = -1;
    float *t, *ds = NULL;
    ft_develop_opts o;
    ft_axes ax;
    ft_share_opts so;

    memset(r, 0, sizeof *r);
    memset(&o, 0, sizeof o);
    o.x = (int)m->x;
    o.bin = m->bin;
    o.defects = d;
    t = ft_develop_t(raw, lines, m->px, cal, cal_lines, faul, faul_n, &o, &nl,
                     e);
    if (!t)
        return -1;
    k = 12 / m->bin;
    /* one NoiseProfile for every frame of the window (same .cal); none
     * is not an error, export estimates */
    r->noise_rc = ft_noise_model(cal, cal_lines, m->px, r->noise,
                                 &r->noise_err);

    /* detection on a ~500 ppi copy, in holder mm */
    f = (int)floor(m->ppi / 500 + 0.5);
    if (f < 1)
        f = 1;
    ax.y_mm_per_line = 25.4 / m->ppi * f;
    ax.y_mm0 = (m->y + k + (f - 1) / 2.0) * 25.4 / m->ppi;
    {
        double half = (f * m->bin - 1) / 2.0;
        ax.x_mm0 = ft_ccd_to_mm(ft_out_to_ccd(m->x, 0, m->bin) + half, scale);
        ax.x_mm_per_px = ft_ccd_to_mm(ft_out_to_ccd(m->x, f, m->bin) + half,
                                      scale) - ax.x_mm0;
    }
    r->detect_ok = 0;
    if (nl / f >= 8 && m->px / f >= 8 &&
        (ds = malloc((size_t)(nl / f) * (m->px / f) * 3 * sizeof *ds)) != NULL) {
        ft_err de;
        ft_box_down(t, nl, m->px, f, ds);
        r->detect_ok = ft_detect(ds, nl / f, m->px / f, &ax, m->film,
                                 ft_holder_find(m->holder), &L, &de) == 0;
    }
    free(ds);
    if (!r->detect_ok)
        L.nframes = 0;

    memset(used, 0, sizeof used);
    for (i = 0; i < m->nframes; i++) {
        const ft_share_frame *p = &m->f[i];
        int j = match(&L, p, used);
        found[i] = j >= 0;
        if (j >= 0) {
            used[j] = 1;
            y0[i] = L.f[j].y0;
            y1[i] = L.f[j].y1;
            x0[i] = L.f[j].x0;
            x1[i] = L.f[j].x1;
            shifts[nshift++] = y0[i] - p->y0;
        }
    }
    if (nshift) {
        qsort(shifts, (size_t)nshift, sizeof shifts[0], cmp_d);
        med = nshift % 2 ? shifts[nshift / 2]
                         : 0.5 * (shifts[nshift / 2 - 1] + shifts[nshift / 2]);
    }
    ft_share_opts_default(&so, 0);
    so.target_px = m->share;
    so.hf_half_res = m->hf_half_res;

    for (i = 0; i < m->nframes; i++) {
        const ft_share_frame *p = &m->f[i];
        ft_share_cut_frame *c = &r->f[r->n];
        ft_develop_opts co;
        ft_share_frame s;
        long ra, rb, ca, cb, fa, fb, ga, gb;
        double bl = FT_SHARE_BLEED_MM;
        if (!found[i]) {
            y0[i] = p->y0 + med;
            y1[i] = p->y0 + p->len + med;
            x0[i] = p->x0;
            x1[i] = p->x0 + p->width;
        }
        c->index = p->index;
        c->type = p->type;
        c->estimated = !found[i];
        c->shift_mm = y0[i] - p->y0;
        /* holder mm -> aligned rows (along) and columns (across); the
         * same mapping as ftpoc's write_crop */
        fa = round_l(y0[i] / 25.4 * m->ppi) - m->y - k;
        fb = round_l(y1[i] / 25.4 * m->ppi) - m->y - k;
        ga = round_l((ft_mm_to_ccd(x0[i], scale) - FT_CCD_FIRST) / m->bin)
             - m->x;
        gb = round_l((ft_mm_to_ccd(x1[i], scale) - FT_CCD_FIRST) / m->bin)
             - m->x;
        ra = round_l((y0[i] - bl) / 25.4 * m->ppi) - m->y - k;
        rb = round_l((y1[i] + bl) / 25.4 * m->ppi) - m->y - k;
        ca = round_l((ft_mm_to_ccd(x0[i] - bl, scale) - FT_CCD_FIRST) / m->bin)
             - m->x;
        cb = round_l((ft_mm_to_ccd(x1[i] + bl, scale) - FT_CCD_FIRST) / m->bin)
             - m->x;
        if (ra < 0) ra = 0;
        if (ca < 0) ca = 0;
        if (rb > nl) rb = nl;
        if (cb > m->px) cb = m->px;
        if (fa < ra) fa = ra;
        if (ga < ca) ga = ca;
        if (fb > rb) fb = rb;
        if (gb > cb) gb = cb;
        if (fb - fa < 8 || gb - ga < 8) {
            ft_fail(e, "share: frame %d is outside the scanned window",
                    p->index);
            goto out;
        }
        memset(&co, 0, sizeof co);
        co.crop_x = (int)ca;
        co.crop_y = (int)ra;
        co.crop_w = (int)(cb - ca);
        co.crop_h = (int)(rb - ra);
        ft_image_init(&c->im);
        if (ft_crop_orient(t, nl, m->px, &co, &c->im, e))
            goto out;
        r->n++;
        c->im.ppi = m->ppi;
        c->im.film = m->film;
        if (r->noise_rc >= 0) {
            c->im.has_noise = 1;
            memcpy(c->im.noise, r->noise, sizeof c->im.noise);
        }
        c->im.frame = p->index;
        snprintf(c->im.desc, sizeof c->im.desc,
                 "share frame %d, %g ppi, bin %d%s", p->index, m->ppi, m->bin,
                 c->estimated ? ", box estimated" : "");
        /* image x = along (columns), y = across (rows) */
        c->box[0] = (int)(fa - ra);
        c->box[1] = (int)(ga - ca);
        c->box[2] = (int)(fb - fa);
        c->box[3] = (int)(gb - ga);
        s = *p;
        s.len = y1[i] - y0[i];
        s.width = x1[i] - x0[i];
        {
            double lng = c->box[2] > c->box[3] ? c->box[2] : c->box[3];
            double tg = ft_share_target_px(&s, &so);
            c->export_scale = tg < lng ? tg / lng : 1.0;
        }
    }
    rc = 0;
out:
    free(t);
    if (rc)
        ft_share_cut_free(r);
    return rc;
}

void ft_share_cut_free(ft_share_cut_result *r)
{
    int i;
    for (i = 0; i < r->n; i++)
        ft_image_free(&r->f[i].im);
    r->n = 0;
}

int ft_share_sidecar_write_buf(char *buf, size_t cap,
                               const ft_share_meta *m,
                               const ft_share_cut_frame *c)
{
    size_t n = 0;
    int w;
#define APP(...) do { \
        w = snprintf(buf ? buf + (n < cap ? n : cap) : NULL, \
                     n < cap ? cap - n : 0, __VA_ARGS__); \
        if (w > 0) \
            n += (size_t)w; \
    } while (0)
    APP("frame=%d\ntype=%s\nfilm=%s\nppi=%g\nshare=%g\nhf_half_res=%d\n",
        c->index, (unsigned)c->type <= 3 ? TYPES[c->type] : "full",
        m->film == FT_FILM_SLIDE ? "slide" : "negative", m->ppi, m->share,
        m->hf_half_res);
    if (m->has_film_base)
        APP("film_base=%.4f %.4f %.4f\n", m->film_base[0], m->film_base[1],
            m->film_base[2]);
    if (c->im.has_noise)
        APP("noise_profile=%.17g %.17g %.17g %.17g %.17g %.17g\n",
            c->im.noise[0], c->im.noise[1], c->im.noise[2], c->im.noise[3],
            c->im.noise[4], c->im.noise[5]);
    APP("box=%d %d %d %d\nexport_scale=%.6f\nestimated=%d\nshift_mm=%.3f\n",
        c->box[0], c->box[1], c->box[2], c->box[3], c->export_scale,
        c->estimated, c->shift_mm);
#undef APP
    return (int)n;
}

int ft_share_sidecar_parse(const char *text, size_t n, ft_share_sidecar *s,
                           ft_err *e)
{
    char k[32], v[160];
    size_t at = 0;
    int has_box = 0, has_scale = 0;
    memset(s, 0, sizeof *s);
    s->film = FT_FILM_NEGATIVE;
    while (at < n) {
        long l;
        int bad = 0;
        at = next_kv(text, n, at, k, sizeof k, v, sizeof v);
        if (!k[0])
            continue;
        if (strcmp(k, "frame") == 0) {
            bad = lnum(v, &l) || l < 0;
            s->frame = (int)l;
        } else if (strcmp(k, "type") == 0) {
            bad = type_of(v, &s->type);
        } else if (strcmp(k, "film") == 0) {
            if (strcmp(v, "negative") == 0) s->film = FT_FILM_NEGATIVE;
            else if (strcmp(v, "slide") == 0) s->film = FT_FILM_SLIDE;
            else bad = 1;
        } else if (strcmp(k, "ppi") == 0) {
            bad = fnum(v, &s->ppi) || !(s->ppi > 0);
        } else if (strcmp(k, "share") == 0) {
            bad = fnum(v, &s->share) || !(s->share >= 0);
        } else if (strcmp(k, "hf_half_res") == 0) {
            bad = lnum(v, &l) || (l != 0 && l != 1);
            s->hf_half_res = (int)l;
        } else if (strcmp(k, "estimated") == 0) {
            bad = lnum(v, &l) || (l != 0 && l != 1);
            s->estimated = (int)l;
        } else if (strcmp(k, "shift_mm") == 0) {
            bad = fnum(v, &s->shift_mm);
        } else if (strcmp(k, "export_scale") == 0) {
            bad = fnum(v, &s->export_scale) ||
                  !(s->export_scale > 0 && s->export_scale <= 1);
            has_scale = !bad;
        } else if (strcmp(k, "box") == 0) {
            int *b = s->box;
            char extra;
            bad = sscanf(v, "%d %d %d %d %c", &b[0], &b[1], &b[2], &b[3],
                         &extra) != 4 || b[0] < 0 || b[1] < 0 || b[2] < 1 ||
                  b[3] < 1;
            has_box = !bad;
        } else if (strcmp(k, "film_base") == 0) {
            double *b = s->film_base;
            char extra;
            bad = sscanf(v, "%lf %lf %lf %c", &b[0], &b[1], &b[2], &extra) != 3
                  || !(b[0] >= 0 && b[0] <= 4.5 && b[1] >= 0 && b[1] <= 4.5 &&
                       b[2] >= 0 && b[2] <= 4.5);
            s->has_film_base = !bad;
        } else if (strcmp(k, "noise_profile") == 0) {
            double *p = s->noise;
            char extra;
            int i;
            bad = sscanf(v, "%lf %lf %lf %lf %lf %lf %c", &p[0], &p[1], &p[2],
                         &p[3], &p[4], &p[5], &extra) != 6;
            s->has_noise = !bad;
            for (i = 0; i < 6 && !bad; i++)
                if (!isfinite(p[i]) || p[i] < 0 || (i % 2 && p[i] == 0))
                    s->has_noise = 0;
        }
        if (bad)
            return ft_fail(e, "sidecar: bad %s=%s", k, v);
    }
    if (!has_box)
        return ft_fail(e, "sidecar: missing box");
    if (!has_scale)
        return ft_fail(e, "sidecar: missing export_scale");
    return 0;
}

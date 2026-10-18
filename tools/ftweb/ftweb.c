/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ftweb.h. Browser tier: heap and snprintf, no files. The steps follow
 * tools/ftdev (cut: share.c ftdev_cut; print: print.c ftdev_print) call
 * for call, with the files replaced by what they would hold. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ft_develop.h"
#include "ft_dng.h"
#include "ft_emulsion.h"
#include "ft_focus.h"
#include "ft_holder.h"
#include "ft_job.h"
#include "ft_output.h"
#include "ft_plan.h"
#include "ft_pm.h"
#include "ft_print.h"
#include "ft_preview.h"
#include "ft_print_file.h"
#include "ft_render_auto.h"
#include "ft_resize.h"
#include "ft_share.h"
#include "ft_share_cut.h"
#include "ft_share_print.h"
#include "ft_zip.h"
#include "ftweb.h"

/* a landed frame, as ftdev cut leaves it on disk: the DNG's samples,
 * ppi and noise profile, and the sidecar as read back */
typedef struct {
    int frame, w, h;
    unsigned short *q;
    double ppi;
    int has_noise;
    double noise[6];
    ft_share_sidecar sc;
    ft_image proxy;             /* the stats proxy (ftdev print's) */
} wframe;

/* a frame of the preview (strip.raw): its box cut from the developed
 * preview, x along, as a share frame is */
typedef struct {
    int frame;
    ft_image im;
    ft_image proxy;
} pframe;

struct ftw {
    ft_lens lens;
    ft_job job;                 /* the preview's: every plan's base */
    int has_job;
    ft_job plan;                /* strip.job as planned */
    int has_plan;
    ft_print_file pf;
    int has_print;
    int n;
    wframe f[FT_MAX_FRAMES];    /* by frame number */
    int measured;
    ft_print_roll roll;
    ft_print_frame pfr[FT_MAX_FRAMES];
    int npv;                    /* the preview's frames (ftw_preview_load) */
    pframe pv[FT_MAX_FRAMES];
    int pv_measured;
    ft_print_roll pv_roll;
    ft_print_frame pv_pfr[FT_MAX_FRAMES];
    int disp_frame;             /* the cached screen copy's frame, 0 none */
    int disp_edge;              /* and the long edge it was asked at */
    ft_image disp;
    unsigned char *out;
    size_t nout, cap;
    char text[FT_SHARE_TEXT_MAX];
    char err[sizeof(((ft_err *)0)->msg)];
};

static const char *TYPES[] = { "full", "half", "panorama", "partial" };

static int fail(ftw *w, const char *msg)
{
    snprintf(w->err, sizeof w->err, "%s", msg);
    return -1;
}

static int fail_e(ftw *w, const ft_err *e)
{
    return fail(w, e->msg);
}

/* ------------------------------------------------------------ out */

static void out_reset(ftw *w)
{
    w->nout = 0;
    if (w->out)
        w->out[0] = 0;
}

/* take ownership of b as the out buffer */
static void out_take(ftw *w, unsigned char *b, size_t n)
{
    free(w->out);
    w->out = b;
    w->nout = n;
    w->cap = n;
}

static int out_room(ftw *w, size_t more)
{
    size_t need = w->nout + more + 1, c = w->cap ? w->cap : 1024;
    unsigned char *nb;
    if (need <= w->cap)
        return 0;
    while (c < need)
        c *= 2;
    nb = realloc(w->out, c);
    if (!nb)
        return -1;
    w->out = nb;
    w->cap = c;
    return 0;
}

#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
static int outf(ftw *w, const char *fmt, ...)
{
    va_list ap;
    int k;
    va_start(ap, fmt);
    k = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (k < 0 || out_room(w, (size_t)k))
        return fail(w, "out of memory");
    va_start(ap, fmt);
    vsnprintf((char *)w->out + w->nout, (size_t)k + 1, fmt, ap);
    va_end(ap);
    w->nout += (size_t)k;
    return 0;
}

/* a JSON string's body (ids, names and messages: no control bytes but
 * newlines expected) */
static int out_str(ftw *w, const char *s)
{
    int rc = outf(w, "\"");
    for (; *s && !rc; s++)
        rc = *s == '"' || *s == '\\' ? outf(w, "\\%c", *s)
             : (unsigned char)*s < 32 ? outf(w, "\\u%04x", *s)
             : outf(w, "%c", *s);
    return rc ? rc : outf(w, "\"");
}

/* ------------------------------------------------------------ life */

ftw *ftw_new(void)
{
    ftw *w = calloc(1, sizeof *w);
    if (w) {
        w->lens = ft_lens_default;
        ft_image_init(&w->disp);
    }
    return w;
}

static void frame_free(wframe *f)
{
    free(f->q);
    ft_image_free(&f->proxy);
    memset(f, 0, sizeof *f);
}

static void preview_free(ftw *w)
{
    int i;
    for (i = 0; i < w->npv; i++) {
        ft_image_free(&w->pv[i].im);
        ft_image_free(&w->pv[i].proxy);
    }
    w->npv = 0;
    w->pv_measured = 0;
}

void ftw_free(ftw *w)
{
    int i;
    if (!w)
        return;
    for (i = 0; i < w->n; i++)
        frame_free(&w->f[i]);
    preview_free(w);
    ft_image_free(&w->disp);
    free(w->out);
    free(w);
}

const char *ftw_error(const ftw *w)
{
    return w->err;
}

const unsigned char *ftw_out(const ftw *w)
{
    return w->out;
}

size_t ftw_out_len(const ftw *w)
{
    return w->nout;
}

const char *ftw_text(const ftw *w)
{
    return w->text;
}

void *ftw_alloc(size_t n)
{
    return malloc(n ? n : 1);
}

void ftw_release(void *p)
{
    free(p);
}

int ftw_emulsions(ftw *w)
{
    static const char *MODE[3] = { "negative", "slide", "bw" };
    const ft_emulsion *em;
    int i, rc;
    out_reset(w);
    rc = outf(w, "[");
    for (i = 0; !rc && (em = ft_emulsion_at(i)) != NULL; i++) {
        rc = outf(w, "%s{\"id\":", i ? "," : "");
        rc = rc ? rc : out_str(w, em->id);
        rc = rc ? rc : outf(w, ",\"name\":");
        rc = rc ? rc : out_str(w, em->name);
        rc = rc ? rc : outf(w, ",\"mode\":\"%s\"}", MODE[em->mode]);
    }
    return rc ? rc : outf(w, "]");
}

int ftw_resolutions(ftw *w, const double *ppi, int n)
{
    int i, bins[6], rc;
    for (i = 0; i < n; i++)
        if (!ft_bins_for_ppi(ppi[i], bins)) {
            snprintf(w->err, sizeof w->err,
                     "%g ppi: out of the optics' range", ppi[i]);
            return -1;
        }
    out_reset(w);
    rc = outf(w, "[");
    for (i = 0; i < n && !rc; i++) {
        double in = FT_SHARE_FF_LONG_MM / 25.4, mp = ppi[i] * in *
                    ppi[i] * in * 24.0 / FT_SHARE_FF_LONG_MM / 1e6;
        rc = outf(w, "%s{\"ppi\":%g,\"mp\":%.1f,\"px\":%.0f,\"dmax\":null}",
                  i ? "," : "", ppi[i], mp, ppi[i] * in);
    }
    return rc ? rc : outf(w, "]");
}

int ftw_holders(ftw *w)
{
    const ft_holder *h;
    int i, rc;
    out_reset(w);
    rc = outf(w, "[");
    for (i = 0; !rc && (h = ft_holder_at(i)) != NULL; i++) {
        rc = outf(w, "%s{\"name\":", i ? "," : "");
        rc = rc ? rc : out_str(w, h->name);
        rc = rc ? rc : outf(w, ",\"label\":");
        rc = rc ? rc : out_str(w, h->label);
        rc = rc ? rc : outf(w, ",\"frames\":%d}", h->grid_frames);
    }
    return rc ? rc : outf(w, "]");
}

/* ------------------------------------------------------------ job */

int ftw_lens(ftw *w, const char *text, size_t n)
{
    ft_err e;
    return ft_lens_parse(&w->lens, text, n, &e) ? fail_e(w, &e) : 0;
}

static int job_parse(ftw *w, const char *text, size_t n, ft_job *to)
{
    static ft_job j;
    ft_err e;
    if (ft_job_parse(&j, text, n, &e))
        return fail_e(w, &e);
    *to = j;
    return 0;
}

int ftw_job_load(ftw *w, const char *text, size_t n)
{
    if (job_parse(w, text, n, &w->job))
        return -1;
    w->has_job = 1;
    w->has_plan = 0;
    return 0;
}

int ftw_plan_load(ftw *w, const char *text, size_t n)
{
    if (job_parse(w, text, n, &w->plan))
        return -1;
    w->has_plan = 1;
    return 0;
}

/* the planned job, else the base; NULL without either */
static const ft_job *job_now(const ftw *w)
{
    return w->has_plan ? &w->plan : w->has_job ? &w->job : NULL;
}

/* the "key=" line's value in the preview's metadata; -1 if none */
static long meta_key(const char *t, size_t n, const char *key)
{
    size_t at = 0, kl = strlen(key);
    long v = -1;
    while (t && at < n) {
        const char *s = t + at, *nl = memchr(s, '\n', n - at);
        size_t len = nl ? (size_t)(nl - s) : n - at;
        if (len > kl + 1 && len < kl + 24 && memcmp(s, key, kl) == 0 &&
            s[kl] == '=') {
            char b[24];
            memcpy(b, s + kl + 1, len - kl - 1);
            b[len - kl - 1] = 0;
            v = atol(b);
        }
        at += len + 1;
    }
    return v;
}

static long preview_x(const char *t, size_t n)
{
    return meta_key(t, n, "x");
}

int ftw_job_json(ftw *w, const char *preview_txt, size_t n)
{
    const ft_job *j = &w->job, *p = w->has_plan ? &w->plan : NULL;
    const ft_preview *pm = ft_preview_find("standard");
    long x = preview_x(preview_txt, n);
    int i, g, rc, groups[FT_MAX_FRAMES], ng = 0;
    ft_axes ax;
    if (!w->has_job)
        return fail(w, "no strip.job");
    ft_preview_axes(pm, x >= 0 ? x : FT_PREVIEW_X_OUT, &ax);
    for (i = 0; p && p->share > 0 && i < p->nframes; i++) {
        if (!p->f[i].include)
            continue;
        for (g = 0; g < ng && groups[g] != p->f[i].group; g++)
            ;
        if (g == ng)
            groups[ng++] = p->f[i].group;
    }
    out_reset(w);
    rc = outf(w, "{\"film\":\"%s\",\"holder\":",
              j->film == FT_FILM_SLIDE ? "slide" : "neg");
    rc = rc ? rc : out_str(w, j->holder);
    rc = rc ? rc : outf(w, ",\"windows\":%d,\"frames\":[", ng);
    for (i = 0; !rc && i < j->nframes; i++) {
        const ft_job_frame *f = &j->f[i], *c = NULL;
        double px = 0;
        for (g = 0; p && g < p->nframes; g++)
            if (p->f[g].index == f->index)
                c = &p->f[g];
        if (c && c->include && p->share > 0)
            px = ft_job_share_px(p, c);
        rc = outf(w, "%s{\"n\":%d,\"type\":\"%s\",\"low\":%s,"
                     "\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f,"
                     "\"px\":%g,\"group\":%d,\"done\":%s}",
                  i ? "," : "", f->index,
                  (unsigned)f->type <= 3 ? TYPES[f->type] : "full",
                  f->confidence_low ? "true" : "false",
                  (f->y0 - ax.y_mm0) / ax.y_mm_per_line,
                  (f->x0 - ax.x_mm0) / ax.x_mm_per_px,
                  f->len / ax.y_mm_per_line, f->width / ax.x_mm_per_px, px,
                  px > 0 ? c->group : 0, px > 0 && c->done ? "true"
                                                           : "false");
    }
    return rc ? rc : outf(w, "]}");
}

int ftw_plan(ftw *w, const double *px, int n)
{
    static ft_job j;
    const ft_holder *h;
    ft_err e;
    int ng;
    if (!w->has_job)
        return fail(w, "no strip.job");
    if (n != w->job.nframes)
        return fail(w, "plan: one size per frame of the job");
    j = w->job;
    h = ft_holder_find(j.holder);
    if (!h)
        return fail(w, "plan: unknown holder");
    ng = ft_share_plan_sizes(&j, px, j.hf_half_res, &w->lens,
                             h->scan_len_mm, &e);
    if (ng < 0 || ft_job_validate(&j, &w->lens, &e))
        return fail_e(w, &e);
    w->plan = j;
    w->has_plan = 1;
    return ng;
}

int ftw_job_text(ftw *w)
{
    const ft_job *j = job_now(w);
    size_t n;
    if (!j)
        return fail(w, "no strip.job");
    n = ft_job_write_buf(j, NULL, 0);
    out_reset(w);
    if (out_room(w, n))
        return fail(w, "out of memory");
    ft_job_write_buf(j, (char *)w->out, n + 1);
    w->nout = n;
    return 0;
}

/* ------------------------------------------------------------ print */

int ftw_print_load(ftw *w, const char *text, size_t n)
{
    static ft_print_file pf;
    ft_err e;
    if (ft_print_file_parse(text, n, &pf, &e))
        return fail_e(w, &e);
    w->pf = pf;
    w->has_print = 1;
    w->measured = 0;
    w->pv_measured = 0;
    w->disp_frame = 0;
    return 0;
}

int ftw_print_new(ftw *w, const char *emulsion)
{
    if (!ft_emulsion_find(emulsion))
        return fail(w, "not an emulsion");
    ft_print_file_init(&w->pf, emulsion);
    w->has_print = 1;
    w->measured = 0;
    w->pv_measured = 0;
    w->disp_frame = 0;
    return 0;
}

int ftw_print_emulsion(ftw *w, const char *emulsion)
{
    static ft_print_file pf;
    const ft_emulsion *em = ft_emulsion_find(emulsion);
    ft_err e;
    int i, dropped = 0;
    if (!w->has_print)
        return fail(w, "no strip.print");
    if (!em)
        return fail(w, "not an emulsion");
    ft_print_file_init(&pf, em->id);
    for (i = 0; i < w->pf.nframes; i++) {
        if (ft_print_clicks_ok(em, w->pf.f[i].k, &e) ||
            ft_print_file_set(&pf, w->pf.f[i].index, w->pf.f[i].k, &e))
            dropped++;
        /* the turn stays whatever the clicks do */
        ft_print_file_set_rotation(&pf, w->pf.f[i].index, w->pf.f[i].rotate,
                                   &e);
    }
    w->pf = pf;
    w->measured = 0;
    w->pv_measured = 0;
    out_reset(w);
    return outf(w, "%s", em->id) ? -1 : dropped;
}

int ftw_print_get(ftw *w, int frame, int *cmyd)
{
    ft_cmyd k;
    if (!w->has_print)
        return fail(w, "no strip.print");
    k = ft_print_file_clicks(&w->pf, frame);
    cmyd[0] = k.c;
    cmyd[1] = k.m;
    cmyd[2] = k.y;
    cmyd[3] = k.d;
    return 0;
}

int ftw_print_clicks(ftw *w, int frame, int c, int m, int y, int d)
{
    ft_cmyd k;
    ft_err e;
    k.c = c;
    k.m = m;
    k.y = y;
    k.d = d;
    if (!w->has_print)
        return fail(w, "no strip.print");
    if (ft_print_clicks_ok(ft_emulsion_find(w->pf.emulsion), k, &e) ||
        ft_print_file_set(&w->pf, frame, k, &e))
        return fail_e(w, &e);
    return 0;
}

int ftw_print_rotate(ftw *w, int frame, int deg)
{
    ft_err e;
    if (!w->has_print)
        return fail(w, "no strip.print");
    if (ft_print_file_set_rotation(&w->pf, frame, deg, &e))
        return fail_e(w, &e);
    return 0;
}

int ftw_print_name(ftw *w, int frame)
{
    if (!w->has_print)
        return fail(w, "no strip.print");
    out_reset(w);
    return outf(w, "%s", ft_print_file_name(&w->pf, frame));
}

int ftw_print_set_name(ftw *w, int frame, const char *name)
{
    ft_err e;
    if (!w->has_print)
        return fail(w, "no strip.print");
    if (ft_print_file_set_name(&w->pf, frame, name, &e))
        return fail_e(w, &e);
    return 0;
}

int ftw_print_rotation(ftw *w, int frame)
{
    if (!w->has_print)
        return fail(w, "no strip.print");
    return ft_print_file_rotation(&w->pf, frame);
}

int ftw_print_text(ftw *w)
{
    size_t n;
    if (!w->has_print)
        return fail(w, "no strip.print");
    n = ft_print_file_write(&w->pf, NULL, 0);
    out_reset(w);
    if (out_room(w, n))
        return fail(w, "out of memory");
    ft_print_file_write(&w->pf, (char *)w->out, n + 1);
    w->nout = n;
    return 0;
}

/* ------------------------------------------------------------ frames */

/* the frame's image as ft_dng_read gives the DNG back */
static int frame_image(const wframe *f, ft_image *im, ft_err *e)
{
    size_t k, n = (size_t)f->w * f->h * 3;
    ft_image_init(im);
    if (ft_image_alloc(im, f->w, f->h, e))
        return -1;
    for (k = 0; k < n; k++)
        im->rgb[k] = (float)f->q[k] / 65535.0f;
    im->ppi = f->ppi;
    im->has_noise = f->has_noise;
    memcpy(im->noise, f->noise, sizeof im->noise);
    return 0;
}

static int frame_at(const ftw *w, int frame)
{
    int i;
    for (i = 0; i < w->n; i++)
        if (w->f[i].frame == frame)
            return i;
    return -1;
}

/* a cut frame -> wf, as ftdev cut writes it and ftdev print reads it */
static int land(const ft_share_meta *m, const ft_share_cut_frame *c,
                wframe *wf, ft_err *e)
{
    char side[1024];
    ft_image hdr, im, cut;
    size_t k, n = (size_t)c->im.w * c->im.h * 3;
    int len;

    memset(wf, 0, sizeof *wf);
    ft_image_init(&wf->proxy);
    len = ft_share_sidecar_write_buf(side, sizeof side, m, c);
    if (len < 0 || len >= (int)sizeof side)
        return ft_fail(e, "sidecar too long");
    if (ft_share_sidecar_parse(side, (size_t)len, &wf->sc, e))
        return -1;
    wf->frame = wf->sc.frame > 0 ? wf->sc.frame : c->index;
    wf->w = c->im.w;
    wf->h = c->im.h;
    wf->q = malloc(n * sizeof *wf->q);
    if (!wf->q)
        return ft_fail(e, "out of memory");
    for (k = 0; k < n; k++)
        wf->q[k] = (unsigned short)ft_dng_q16(c->im.rgb[k]);
    hdr = c->im;                        /* ppi and noise as stored */
    hdr.rgb = NULL;
    ft_dng_as_stored(&hdr);
    wf->ppi = hdr.ppi;
    wf->has_noise = hdr.has_noise;
    memcpy(wf->noise, hdr.noise, sizeof wf->noise);
    /* the stats proxy: ftdev print's load_cut + ft_share_print_proxy */
    if (frame_image(wf, &im, e))
        goto bad;
    ft_image_init(&cut);
    if (ft_image_crop(&im, wf->sc.box, &cut, e)) {
        ft_image_free(&im);
        goto bad;
    }
    ft_image_free(&im);
    if (ft_share_print_proxy(&cut, &wf->proxy, e)) {
        ft_image_free(&cut);
        goto bad;
    }
    ft_image_free(&cut);
    return 0;
bad:
    frame_free(wf);
    return -1;
}

int ftw_cut(ftw *w, const unsigned short *raw, size_t raw_n,
            const unsigned short *cal, size_t cal_n, const char *faul,
            size_t faul_n, const char *meta, size_t meta_n,
            const char *defects, size_t defects_n)
{
    static ft_share_cut_result r;
    static ft_share_meta m;
    static ft_defects dfx;
    static wframe nf;
    ft_err e;
    int i, j, rc;

    if (ft_share_meta_parse(meta, meta_n, &m, &e))
        return fail_e(w, &e);
    if (defects && ft_defects_parse(defects, defects_n, &dfx, &e))
        return fail_e(w, &e);
    if (!raw || !cal)
        return fail(w, "no scan or no calibration");
    if (ft_share_cut(raw, (int)(raw_n / ((size_t)m.px * 6)), cal,
                     (int)(cal_n / ((size_t)m.px * 6)), faul,
                     faul ? faul_n : 0, defects ? &dfx : NULL, &m, &r, &e))
        return fail_e(w, &e);
    out_reset(w);
    rc = outf(w, "{\"detect_ok\":%d,\"noise_note\":", r.detect_ok);
    rc = rc ? rc : out_str(w, r.noise_rc ? r.noise_err.msg : "");
    rc = rc ? rc : outf(w, ",\"frames\":[");
    for (i = 0; !rc && i < r.n; i++) {
        const ft_share_cut_frame *c = &r.f[i];
        if (land(&m, c, &nf, &e)) {
            rc = fail_e(w, &e);
            break;
        }
        j = frame_at(w, nf.frame);
        if (j >= 0) {
            frame_free(&w->f[j]);
        } else {
            if (w->n == FT_MAX_FRAMES) {
                frame_free(&nf);
                rc = fail(w, "too many frames");
                break;
            }
            for (j = w->n; j > 0 && w->f[j - 1].frame > nf.frame; j--)
                w->f[j] = w->f[j - 1];
            w->n++;
        }
        w->f[j] = nf;
        if (w->disp_frame == nf.frame)
            w->disp_frame = 0;
        rc = outf(w, "%s{\"frame\":%d,\"estimated\":%d,\"shift_mm\":%.3f,"
                     "\"w\":%d,\"h\":%d}", i ? "," : "", nf.frame,
                  c->estimated, c->shift_mm, c->box[2], c->box[3]);
    }
    w->measured = 0;
    i = r.n;
    ft_share_cut_free(&r);
    if (rc)
        return -1;
    return outf(w, "]}") ? -1 : i;
}

int ftw_frames(const ftw *w, int *frames, int cap)
{
    int i;
    for (i = 0; i < w->n && i < cap; i++)
        frames[i] = w->f[i].frame;
    return w->n;
}

/* ------------------------------------------------------------ prints */

static const ft_emulsion *emulsion(ftw *w)
{
    const ft_emulsion *em;
    if (!w->has_print) {
        fail(w, "no strip.print");
        return NULL;
    }
    em = ft_emulsion_find(w->pf.emulsion);
    if (!em)
        fail(w, "not an emulsion");
    return em;
}

int ftw_measure(ftw *w)
{
    static double bsum[3][FT_MAX_FRAMES];
    const ft_image *proxies[FT_MAX_FRAMES];
    const ft_emulsion *em = emulsion(w);
    float base[3];
    int i, c, nbase = 0, idx[FT_MAX_FRAMES], ni = 0;
    ft_err e;

    if (!em)
        return -1;
    if (w->n == 0)
        return fail(w, "no frames");
    out_reset(w);
    for (i = 0; i < w->n; i++) {
        const wframe *f = &w->f[i];
        if ((f->sc.film == FT_FILM_SLIDE) != (em->mode == FT_RENDER_SLIDE)) {
            char m[160];
            snprintf(m, sizeof m, "frame %d is a %s; %s prints %s",
                     f->frame, f->sc.film == FT_FILM_SLIDE ? "slide"
                                                          : "negative",
                     em->id, em->mode == FT_RENDER_SLIDE ? "slides"
                                                         : "negatives");
            return fail(w, m);
        }
        if (f->sc.has_film_base) {
            for (c = 0; c < 3; c++)
                bsum[c][nbase] = f->sc.film_base[c];
            nbase++;
        }
        proxies[i] = &f->proxy;
    }
    if (nbase) {
        for (c = 0; c < 3; c++)
            base[c] = (float)ft_print_median(bsum[c], nbase);
    } else {
        if (ft_render_base_from_frames(proxies, w->n, em->mode, base, &e))
            return fail_e(w, &e);
        outf(w, "note: no film_base in the sidecars; estimated from the "
                "frames\n");
    }
    if (job_now(w)) {
        const ft_job *j = job_now(w);
        for (i = 0; i < j->nframes; i++)
            idx[ni++] = j->f[i].index;
    } else {
        for (i = 0; i < w->n; i++)
            idx[ni++] = w->f[i].frame;
    }
    if (ft_print_file_unknown(&w->pf, idx, ni, &e))
        outf(w, "warning: strip.print: %s\n", e.msg);
    if (ft_print_roll_measure(em, base, proxies, w->n, w->pfr, &w->roll,
                              &e))
        return fail_e(w, &e);
    if (w->n < 6 && em->balance == &ft_pm_balance_frontier)
        outf(w, "note: %d frame%s: print a roll's frames together for the "
                "roll's colour balance\n", w->n, w->n == 1 ? "" : "s");
    w->measured = 1;
    return 0;
}

/* frame i's JPEG into *jpg, its text into w->text */
static int print_one(ftw *w, int i, unsigned char **jpg, size_t *n)
{
    const ft_emulsion *em = emulsion(w);
    const wframe *f = &w->f[i];
    ft_share_print_info info;
    ft_image im;
    ft_err e;
    int rc;
    if (!em)
        return -1;
    if (frame_image(f, &im, &e))
        return fail_e(w, &e);
    rc = ft_share_print_frame(em, &w->roll, &w->pfr[i],
                              ft_print_file_clicks(&w->pf, f->frame),
                              ft_print_file_rotation(&w->pf, f->frame), &im,
                              &f->sc, f->frame, jpg, n, w->text, &info, &e);
    ft_image_free(&im);
    return rc ? fail_e(w, &e) : 0;
}

static int ensure_measured(ftw *w)
{
    if (w->measured)
        return 0;
    return ftw_measure(w);
}

int ftw_jpeg(ftw *w, int frame)
{
    unsigned char *jpg;
    size_t n;
    int i = frame_at(w, frame);
    if (i < 0)
        return fail(w, "no such frame");
    if (ensure_measured(w) || print_one(w, i, &jpg, &n))
        return -1;
    out_take(w, jpg, n);
    return 0;
}

/* src (transmittance) printed with em's recipe for roll/fr and clicks k,
 * turned rot degrees, sRGB 8-bit -> out: RGBA rows; *pw x *ph */
static int rgba_out(ftw *w, const ft_emulsion *em, const ft_print_roll *roll,
                    const ft_print_frame *fr, ft_cmyd k, const ft_image *src,
                    int rot, int *pw, int *ph)
{
    static ft_print_render pr;
    static ft_output out;
    ft_print_auto a;
    ft_recipe r;
    ft_err e;
    float *px = NULL;
    size_t npx, i;
    int y, rw, rh;
    if (ft_print_recipe(em, roll, fr, k, &r, &a, NULL, &e) ||
        ft_print_compile(em, &r, &pr, &e) ||
        ft_output_compile(FT_SPACE_SRGB, FT_GAMUT_SOFT, &out, &e))
        return fail_e(w, &e);
    npx = (size_t)src->w * src->h;
    px = malloc(npx * 3 * sizeof *px);
    out_reset(w);
    if (!px || out_room(w, npx * 4)) {
        free(px);
        return fail(w, "out of memory");
    }
    ft_print_apply(&pr, src->rgb, px, npx);
    rw = src->w;
    rh = src->h;
    if (rot) {                          /* the print's turn */
        ft_image ia, ib;
        ft_image_init(&ia);
        ft_image_init(&ib);
        ia.w = rw;
        ia.h = rh;
        ia.rgb = px;
        if (ft_image_rotate(&ia, rot, &ib, &e)) {
            free(px);
            return fail_e(w, &e);
        }
        free(px);
        px = ib.rgb;
        rw = ib.w;
        rh = ib.h;
    }
    for (y = 0; y < rh; y++) {
        float *row = px + (size_t)y * rw * 3;
        unsigned char *o = w->out + (size_t)y * rw * 4;
        ft_output_apply(&out, row, row, (size_t)rw);
        /* RGB at the row's end, then spread to RGBA in place */
        ft_output_quantise8(row, o + (size_t)rw, (size_t)rw, (size_t)y * rw);
        for (i = 0; i < (size_t)rw; i++) {
            const unsigned char *s = o + (size_t)rw + i * 3;
            unsigned char t0 = s[0], t1 = s[1], t2 = s[2];
            o[i * 4] = t0;
            o[i * 4 + 1] = t1;
            o[i * 4 + 2] = t2;
            o[i * 4 + 3] = 255;
        }
    }
    free(px);
    w->nout = npx * 4;
    *pw = rw;
    *ph = rh;
    return 0;
}

/* src resized so its long side is at most long_edge (never up) -> dst */
static int fit(ftw *w, const ft_image *src, int long_edge, ft_image *dst)
{
    ft_err e;
    void *scratch;
    int dw, dh, big = src->w > src->h ? src->w : src->h;
    double s = (double)long_edge / big;
    ft_image_init(dst);
    ft_resize_dims(src->w, src->h, s > 1 ? 1 : s, &dw, &dh);
    scratch = malloc(ft_resize_scratch_bytes(src->w, src->h, dw, dh));
    if (!scratch || ft_image_alloc(dst, dw, dh, &e) ||
        ft_resize(src->rgb, src->w, src->h, dst->rgb, dw, dh, scratch, &e)) {
        free(scratch);
        ft_image_free(dst);
        return scratch ? fail_e(w, &e) : fail(w, "out of memory");
    }
    free(scratch);
    return 0;
}

int ftw_display(ftw *w, int frame, int long_edge, int *pw, int *ph)
{
    const ft_emulsion *em;
    ft_err e;
    int i = frame_at(w, frame);
    if (i < 0)
        return fail(w, "no such frame");
    if (long_edge < 16)
        return fail(w, "display: too small");
    if (ensure_measured(w) || !(em = emulsion(w)))
        return -1;
    if (w->disp_frame != frame || w->disp_edge != long_edge) {
        const wframe *f = &w->f[i];
        ft_image im, cut;
        ft_image_free(&w->disp);
        w->disp_frame = 0;
        if (frame_image(f, &im, &e))
            return fail_e(w, &e);
        ft_image_init(&cut);
        if (ft_image_crop(&im, f->sc.box, &cut, &e)) {
            ft_image_free(&im);
            return fail_e(w, &e);
        }
        ft_image_free(&im);
        if (fit(w, &cut, long_edge, &w->disp)) {
            ft_image_free(&cut);
            return -1;
        }
        ft_image_free(&cut);
        w->disp_frame = frame;
        w->disp_edge = long_edge;
    }
    return rgba_out(w, em, &w->roll, &w->pfr[i],
                    ft_print_file_clicks(&w->pf, frame), &w->disp,
                    ft_print_file_rotation(&w->pf, frame), pw, ph);
}

/* ------------------------------------------------------------ preview */

int ftw_preview_load(ftw *w, const unsigned short *raw, size_t raw_n,
                     const unsigned short *cal, size_t cal_n,
                     const char *faul, size_t faul_n, const char *meta,
                     size_t meta_n)
{
    const ft_preview *pm = ft_preview_find("standard");
    const ft_job *j = &w->job;
    long px = meta_key(meta, meta_n, "px"), x = meta_key(meta, meta_n, "x"),
         bin = meta_key(meta, meta_n, "bin");
    ft_develop_opts o;
    ft_axes ax;
    ft_err e;
    float *t;
    int i, nl = 0, lines, cl;

    preview_free(w);
    if (!w->has_job)
        return fail(w, "no strip.job");
    if (px < 1 || px > 20000 || x < 0 || bin != pm->bin)
        return fail(w, "preview: not a standard preview (px, x, bin=12)");
    if (!raw || !cal)
        return fail(w, "no scan or no calibration");
    lines = (int)(raw_n / ((size_t)px * 6));
    cl = (int)(cal_n / ((size_t)px * 6));
    memset(&o, 0, sizeof o);
    o.x = (int)x;
    o.bin = (int)bin;
    t = ft_develop_t(raw, lines, (int)px, cal, cl, faul, faul ? faul_n : 0,
                     &o, &nl, &e);
    if (!t)
        return fail_e(w, &e);
    ft_preview_axes(pm, x, &ax);
    for (i = 0; i < j->nframes; i++) {
        const ft_job_frame *f = &j->f[i];
        pframe *p = &w->pv[w->npv];
        ft_develop_opts co;
        long l0 = (long)((f->y0 - ax.y_mm0) / ax.y_mm_per_line + 0.5),
             l1 = (long)((f->y0 + f->len - ax.y_mm0) / ax.y_mm_per_line + 0.5),
             c0 = (long)((f->x0 - ax.x_mm0) / ax.x_mm_per_px + 0.5),
             c1 = (long)((f->x0 + f->width - ax.x_mm0) / ax.x_mm_per_px + 0.5);
        if (l0 < 0) l0 = 0;
        if (c0 < 0) c0 = 0;
        if (l1 > nl) l1 = nl;
        if (c1 > px) c1 = px;
        if (l1 - l0 < 8 || c1 - c0 < 8)
            continue;                   /* off the preview: no frame */
        memset(&co, 0, sizeof co);
        co.crop_x = (int)c0;
        co.crop_y = (int)l0;
        co.crop_w = (int)(c1 - c0);
        co.crop_h = (int)(l1 - l0);
        ft_image_init(&p->im);
        ft_image_init(&p->proxy);
        if (ft_crop_orient(t, nl, (int)px, &co, &p->im, &e) ||
            ft_share_print_proxy(&p->im, &p->proxy, &e)) {
            ft_image_free(&p->im);
            ft_image_free(&p->proxy);
            free(t);
            preview_free(w);
            return fail_e(w, &e);
        }
        p->frame = f->index;
        w->npv++;
    }
    free(t);
    return w->npv;
}

/* the strip's balance over the preview's frames (no clicks) */
static int preview_measure(ftw *w, const ft_emulsion *em)
{
    const ft_image *proxies[FT_MAX_FRAMES];
    const ft_job *j = &w->job;
    float base[3];
    ft_err e;
    int i;
    if (w->pv_measured)
        return 0;
    if (w->npv == 0)
        return fail(w, "no preview frames");
    if ((j->film == FT_FILM_SLIDE) != (em->mode == FT_RENDER_SLIDE))
        return fail(w, j->film == FT_FILM_SLIDE
                       ? "the strip is slide film; the emulsion prints "
                         "negatives"
                       : "the strip is negative film; the emulsion prints "
                         "slides");
    for (i = 0; i < w->npv; i++)
        proxies[i] = &w->pv[i].proxy;
    if (j->has_film_base) {
        for (i = 0; i < 3; i++)
            base[i] = (float)j->film_base[i];
    } else if (ft_render_base_from_frames(proxies, w->npv, em->mode, base,
                                          &e)) {
        return fail_e(w, &e);
    }
    if (ft_print_roll_measure(em, base, proxies, w->npv, w->pv_pfr,
                              &w->pv_roll, &e))
        return fail_e(w, &e);
    w->pv_measured = 1;
    return 0;
}

int ftw_preview_display(ftw *w, int frame, int long_edge, int *pw, int *ph)
{
    static const ft_cmyd none;
    const ft_emulsion *em;
    ft_image d;
    int i, rc;
    for (i = 0; i < w->npv && w->pv[i].frame != frame; i++)
        ;
    if (i == w->npv)
        return fail(w, "no such frame in the preview");
    if (long_edge < 16)
        return fail(w, "display: too small");
    if (!(em = emulsion(w)) || preview_measure(w, em) ||
        fit(w, &w->pv[i].im, long_edge, &d))
        return -1;
    rc = rgba_out(w, em, &w->pv_roll, &w->pv_pfr[i], none, &d, 0, pw, ph);
    ft_image_free(&d);
    return rc;
}

/* a frame's JPEG in the zip: its name, else <base>_frameNN */
static void zip_name(const ftw *w, const char *base, int frame, char *name,
                     size_t cap)
{
    const char *nm = w->has_print ? ft_print_file_name(&w->pf, frame) : "";
    if (nm[0])
        snprintf(name, cap, "%s.jpg", nm);
    else
        snprintf(name, cap, "%.120s_frame%02d.jpg", base, frame);
}

int ftw_zip(ftw *w, const char *base)
{
    ft_zip z;
    ft_err e;
    unsigned char *jpg, *zb;
    size_t n, nz;
    char name[160];
    int i;
    if (w->n == 0)
        return fail(w, "no frames");
    if (ensure_measured(w))
        return -1;
    ft_zip_init(&z);
    for (i = 0; i < w->n; i++) {
        if (print_one(w, i, &jpg, &n)) {
            ft_zip_free(&z);
            return -1;
        }
        zip_name(w, base, w->f[i].frame, name, sizeof name);
        if (ft_zip_add(&z, name, jpg, n, &e)) {
            free(jpg);
            ft_zip_free(&z);
            return fail_e(w, &e);
        }
        free(jpg);
        snprintf(name + strlen(name), sizeof name - strlen(name), ".txt");
        if (ft_zip_add(&z, name, w->text, strlen(w->text), &e)) {
            ft_zip_free(&z);
            return fail_e(w, &e);
        }
    }
    if (ft_zip_finish(&z, &zb, &nz, &e))
        return fail_e(w, &e);
    out_take(w, zb, nz);
    return 0;
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ftweb_cli: the page's engine (ftweb.h) driven from files, as the page
 * drives it from the box's bytes. The test harness for "the page makes
 * the CLI's JPEGs" (tests/test_ftweb.py), native and as a WASI program.
 *
 *   ftweb_cli --print strip.print [--job strip.job] -o outdir
 *             [--display N px] window.raw...
 *
 * Each window: <w>.raw, <w>.raw.cal, <w>.raw.txt and <w>.raw.faul if
 * there. Then every frame: <base>_frameNN.jpg and .jpg.txt (base: the
 * first window's name without .raw and _winNN, as ftdev cut), and
 * <base>.zip; --display: the screen copy of frame N as display.rgba.
 *
 *   ftweb_cli [--lens focus.txt] --dof95 scale bin
 *
 * ftw_dof95 on that lens (default: libft's), printed as %.6f.
 *
 *   ftweb_cli --stream <base> [--chunk N] [--slide]
 *
 * The streamed preview: <base>.raw.cal, .raw.txt (.raw.faul if there),
 * then <base>.raw fed N lines a call (0: 1..400 lines, from a fixed
 * seed), as the live bar reads it; one line per call that sealed frames
 * ("feed <lines in>: <json>"), "end: <json>", then the base job's text.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ft.h"
#include "ftweb.h"

/* the whole file in an ftw_alloc buffer; NULL if unreadable */
static void *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b = NULL;
    size_t cap = 0, got;
    *n = 0;
    if (!f)
        return NULL;
    for (;;) {
        if (*n + 65536 > cap) {
            unsigned char *nb;
            cap = cap ? cap * 2 : 1 << 20;
            nb = realloc(b, cap);
            if (!nb) {
                free(b);
                fclose(f);
                return NULL;
            }
            b = nb;
        }
        got = fread(b + *n, 1, cap - *n, f);
        *n += got;
        if (got == 0)
            break;
    }
    fclose(f);
    return b;
}

static int save(const char *path, const void *b, size_t n)
{
    FILE *f = fopen(path, "wb");
    int bad = !f || fwrite(b, 1, n, f) != n;
    if (f && fclose(f))
        bad = 1;
    if (bad)
        fprintf(stderr, "ftweb_cli: cannot write %s\n", path);
    return bad;
}

static int stream(ftw *w, const char *base, int chunk, int slide)
{
    char path[1024];
    size_t nr, nc, nm, nf = 0, at = 0, row;
    unsigned char *raw;
    void *cal, *meta, *faul;
    unsigned seed = 1;
    int rc = 1, k;
    snprintf(path, sizeof path, "%s.raw", base);
    raw = load(path, &nr);
    snprintf(path, sizeof path, "%s.raw.cal", base);
    cal = load(path, &nc);
    snprintf(path, sizeof path, "%s.raw.txt", base);
    meta = load(path, &nm);
    snprintf(path, sizeof path, "%s.raw.faul", base);
    faul = load(path, &nf);
    if (!raw || !cal || !meta) {
        fprintf(stderr, "ftweb_cli: %s: files missing\n", base);
        goto out;
    }
    if (ftw_stream_begin(w, cal, nc, faul, nf, meta, nm, slide)) {
        fprintf(stderr, "ftweb_cli: %s\n", ftw_error(w));
        goto out;
    }
    row = (size_t)ftw_preview_px() * 6;
    while (at + row <= nr) {
        size_t n = (size_t)chunk;
        if (!n) {
            seed = seed * 1103515245u + 12345u;
            n = 1 + (seed >> 16) % 400;
        }
        n *= row;
        if (at + n > nr)
            n = (nr - at) / row * row;
        k = ftw_stream_feed(w, (const unsigned short *)(raw + at), n);
        if (k < 0) {
            fprintf(stderr, "ftweb_cli: %s\n", ftw_error(w));
            goto out;
        }
        at += n;
        if (k > 0)
            printf("feed %lu: %.*s\n", (unsigned long)(at / row),
                   (int)ftw_out_len(w), (const char *)ftw_out(w));
    }
    if (ftw_stream_end(w) < 0) {
        fprintf(stderr, "ftweb_cli: %s\n", ftw_error(w));
        goto out;
    }
    printf("end: %.*s\n", (int)ftw_out_len(w), (const char *)ftw_out(w));
    if (ftw_job_text(w)) {
        fprintf(stderr, "ftweb_cli: %s\n", ftw_error(w));
        goto out;
    }
    printf("%.*s", (int)ftw_out_len(w), (const char *)ftw_out(w));
    rc = 0;
out:
    free(raw);
    free(cal);
    free(meta);
    free(faul);
    return rc;
}

static void usage(void)
{
    fprintf(stderr, "usage: ftweb_cli --print strip.print [--job strip.job]"
                    " -o outdir [--display N px] window.raw...\n"
                    "       ftweb_cli [--lens focus.txt] --dof95 scale"
                    " bin\n"
                    "       ftweb_cli --stream <base> [--chunk N] [--slide]"
                    "\n");
}

int main(int argc, char **argv)
{
    const char *print = NULL, *job = NULL, *outdir = NULL, *lens = NULL;
    const char *strm = NULL;
    int chunk = 0, slide = 0;
    double dof_scale = 0;
    int dof_bin = 0, dof = 0;
    const char *wins[64];
    char base[512], path[1024];
    int nw = 0, i, disp = 0, disp_px = 0, frames[64], nf, rc = 1;
    ftw *w = ftw_new();

    if (!w)
        return 1;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--print") == 0 && i + 1 < argc)
            print = argv[++i];
        else if (strcmp(argv[i], "--job") == 0 && i + 1 < argc)
            job = argv[++i];
        else if (strcmp(argv[i], "--lens") == 0 && i + 1 < argc)
            lens = argv[++i];
        else if (strcmp(argv[i], "--dof95") == 0 && i + 2 < argc) {
            dof = 1;
            dof_scale = atof(argv[++i]);
            dof_bin = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--stream") == 0 && i + 1 < argc)
            strm = argv[++i];
        else if (strcmp(argv[i], "--chunk") == 0 && i + 1 < argc)
            chunk = atoi(argv[++i]);
        else if (strcmp(argv[i], "--slide") == 0)
            slide = 1;
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            outdir = argv[++i];
        else if (strcmp(argv[i], "--display") == 0 && i + 2 < argc) {
            disp = atoi(argv[++i]);
            disp_px = atoi(argv[++i]);
        } else if (argv[i][0] != '-' && nw < 64)
            wins[nw++] = argv[i];
        else {
            usage();
            return 2;
        }
    }
    if (lens) {
        size_t n;
        char *t = load(lens, &n);
        if (!t || ftw_lens(w, t, n)) {
            fprintf(stderr, "ftweb_cli: %s: %s\n", lens,
                    t ? ftw_error(w) : "cannot read");
            free(t);
            goto out;
        }
        free(t);
    }
    if (strm) {
        rc = stream(w, strm, chunk < 0 ? 0 : chunk, slide);
        goto out;
    }
    if (dof) {
        printf("%.6f\n", ftw_dof95(w, dof_scale, dof_bin));
        rc = 0;
        goto out;
    }
    if (!print || !outdir || nw == 0) {
        usage();
        return 2;
    }
    {
        size_t n;
        char *t = load(print, &n);
        if (!t || ftw_print_load(w, t, n)) {
            fprintf(stderr, "ftweb_cli: %s: %s\n", print,
                    t ? ftw_error(w) : "cannot read");
            free(t);
            goto out;
        }
        free(t);
        if (job) {
            t = load(job, &n);
            if (!t || ftw_job_load(w, t, n)) {
                fprintf(stderr, "ftweb_cli: %s: %s\n", job,
                        t ? ftw_error(w) : "cannot read");
                free(t);
                goto out;
            }
            free(t);
        }
    }
    for (i = 0; i < nw; i++) {
        size_t nr, nc, nm, nfa = 0;
        void *raw = load(wins[i], &nr), *cal, *meta, *faul;
        snprintf(path, sizeof path, "%s.cal", wins[i]);
        cal = load(path, &nc);
        snprintf(path, sizeof path, "%s.txt", wins[i]);
        meta = load(path, &nm);
        snprintf(path, sizeof path, "%s.faul", wins[i]);
        faul = load(path, &nfa);
        if (!raw || !cal || !meta ||
            ftw_cut(w, raw, nr, cal, nc, faul, nfa, meta, nm, NULL, 0) < 0) {
            fprintf(stderr, "ftweb_cli: %s: %s\n", wins[i],
                    raw && cal && meta ? ftw_error(w) : "cannot read");
            free(raw);
            free(cal);
            free(meta);
            free(faul);
            goto out;
        }
        printf("%s: %.*s\n", wins[i], (int)ftw_out_len(w),
               (const char *)ftw_out(w));
        free(raw);
        free(cal);
        free(meta);
        free(faul);
    }
    /* the base name: ftdev cut's */
    {
        const char *s = strrchr(wins[0], '/');
        size_t l;
        snprintf(base, sizeof base, "%s", s ? s + 1 : wins[0]);
        l = strlen(base);
        if (l > 4 && strcmp(base + l - 4, ".raw") == 0)
            base[l -= 4] = 0;
        if (l > 6 && strncmp(base + l - 6, "_win", 4) == 0 &&
            base[l - 2] >= '0' && base[l - 2] <= '9' &&
            base[l - 1] >= '0' && base[l - 1] <= '9')
            base[l - 6] = 0;
    }
    if (ftw_measure(w)) {
        fprintf(stderr, "ftweb_cli: %s\n", ftw_error(w));
        goto out;
    }
    fprintf(stderr, "%.*s", (int)ftw_out_len(w), (const char *)ftw_out(w));
    nf = ftw_frames(w, frames, 64);
    for (i = 0; i < nf; i++) {
        if (ftw_jpeg(w, frames[i])) {
            fprintf(stderr, "ftweb_cli: frame %d: %s\n", frames[i],
                    ftw_error(w));
            goto out;
        }
        snprintf(path, sizeof path, "%s/%s_frame%02d.jpg", outdir, base,
                 frames[i]);
        if (save(path, ftw_out(w), ftw_out_len(w)))
            goto out;
        strcat(path, ".txt");
        if (save(path, ftw_text(w), strlen(ftw_text(w))))
            goto out;
        printf("%s\n", path);
    }
    if (ftw_zip(w, base)) {
        fprintf(stderr, "ftweb_cli: zip: %s\n", ftw_error(w));
        goto out;
    }
    snprintf(path, sizeof path, "%s/%s.zip", outdir, base);
    if (save(path, ftw_out(w), ftw_out_len(w)))
        goto out;
    if (disp) {
        int pw, ph;
        if (ftw_display(w, disp, disp_px, &pw, &ph)) {
            fprintf(stderr, "ftweb_cli: display: %s\n", ftw_error(w));
            goto out;
        }
        snprintf(path, sizeof path, "%s/display.rgba", outdir);
        if (save(path, ftw_out(w), ftw_out_len(w)))
            goto out;
        printf("display %dx%d\n", pw, ph);
    }
    rc = 0;
out:
    ftw_free(w);
    return rc;
}

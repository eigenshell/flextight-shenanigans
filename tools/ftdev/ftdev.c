/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ftdev: develop an ftpoc scan into image files, on any OS (no scanner).
 *
 *   ftdev <scan.raw> [-f dng,png,positive] [-o base] [-d defects.txt]
 *         [--no-crop] [--film neg|slide|bw] [--base r,g,b] [--exposure s]
 *         [--tint r,g,b] [--grade g] [--curve preset|"x,y x,y ..."]
 *   ftdev detail <pass.raw> --job <file.job> ...   (detail.c)
 *   ftdev bench [...]                               (detail.c)
 *   ftdev cut <window.raw> [-o base] [-d defects.txt]
 *   ftdev export <frame.dng> [render flags] [--space srgb|p3|adobe]
 *         [--gamut soft|clip] [--crop x,y,w,h] [--long-edge px]
 *         [--quality q] [--raw-too] [-o out.jpg]
 *         [--edit roll.edit [--frame N]]
 *   ftdev stock learn <roll.edit> <frame.dng>... -o <name.look> [--replace]
 *   ftdev print <frame.dng>... (--emulsion <id> [--cmyd c,m,y,d] |
 *         --roll <roll.print> [--job <file.job>]) [-o out.jpg]
 *   ftdev emulsions
 *
 * Reads <scan.raw> with its .cal, .faul and .txt (ftpoc's output), runs
 * the libft pipeline (ft_develop.h) and writes one file per format to
 * <base><ext>; base defaults to the scan path without ".raw". A scan from
 * `scan --job` carries its frame's crop (crop_* in the .txt), which is
 * applied unless --no-crop. -d adds defective CCD pixels the scanner's
 * Faul Pixs table misses (format in ft_develop.h).
 *
 * -f positive renders the frame (ft_render.h; needs --film): the film base
 * from --base, else the .txt's film_base (from the frames preview), else
 * an estimate from the frame itself; range and exposure automatic; then
 * the overrides. The recipe used is printed as flags to paste back.
 *
 * cut and export are the scan-to-share steps (share.c, ft_share_cut.h).
 * export --edit renders with a roll's look and the frame's overrides
 * (ft_look.h); every export writes how it was made to <out.jpg>.txt and
 * the JPEG's comment. stock learn (stock.c) saves a roll's look with stock
 * hints learned from its frames. print (print.c) prints frames through an
 * emulsion with C/M/Y/D clicks (ft_print.h).
 *
 * Build: make (Linux/macOS), or with mingw:
 *   gcc -std=c99 -O2 -I..\..\libft -I..\detail -o ftdev.exe ftdev.c
 *       detail.c util.c render_flags.c share.c stock.c print.c
 *       ..\detail\ring.c ..\detail\worker.c ..\..\libft\ft_jpeg.c
 *       <every other libft .c>
 *       -ljpeg -lm
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ft_develop.h"
#include "ft_image.h"
#include "ft_noise_model.h"
#include "ft_out.h"
#include "ft_render.h"
#include "ft_render_auto.h"
#include "render_flags.h"
#include "share.h"
#include "util.h"

int ftdev_detail(int argc, char **argv);    /* detail.c */
int ftdev_bench(int argc, char **argv);

#define MAX_FORMATS 8

static ft_render g_render;              /* ~380 KB of LUTs: not on the stack */

void *ftdev_load(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    long n;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 &&
        fseek(f, 0, SEEK_SET) == 0 && (buf = malloc((size_t)n + 1)) != NULL) {
        if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
            free(buf);
            buf = NULL;
        } else {
            buf[n] = 0;
            *size = n;
        }
    }
    fclose(f);
    return buf;
}

static void usage(void)
{
    const ft_writer *w;
    int i;
    fprintf(stderr, "usage: ftdev <scan.raw> [-f fmt[,fmt..]] [-o base] "
                    "[-d defects.txt] [--no-crop]\n"
                    "             [--film neg|slide|bw] [--base r,g,b] "
                    "[--exposure stops] [--tint r,g,b]\n"
                    "             [--grade g] [--curve preset|\"x,y x,y ...\"]\n"
                    "       ftdev detail <pass.raw> --job <file.job> ... "
                    "(a kept detailed-scan pass)\n"
                    "       ftdev bench [...] (the detailed-scan worker's "
                    "speed here)\n"
                    "       ftdev cut <window.raw> [-o base] [-d defects.txt]\n"
                    "       ftdev export <frame.dng> [render flags] "
                    "[--space srgb|p3|adobe]\n"
                    "             [--gamut soft|clip] [--crop x,y,w,h] "
                    "[--long-edge px] [--quality q]\n"
                    "             [--nr-sensor k] [--nr-chroma k] "
                    "[--nr-grain k] [--sharpen a]\n"
                    "             [--raw-too] [-o out.jpg] "
                    "[--edit roll.edit [--frame N]]\n"
                    "       ftdev stock learn <roll.edit> <frame.dng>... "
                    "-o <name.look> [--replace]\n"
                    "       ftdev print <frame.dng>... (--emulsion <id> "
                    "[--cmyd c,m,y,d] | --roll <roll.print>\n"
                    "             [--job <file.job>]) [-o out.jpg]\n"
                    "       ftdev emulsions\n"
                    "       ftdev plan <strip.job> --size 4mp|6mp|px:frames "
                    "... [-o out.job]\n"
                    "formats (default dng):\n");
    for (w = ft_writers(); w->name; w++)
        fprintf(stderr, "  %-8s %s\n", w->name, w->what);
    fprintf(stderr, "curve presets:");
    for (i = 0; ft_curve_preset_name(i); i++)
        fprintf(stderr, " %s", ft_curve_preset_name(i));
    fprintf(stderr, "\n");
}

int main(int argc, char **argv)
{
    const char *scan = NULL, *base = NULL, *fmts = "dng", *dpath = NULL;
    const ft_writer *wr[MAX_FORMATS];
    int nwr = 0, no_crop = 0, i, rc = 1, render = 0;
    rflags rf;
    ft_recipe rcp;
    char path[1024], *meta = NULL, *faul = NULL, *dtext = NULL;
    char basebuf[1024], preset[32] = "";
    unsigned short *raw = NULL, *cal = NULL;
    long nmeta = 0, nraw = 0, ncal = 0, nfaul = 0, nd = 0, px, lines, cl;
    ft_defects defects;
    ft_develop_opts o;
    ft_image im;
    ft_err e;

    if (argc > 1 && strcmp(argv[1], "detail") == 0)
        return ftdev_detail(argc - 2, argv + 2);
    if (argc > 1 && strcmp(argv[1], "bench") == 0)
        return ftdev_bench(argc - 2, argv + 2);
    if (argc > 1 && strcmp(argv[1], "cut") == 0)
        return ftdev_cut(argc - 2, argv + 2);
    if (argc > 1 && strcmp(argv[1], "export") == 0)
        return ftdev_export(argc - 2, argv + 2);
    if (argc > 1 && strcmp(argv[1], "stock") == 0)
        return ftdev_stock(argc - 2, argv + 2);
    if (argc > 1 && strcmp(argv[1], "print") == 0)
        return ftdev_print(argc - 2, argv + 2);
    if (argc > 1 && strcmp(argv[1], "emulsions") == 0)
        return ftdev_emulsions(argc - 2, argv + 2);
    if (argc > 1 && strcmp(argv[1], "plan") == 0)
        return ftdev_plan(argc - 2, argv + 2);
    ft_image_init(&im);
    rflags_init(&rf);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        int r = rflags_parse(&rf, argc, argv, &i);
        if (r < 0)
            return 2;
        if (r > 0)
            continue;
        if (strcmp(a, "-f") == 0 && i + 1 < argc) fmts = argv[++i];
        else if (strcmp(a, "-o") == 0 && i + 1 < argc) base = argv[++i];
        else if (strcmp(a, "-d") == 0 && i + 1 < argc) dpath = argv[++i];
        else if (strcmp(a, "--no-crop") == 0) no_crop = 1;
        else if (a[0] == '-' || scan) {
            usage();
            return 2;
        } else
            scan = a;
    }
    if (!scan) {
        usage();
        return 2;
    }
    {
        char list[256], *tok;
        snprintf(list, sizeof list, "%s", fmts);
        for (tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
            const ft_writer *w = ft_writer_find(tok);
            if (!w) {
                fprintf(stderr, "ftdev: unknown format '%s'\n", tok);
                usage();
                return 2;
            }
            if (nwr == MAX_FORMATS)
                break;
            wr[nwr++] = w;
            render |= w->write == ft_png_write_positive;
        }
    }
    if (render) {
        if (rf.film < 0) {
            fprintf(stderr, "ftdev: -f positive needs --film neg|slide|bw\n");
            return 2;
        }
        if (rflags_recipe(&rf, &rcp))
            return 2;
    } else if (rflags_any(&rf))
        fprintf(stderr, "ftdev: note: render flags ignored without "
                        "-f positive\n");
    if (!base) {
        size_t n = strlen(scan);
        snprintf(basebuf, sizeof basebuf, "%s", scan);
        if (n > 4 && strcmp(basebuf + n - 4, ".raw") == 0)
            basebuf[n - 4] = 0;
        base = basebuf;
    }

    snprintf(path, sizeof path, "%s.txt", scan);
    meta = ftdev_load(path, &nmeta);
    if (!meta) {
        fprintf(stderr, "ftdev: cannot read %s\n", path);
        goto out;
    }
    memset(&o, 0, sizeof o);
    px = meta_long(meta, "px", 0);
    o.x = (int)meta_long(meta, "x", -1);
    o.bin = (int)meta_long(meta, "bin", 0);
    meta_str(meta, "preset", preset, sizeof preset);
    if (o.bin < 1)              /* .txt from before ftpoc recorded bin */
        o.bin = strcmp(preset, "preview") == 0 ? 12
              : strcmp(preset, "2000") == 0 ? 3 : 6;
    if (px < 1 || px > 20000 || o.x < 0) {
        fprintf(stderr, "ftdev: %s lacks px/x\n", path);
        goto out;
    }
    if (!no_crop && meta_long(meta, "crop_w", 0) > 0) {
        o.crop_x = (int)meta_long(meta, "crop_x", 0);
        o.crop_y = (int)meta_long(meta, "crop_y", 0);
        o.crop_w = (int)meta_long(meta, "crop_w", 0);
        o.crop_h = (int)meta_long(meta, "crop_h", 0);
    }
    if (dpath) {
        dtext = ftdev_load(dpath, &nd);
        if (!dtext) {
            fprintf(stderr, "ftdev: cannot read %s\n", dpath);
            goto out;
        }
        if (ft_defects_parse(dtext, (size_t)nd, &defects, &e)) {
            fprintf(stderr, "ftdev: %s: %s\n", dpath, e.msg);
            goto out;
        }
        o.defects = &defects;
    }

    raw = ftdev_load(scan, &nraw);
    snprintf(path, sizeof path, "%s.cal", scan);
    cal = ftdev_load(path, &ncal);
    if (!raw || !cal) {
        fprintf(stderr, "ftdev: cannot read %s or its .cal\n", scan);
        goto out;
    }
    lines = nraw / (px * 6);
    cl = ncal / (px * 6);
    snprintf(path, sizeof path, "%s.faul", scan);
    faul = ftdev_load(path, &nfaul);
    if (!faul)
        fprintf(stderr, "ftdev: note: no %s; Faul Pixs not interpolated\n",
                path);

    im.ppi = (double)meta_long(meta, "ppi", 0);
    im.film = rf.film;
    snprintf(im.desc, sizeof im.desc,
             "%s; preset %s, %ld ppi, bin %d, x %d, %ld px x %ld lines; "
             "crop %d,%d %dx%d; film %s",
             scan, preset[0] ? preset : "?", meta_long(meta, "ppi", 0), o.bin,
             o.x, px, lines, o.crop_x, o.crop_y, o.crop_w, o.crop_h,
             rf.film == FT_FILM_NEGATIVE ? "negative"
             : rf.film == FT_FILM_SLIDE ? "slide" : "unknown");
    if (ft_develop(raw, (int)lines, (int)px, cal, (int)cl, faul,
                   (size_t)(faul ? nfaul : 0), &o, &im, &e)) {
        fprintf(stderr, "ftdev: %s: %s\n", scan, e.msg);
        goto out;
    }
    free(raw);
    raw = NULL;
    /* the NoiseProfile rides in the DNG; without one export estimates */
    {
        int nr = ft_noise_model(cal, (int)cl, (int)px, im.noise, &e);
        im.has_noise = nr >= 0;
        if (nr)
            fprintf(stderr, "ftdev: note: %s\n", e.msg);
    }
    rc = 0;
    /* the developed image first (DNG, PNG preview), then the render */
    for (i = 0; i < nwr; i++) {
        if (wr[i]->write == ft_png_write_positive)
            continue;
        snprintf(path, sizeof path, "%s%s", base, wr[i]->ext);
        if (wr[i]->write(path, &im, &e)) {
            fprintf(stderr, "ftdev: %s\n", e.msg);
            rc = 1;
        } else
            printf("%s: %dx%d, %.0f ppi\n", path, im.w, im.h, im.ppi);
    }
    if (render) {
        char fb[128];
        double d[3];
        int has = meta_str(meta, "film_base", fb, sizeof fb) &&
                  sscanf(fb, "%lf %lf %lf", &d[0], &d[1], &d[2]) == 3 &&
                  isfinite(d[0]) && isfinite(d[1]) && isfinite(d[2]);
        int r = rflags_render(&rf, &rcp, has ? d : NULL, &im, &g_render);
        if (r) {
            rc = r;
            goto out;
        }
        snprintf(path, sizeof path, "%s%s", base,
                 ft_writer_find("positive")->ext);
        if (ft_png_write_positive(path, &im, &e)) {
            fprintf(stderr, "ftdev: %s\n", e.msg);
            rc = 1;
        } else
            printf("%s: %dx%d, %.0f ppi\n", path, im.w, im.h, im.ppi);
    }
out:
    ft_image_free(&im);
    free(meta);
    free(faul);
    free(dtext);
    free(raw);
    free(cal);
    return rc;
}

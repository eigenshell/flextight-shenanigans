/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ftdev's scan-to-share commands (share.c) and stock learning (stock.c). */
#ifndef SHARE_H
#define SHARE_H
#include "ft_image.h"
#include "ft_look.h"
#include "ft_output.h"
#include "ft_share_cut.h"

/* whole file + a NUL; NULL if unreadable (ftdev.c) */
void *ftdev_load(const char *path, long *size);
/* noise reduction on cut (transmittance, before the render) as export
 * does it: sensor NR with the profile of im (the whole frame's DNG), else
 * the sidecar's (sc NULL: none), else estimated; then grain NR. 0 ok, 1
 * out of memory (printed). */
int ftdev_denoise(
    const ft_image *im, const ft_share_sidecar *sc, ft_image *cut,
    const ft_look_detail *d
);
/* jpg (n bytes, ow x oh) at outp, text in <outp>.txt, the summary line
 * on stdout. 0 ok, 1 error, 2 bad path (messages printed). */
int ftdev_write_jpeg(
    const char *outp, const unsigned char *jpg, size_t n, int ow, int oh,
    ft_space space, ft_gamut gamut, int quality, const char *text
);
/* the share tail (ft_share_finish) into outp: resize cut (rendered: linear
 * Rec.2020) by scale (never up), output sharpening, output colour, an 8-bit
 * JPEG at outp with its ICC profile and text as its comment, and text in
 * <outp>.txt. 0 ok, 1 error, 2 bad path (messages printed). */
int ftdev_finish(
    const ft_image *cut, double scale, double sharpen, ft_space space,
    ft_gamut gamut, int quality, const char *text, const char *outp
);
/* ftdev cut <window.raw> [-o base] [-d defects.txt] */
int ftdev_cut(int argc, char **argv);
/* ftdev export <frame.dng> [render flags] [--space ..] [--gamut ..]
 * [--crop x,y,w,h] [--long-edge px] [--quality q] [--raw-too] [-o ..]
 * [--edit roll.edit [--frame N]] */
int ftdev_export(int argc, char **argv);
/* ftdev print <frame.dng>... (--emulsion <id> [--cmyd c,m,y,d] |
 * --roll <roll.print> [--job <file.job>]) [-o out.jpg] (print.c) */
int ftdev_print(int argc, char **argv);
/* ftdev emulsions: the emulsion table */
int ftdev_emulsions(int argc, char **argv);
/* ftdev plan <strip.job> --size 4mp|6mp|px:frames ... (plan.c) */
int ftdev_plan(int argc, char **argv);
/* ftdev stock learn <roll.edit> <frame.dng>... -o <name.look> [--replace] */
int ftdev_stock(int argc, char **argv);

#endif

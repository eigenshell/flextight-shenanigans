/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Scan to share, after the scan: a window scan (several selected frames,
 * ftpoc scan --job of a share job) -> one working raw per frame.
 *
 * The film creeps between runs (the holder travels out and back for every
 * scan's calibration), so the preview's frame boxes are only a guess:
 * the window is scanned with a margin and the frames are detected again
 * in its own pixels (ft_detect on a ~500 ppi copy), matched to the
 * selected frames and cut out with a small bleed. A frame detection
 * misses keeps its preview box, moved by the other frames' shift, and is
 * flagged estimated.
 *
 * Text formats (key=value lines, as ftpoc's scan .txt):
 *   window scan .txt, appended by ftpoc for a share job:
 *     film=negative|slide  holder=24x217  share=2000  hf_half_res=0|1
 *     film_base=<D r> <D g> <D b>  (optional)
 *     share_frame=<index> <type> <y0> <len> <x0> <width>   (one per frame,
 *     holder mm, the preview box)
 *   working raw sidecar, <frame>.dng.txt, written by ftdev cut:
 *     frame= type= film= ppi= share= hf_half_res= film_base= (optional)
 *     box=<x> <y> <w> <h>  (the frame in the DNG, px, without bleed)
 *     export_scale=<s>  estimated=0|1  shift_mm=<along>
 *     noise_profile=<S r> <O r> <S g> <O g> <S b> <O b>  (optional; the
 *     DNG's NoiseProfile, ft_noise_model.h) */
#ifndef FT_SHARE_CUT_H
#define FT_SHARE_CUT_H
#include <stddef.h>
#include <stdio.h>
#include "ft.h"
#include "ft_develop.h"
#include "ft_image.h"
#include "ft_job.h"
#include "ft_share.h"

typedef struct {
    /* the scan (ftpoc's own keys) */
    double ppi;
    int bin, px;
    long x, y;                 /* first output px across, first line along */
    /* the share keys */
    ft_film film;
    char holder[16];
    double share;
    int hf_half_res;
    int has_film_base;
    double film_base[3];
    int nframes;
    ft_share_frame f[FT_MAX_FRAMES];
} ft_share_meta;

/* parse a window scan's .txt; -1 with e naming the key on error or when
 * a required key (ppi px x y bin share, at least one share_frame) is
 * missing */
int ft_share_meta_parse(const char *text, size_t n, ft_share_meta *m,
                        ft_err *e);
/* append the share keys for group g of job j (to the scan's .txt) */
int ft_share_meta_write(FILE *out, const ft_job *j, int g);

typedef struct {
    int index;
    ft_frame_type type;
    int estimated;             /* detection missed it: preview box moved */
    double shift_mm;           /* along: where it was found - preview */
    int box[4];                /* x y w h: the frame in im, bleed excluded;
                                  x along the strip, y across (as im) */
    double export_scale;       /* min(1, target px / box long side) */
    ft_image im;               /* frame + bleed: linear transmittance */
} ft_share_cut_frame;

typedef struct {
    int detect_ok;             /* 0: detection failed, all estimated */
    int noise_rc;              /* ft_noise_model: 0, 1 clamped, -1 none */
    ft_err noise_err;          /* its note when noise_rc != 0 */
    double noise[6];
    int n;
    ft_share_cut_frame f[FT_MAX_FRAMES];
} ft_share_cut_result;

/* develop the window (raw: lines x m->px x 3, cal: cal_lines of them),
 * detect, cut every frame of m. Fills r (caller: ft_share_cut_free).
 * -1 with e filled when the scan cannot be developed or a frame's box
 * leaves the scan entirely. */
int ft_share_cut(const unsigned short *raw, int lines,
                 const unsigned short *cal, int cal_lines,
                 const char *faul, size_t faul_n, const ft_defects *d,
                 const ft_share_meta *m, ft_share_cut_result *r, ft_err *e);
void ft_share_cut_free(ft_share_cut_result *r);

typedef struct {
    int frame;
    ft_frame_type type;
    ft_film film;
    double ppi, share, export_scale, shift_mm;
    int hf_half_res, estimated;
    int has_film_base;
    double film_base[3];
    int box[4];
    int has_noise;             /* noise_profile=, all finite, S >= 0, O > 0 */
    double noise[6];
} ft_share_sidecar;

/* the sidecar text into buf (snprintf's contract: the full length back,
 * at most cap - 1 bytes and a NUL written; buf may be NULL with cap 0) */
int ft_share_sidecar_write_buf(char *buf, size_t cap,
                               const ft_share_meta *m,
                               const ft_share_cut_frame *c);
/* the same into a stream (ft_share_cut_io.c, as ft_share_meta_write) */
int ft_share_sidecar_write(FILE *out, const ft_share_meta *m,
                           const ft_share_cut_frame *c);
/* -1 with e naming the key if box or export_scale is missing or bad, or
 * noise_profile has not six numbers; a noise_profile out of range reads as
 * none (has_noise 0) */
int ft_share_sidecar_parse(const char *text, size_t n, ft_share_sidecar *s,
                           ft_err *e);

#endif

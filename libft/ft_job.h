/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The job file: what `ftpoc frames` found and `ftpoc scan --job` scans.
 * Plain key = value text; see tools/poc/README.md. '#' always starts a
 * comment, even after a value. */
#ifndef FT_JOB_H
#define FT_JOB_H
#include <stdio.h>
#include "ft.h"
#include "ft_focus.h"

typedef struct {
    int index, line;              /* line: where [frame N] was read */
    ft_frame_type type;
    int include;
    double ppi;
    int bin;                      /* CCD px per output px; 0 = the fastest
                                     (ft_bin_for_ppi). Smaller = lower
                                     magnification, more light, slower */
    double y0, x0, len, width;    /* window, holder mm */
    double tilt_deg;
    int sprockets, straighten, confidence_low, done;
    long focus_offset;            /* per-frame mode: measured offset */
    int pass;                     /* detailed scan: its pass (1..), 0 none */
    int group;                    /* share job: the window it scans in
                                     (1..); 0 = not in a share job */
    double share_px;              /* share job: this frame's target, px
                                     on its long side; 0 = the strip's
                                     share. One target per group */
} ft_job_frame;

/* Detailed scans (sub-project 2b): what the passes write */
typedef enum { FT_DETAIL_NONE, FT_DETAIL_NEG, FT_DETAIL_POS } ft_detail_out;
typedef enum { FT_DFMT_DNG, FT_DFMT_TIFF } ft_detail_fmt;
typedef enum { FT_BW_OFF, FT_BW_MONO, FT_BW_RGB } ft_detail_bw;
#define FT_RING_MB_DEFAULT 64
#define FT_RING_MB_MIN 8
#define FT_RING_MB_MAX 1024     /* a 32-bit process (ftpoc on XP) has
                                   2 GB of address space in all */

typedef struct {
    char holder[16];
    ft_film film;
    ft_focus_mode focus;
    long focus_offset;            /* shifted mode: CCD steps on the curve */
    int has_film_base;
    double film_base[3];          /* D of unexposed film per channel, from
                                     the preview (ft_layout.base): the
                                     negative's base or the slide's black */
    /* detailed scan; written only when detail != FT_DETAIL_NONE */
    ft_detail_out detail;         /* neg: digital negative, pos: archive
                                     positive (density, ft_density.h) */
    ft_detail_fmt detail_fmt;     /* neg only: DNG or TIFF */
    ft_detail_bw detail_bw;       /* pos only: off (colour), mono, rgb */
    int keep_raw;                 /* also write the undeveloped lines */
    int ring_mb;                  /* RAM between the scanner and the disk */
    double share;                 /* scan to share: target px on each
                                     frame's long side (ft_share.h); 0 =
                                     an ordinary job, one scan per frame */
    int hf_half_res;              /* share: half frames at the FF's ppi */
    int nframes;
    ft_job_frame f[FT_MAX_FRAMES];
} ft_job;

/* j -> text in buf (NUL-terminated if cap > 0); returns the length the
 * whole text needs, without the NUL: >= cap means truncated. FT_JOB_BAD
 * (nothing written) when a frame type or a detail setting is out of
 * range. */
#define FT_JOB_BAD ((size_t)-1)
size_t ft_job_write_buf(const ft_job *j, char *buf, size_t cap);
/* text[0..n) -> j (no NUL needed); errors name the line: "line 3:
 * unknown key bogus". -1 with e filled. */
int ft_job_parse(ft_job *j, const char *text, size_t n, ft_err *e);
/* the same through a stream (ft_job_io.c; not in the browser build) */
int ft_job_write(const ft_job *j, FILE *out);
int ft_job_read(ft_job *j, FILE *in, ft_err *e);
int ft_job_validate(const ft_job *j, const ft_lens *lens, ft_err *e);
/* the bin a frame scans with: its own, or the fastest for its ppi */
int ft_job_bin(const ft_job_frame *f);
/* share job: the frame's target px, its own or the strip's share */
double ft_job_share_px(const ft_job *j, const ft_job_frame *f);
/* detailed scan: the included frames of pass `pass` not yet done; 0 =
 * nothing to scan in it (finished, or no frame) */
int ft_job_pass_todo(const ft_job *j, int pass);

#endif

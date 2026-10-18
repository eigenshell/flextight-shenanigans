/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ftweb: the web page's engine (ftweb.wasm). Everything the page does
 * with pixels and files is here, in C on libft's browser tier; the page's
 * JS only moves bytes between the box (GET/PUT), this engine and the DOM.
 *
 * One strip at a time (one ftw): its strip.job and strip.print, and the
 * share frames cut from its window scans. Cutting and printing go
 * through the same libft calls as ftdev cut + ftdev print --roll
 * (ft_share_cut, ft_share_print_frame), and each frame is kept as the
 * DNG would keep it (16-bit samples, ppi and noise profile as stored,
 * the sidecar through its text), so the JPEG bytes are the CLI's
 * (tests/test_ftweb.py).
 *
 * Calls return 0 (or a count) or -1 with ftw_error() set. Results that
 * are bytes (JSON, file texts, JPEG, zip) go to the out buffer:
 * ftw_out() / ftw_out_len(), valid until the next call that fills it. */
#ifndef FTWEB_H
#define FTWEB_H
#include <stddef.h>

typedef struct ftw ftw;

ftw *ftw_new(void);
void ftw_free(ftw *w);
const char *ftw_error(const ftw *w);
const unsigned char *ftw_out(const ftw *w);
size_t ftw_out_len(const ftw *w);
/* the last print text (ftw_jpeg): the JPEG's .txt and comment */
const char *ftw_text(const ftw *w);

/* the compiled-in emulsions -> out: [{"id","name","mode"}...] */
int ftw_emulsions(ftw *w);

/* the landing page's resolutions, one per ppi[i] -> out: [{"ppi","mp",
 * "px","dmax"}]: mp = a 36x24 frame's megapixels (one decimal), px = its
 * long side (the planner's target, FT_SHARE_FF_LONG_MM), dmax = null (no
 * estimator yet: the page shows #N/A). -1 for a ppi no bin reaches. */
int ftw_resolutions(ftw *w, const double *ppi, int n);
/* the holders -> out: [{"name","label","frames"}] */
int ftw_holders(ftw *w);

/* the planner's focus fit: the box's focus.txt (default: libft's
 * built-in curve, ft_lens_default) */
int ftw_lens(ftw *w, const char *text, size_t n);

/* strip.job. ftw_job_load: the preview's job (strip.preview.job, every
 * frame as detected), the base every plan starts from (the planner
 * excludes frames). ftw_plan_load: the job as planned on the box
 * (strip.job: sizes, windows, done). ftw_plan: re-plan the base with a
 * size per frame (px[i] for its i-th frame: 0 skip, FT_SHARE_4MP,
 * FT_SHARE_6MP, ...); the number of windows. ftw_job_text: the planned
 * job's text (else the base's) -> out. ftw_job_json -> out: {"film":
 * "neg|slide","holder","windows","frames":[{"n","type","low","x","y",
 * "w","h","px","ppi","group","done"}]}: the base's frames, their boxes in the
 * preview image's pixels (strip.png; x = the "x=" line of the preview's
 * strip.raw.txt, NULL/0: the standard preview's), px/ppi/group/done from
 * the plan (0/0/0/false without one). */
int ftw_job_load(ftw *w, const char *text, size_t n);
int ftw_plan_load(ftw *w, const char *text, size_t n);
int ftw_plan(ftw *w, const double *px, int n);
int ftw_job_text(ftw *w);
int ftw_job_json(ftw *w, const char *preview_txt, size_t n);
/* a crop: frame's box in the preview image's pixels (as ftw_job_json
 * gives it: x along, y across, w, h; preview_txt as there) becomes the
 * base job's window, checked as the box checks jobs (inside the holder).
 * -> out: the base job's text (strip.preview.job). The plan is not
 * rerun: call ftw_plan. */
int ftw_frame_box(ftw *w, int frame, const double *box,
                  const char *preview_txt, size_t n);

/* strip.print: load (or start one: ftw_print_new), change the emulsion
 * (frames whose clicks it does not take go back to auto; their count),
 * read and set one frame's clicks (cmyd[4]) and turn (degrees clockwise:
 * 0 90 180 270; ftw_print_rotation returns it), and write (-> out, the
 * file text). ftw_print_emulsion -> out: the emulsion's id. The turn
 * applies to ftw_display, ftw_jpeg and ftw_zip. */
int ftw_print_load(ftw *w, const char *text, size_t n);
int ftw_print_new(ftw *w, const char *emulsion);
int ftw_print_emulsion(ftw *w, const char *emulsion);
int ftw_print_get(ftw *w, int frame, int *cmyd);
int ftw_print_clicks(ftw *w, int frame, int c, int m, int y, int d);
int ftw_print_rotate(ftw *w, int frame, int deg);
int ftw_print_rotation(ftw *w, int frame);
/* a frame's name, its files' name without extension ("" = none: the
 * zip's <base>_frameNN): ftw_print_name -> out; ftw_print_set_name (""
 * clears; file-safe characters only). The zip names frames by it. */
int ftw_print_name(ftw *w, int frame);
int ftw_print_set_name(ftw *w, int frame, const char *name);
int ftw_print_text(ftw *w);

/* one window scan (ftpoc scan --job): its raw lines, the .cal, the .faul
 * (NULL/0: none), the .txt and a defects file (NULL/0: none) -> its
 * frames, cut, replacing frames of the same number. Returns the number
 * of frames cut; out: JSON {"detect_ok","noise_note","frames":[{"frame",
 * "estimated","shift_mm","w","h"}]}. */
int ftw_cut(ftw *w, const unsigned short *raw, size_t raw_n,
            const unsigned short *cal, size_t cal_n, const char *faul,
            size_t faul_n, const char *meta, size_t meta_n,
            const char *defects, size_t defects_n);
/* the landed frames' numbers, ascending, into frames (cap); the count */
int ftw_frames(const ftw *w, int *frames, int cap);

/* the strip's colour balance over every landed frame (ftdev print
 * --roll over them): rerun as frames land. -> out: notes, one per line
 * (the "[frame N] not in the job" warning, few frames, estimated base) */
int ftw_measure(ftw *w);
/* one frame's share JPEG (-> out) and its text (ftw_text) with its
 * clicks from strip.print */
int ftw_jpeg(ftw *w, int frame);
/* the screen copy: the frame printed at long_edge px (never up), sRGB,
 * no NR or sharpening -> out: RGBA rows; *pw x *ph */
int ftw_display(ftw *w, int frame, int long_edge, int *pw, int *ph);
/* the preview (strip.raw, .cal, .faul or NULL/0, strip.raw.txt: a
 * standard 525 ppi preview) with the preview's job (ftw_job_load): each
 * frame's box cut as a share frame is (x along). Returns the number of
 * frames; replaces any earlier preview. ftw_preview_display: one of them
 * printed with strip.print's emulsion, the strip's balance over the
 * preview's frames, the job's film base, no clicks and no turn, at most
 * long_edge px -> out: RGBA rows; *pw x *ph. */
int ftw_preview_load(ftw *w, const unsigned short *raw, size_t raw_n,
                     const unsigned short *cal, size_t cal_n,
                     const char *faul, size_t faul_n, const char *meta,
                     size_t meta_n);
int ftw_preview_display(ftw *w, int frame, int long_edge, int *pw, int *ph);
/* the same with strip.print's clicks and turn: the review's view of a
 * frame before it is scanned */
int ftw_preview_print(ftw *w, int frame, int long_edge, int *pw, int *ph);
/* the standard preview's px per line (strip.raw's lines while it scans) */
int ftw_preview_px(void);
/* the live bar while the preview scans: whole raw lines (px x 3 samples
 * each, as strip.raw holds them) -> out: RGBA rows, one per line; the
 * line count. first: these lines start a preview, their brightest sample
 * per channel is its white until the next first. neg: inverted. No
 * calibration: the colour is only roughly right. */
int ftw_live_rows(ftw *w, const unsigned short *raw, size_t n, int px,
                  int neg, int first);
/* every landed frame's JPEG and text, <base>_frameNN.jpg(.txt), in a
 * store-only zip -> out */
int ftw_zip(ftw *w, const char *base);

/* the page's heap */
void *ftw_alloc(size_t n);
void ftw_release(void *p);

#endif

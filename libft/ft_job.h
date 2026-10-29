/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The job file: what `ftpoc frames` found and `ftpoc scan --job` scans.
 * Plain key = value text; see tools/poc/README.md. '#' always starts a
 * comment, even after a value. */
#ifndef FT_JOB_H
#define FT_JOB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ft.h"
#include "ft_focus.h"

/* One frame of the job: a [frame N] section. */
typedef struct ft_job_frame {
  /* the frame's number, N in [frame N] */
  uint32_t index;
  /* the line [frame N] was read from; 0 when not read from a file */
  uint32_t source_line;
  /* whole, half, panorama or partial */
  ft_frame_type type;
  /* scan this frame */
  bool is_included;
  /* output pixels per inch */
  double ppi;
  /* CCD pixels per output pixel; 0 = the fastest (ft_get_bin_for_ppi).
   * Smaller = lower magnification, more light, slower. */
  int32_t bin;
  /* the scan window, holder mm: along the strip from along_start_mm,
   * length_mm long; across from across_start_mm, width_mm wide */
  double along_start_mm;
  double across_start_mm;
  double length_mm;
  double width_mm;
  /* how far the frame is turned, degrees */
  double tilt_degrees;
  /* the frame has sprocket holes in the window */
  bool has_sprockets;
  /* straighten the frame by tilt_degrees */
  bool should_straighten;
  /* the detection was unsure about this frame */
  bool is_confidence_low;
  /* the frame was scanned already */
  bool is_done;
  /* per-frame focus mode: the measured offset, tandem steps */
  int64_t focus_offset_steps;
  /* detailed scan: its pass (1..); 0 = none */
  int32_t pass;
  /* share job: the window it scans in (1..); 0 = not in a share job */
  int32_t group;
  /* share job: this frame's target, pixels on its long side; 0 = the
   * strip's share. One target per group. */
  double share_pixels;
} ft_job_frame;

/* Detailed scans (sub-project 2b): what the passes write. */
typedef enum ft_detail_output {
  /* not a detailed scan */
  FT_DETAIL_NONE,
  /* a digital negative */
  FT_DETAIL_NEGATIVE,
  /* an archive positive (density, ft_density.h) */
  FT_DETAIL_POSITIVE
} ft_detail_output;

/* The file format of a digital negative. */
typedef enum ft_detail_format {
  FT_DETAIL_FORMAT_DNG,
  FT_DETAIL_FORMAT_TIFF
} ft_detail_format;

/* Black and white for an archive positive. */
typedef enum ft_detail_bw {
  /* colour */
  FT_BW_OFF,
  /* one channel */
  FT_BW_MONO,
  /* grey in three channels */
  FT_BW_RGB
} ft_detail_bw;

/* RAM between the scanner and the disk, megabytes */
#define FT_RING_MEGABYTES_DEFAULT 64
#define FT_RING_MEGABYTES_MIN 8
/* a 32-bit process (Sancho on XP) has 2 GB of address space in all */
#define FT_RING_MEGABYTES_MAX 1024

/* A whole job: the [strip] section and its frames. */
typedef struct ft_job {
  /* the holder's name (ft_find_holder) */
  char holder[16];
  /* negative or slide */
  ft_film film;
  /* one shifted focus, or focus per frame */
  ft_focus_mode focus_mode;
  /* shifted mode: the offset from the curve, tandem steps */
  int64_t focus_offset_steps;
  /* film_base_density is filled in */
  bool has_film_base;
  /* density of unexposed film per channel, from the preview
   * (ft_layout's base): the negative's base or the slide's black */
  double film_base_density[3];
  /* detailed scan; written to the file only when detail is not
   * FT_DETAIL_NONE */
  ft_detail_output detail;
  /* negative only: DNG or TIFF */
  ft_detail_format detail_format;
  /* positive only: off (colour), mono or rgb */
  ft_detail_bw detail_bw;
  /* also write the undeveloped lines */
  bool keep_raw;
  /* RAM between the scanner and the disk, megabytes */
  int32_t ring_megabytes;
  /* scan to share: target pixels on each frame's long side
   * (ft_share.h); 0 = an ordinary job, one scan per frame */
  double share_pixels;
  /* share: scan half frames at the full frame's ppi */
  bool half_frames_at_full_frame_ppi;
  /* frames in frames[] */
  uint32_t frame_count;
  ft_job_frame frames[FT_MAX_FRAMES];
} ft_job;

/* ft_format_job's answer when a field is out of range */
#define FT_JOB_BAD ((size_t)-1)

/* Write a job as text.
 *
 * text           receives the text, NUL-terminated when capacity > 0
 * text_capacity  bytes available in text
 *
 * Returns the length the whole text needs, without the NUL (>=
 * text_capacity: cut short), or FT_JOB_BAD (nothing written) when a frame
 * type or a detail setting is out of range. */
size_t ft_format_job(const ft_job* job, char* text, size_t text_capacity);

/* Read a job from text; no NUL needed. Errors name the line: "line 3:
 * unknown key bogus". Returns 0, or -1 with error filled. */
int32_t ft_parse_job(ft_job* job, const char* text, size_t text_length,
                     ft_err* error);

/* The same as ft_format_job and ft_parse_job, through a stream
 * (ft_job_io.c; not in the browser build). Return 0, or -1. */
int32_t ft_write_job(const ft_job* job, FILE* file);
int32_t ft_read_job(ft_job* job, FILE* file, ft_err* error);

/* Check a job can be scanned: known holder, windows inside it, ppi and
 * bin usable, focus positions safe for lens, the detail and share rules.
 * Returns 0, or -1 with error filled. */
int32_t ft_validate_job(const ft_job* job, const ft_lens* lens, ft_err* error);

/* The bin a frame scans with: its own, or the fastest for its ppi. */
int32_t ft_get_frame_bin(const ft_job_frame* frame);

/* Share job: the frame's target pixels, its own or the strip's. */
double ft_get_frame_share_pixels(const ft_job* job, const ft_job_frame* frame);

/* Detailed scan: count the included frames of a pass not yet done.
 * 0 = nothing to scan in it (finished, or no frame). */
uint32_t ft_count_pass_frames_to_do(const ft_job* job, int32_t pass);

#endif

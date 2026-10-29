/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Scan to share, after the scan: a window scan (several selected frames,
 * ftpoc scan --job of a share job) -> one working raw per frame.
 *
 * The film creeps between runs (the holder travels out and back for every
 * scan's calibration), so the preview's frame boxes are only a guess:
 * the window is scanned with a margin and the frames are detected again
 * in its own pixels (ft_detect_frames on a ~500 ppi copy), matched to the
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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ft.h"
#include "ft_develop.h"
#include "ft_image.h"
#include "ft_job.h"
#include "ft_noise_model.h"
#include "ft_share.h"

/* A window scan's .txt, read back. */
typedef struct ft_share_metadata {
  /* the scan (Sancho's own keys): ppi, bin, pixels per line, the first
   * output pixel across and the first line along */
  double ppi;
  int32_t bin;
  int32_t pixel_count;
  int64_t first_pixel;
  int64_t first_line;
  /* the share keys */
  ft_film film;
  char holder[16];
  double share_pixels;
  bool half_frames_at_full_frame_ppi;
  bool has_film_base;
  double film_base_density[3];
  /* the frames of the window, as the preview saw them */
  uint32_t frame_count;
  ft_share_frame frames[FT_MAX_FRAMES];
} ft_share_metadata;

/* Read a window scan's .txt.
 * Returns 0, or -1 with error naming the key when a value is bad or a
 * required key (ppi px x y bin share, at least one share_frame) is
 * missing. */
int32_t ft_parse_share_metadata(const char* text, size_t text_length,
                                ft_share_metadata* metadata, ft_err* error);

/* Append the share keys for group of job to the scan's .txt.
 * Returns 0, or -1 on a write error. */
int32_t ft_write_share_metadata(FILE* file, const ft_job* job, int32_t group);

/* One frame cut from the window. */
typedef struct ft_share_cut_frame {
  /* the job's frame number */
  uint32_t index;
  ft_frame_type type;
  /* the detection missed it: the preview box, moved */
  bool is_estimated;
  /* along: where it was found - the preview */
  double shift_mm;
  /* x y width height: the frame in image, bleed excluded; x along the
   * strip, y across (as image) */
  int32_t box[4];
  /* min(1, target pixels / the box's long side) */
  double export_scale;
  /* the frame and its bleed: linear transmittance */
  ft_image image;
} ft_share_cut_frame;

/* The frames cut from a window. */
typedef struct ft_share_cut_result {
  /* false: the detection failed, every frame is estimated */
  bool was_detected;
  /* ft_measure_noise_profile's answer */
  ft_noise_result noise_result;
  /* its note when noise_result is not FT_NOISE_OK */
  ft_err noise_error;
  double noise_profile[6];
  uint32_t frame_count;
  ft_share_cut_frame frames[FT_MAX_FRAMES];
} ft_share_cut_result;

/* Develop the window, detect, and cut every frame of the metadata.
 *
 * raw          lines x pixel_count x 3
 * calibration  calibration_lines x pixel_count x 3
 * result       receives the frames; free it with ft_free_share_cut
 *
 * Returns 0, or -1 with error filled when the scan cannot be developed or
 * a frame's box leaves the scan entirely. */
int32_t ft_cut_share_frames(const uint16_t* raw, uint32_t lines,
                            const uint16_t* calibration,
                            uint32_t calibration_lines, const char* faulty_dump,
                            size_t faulty_dump_length,
                            const ft_defects* defects,
                            const ft_share_metadata* metadata,
                            ft_share_cut_result* result, ft_err* error);

/* Free the frames' images. */
void ft_free_share_cut(ft_share_cut_result* result);

/* A working raw's sidecar, read back. */
typedef struct ft_share_sidecar {
  uint32_t frame;
  ft_frame_type type;
  ft_film film;
  double ppi;
  double share_pixels;
  double export_scale;
  double shift_mm;
  bool half_frames_at_full_frame_ppi;
  bool is_estimated;
  bool has_film_base;
  double film_base_density[3];
  int32_t box[4];
  /* noise_profile= was given, all finite, S >= 0, O > 0 */
  bool has_noise_profile;
  double noise_profile[6];
} ft_share_sidecar;

/* Write the sidecar text into text, as snprintf does: the full length is
 * returned, at most capacity - 1 bytes and a NUL are written; text may be
 * NULL with capacity 0. */
int32_t ft_format_share_sidecar(char* text, size_t capacity,
                                const ft_share_metadata* metadata,
                                const ft_share_cut_frame* frame);

/* The same into a stream (ft_share_cut_io.c). Returns 0, or -1. */
int32_t ft_write_share_sidecar(FILE* file, const ft_share_metadata* metadata,
                               const ft_share_cut_frame* frame);

/* Read a sidecar.
 * Returns 0, or -1 with error naming the key when box or export_scale is
 * missing or bad, or noise_profile has not six numbers. A noise_profile
 * out of range reads as none (has_noise_profile false). */
int32_t ft_parse_share_sidecar(const char* text, size_t text_length,
                               ft_share_sidecar* sidecar, ft_err* error);

#endif

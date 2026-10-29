/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The share text formats into streams (Sancho's side: stdio); the formats
 * are ft_share_cut.c's. */
#include <stdint.h>
#include <stdio.h>

#include "ft_share_cut.h"

/* the frame type words, in the order of ft_frame_type */
static const char* const TYPE_WORDS[] = {"full", "half", "panorama", "partial"};
#define TYPE_COUNT (sizeof TYPE_WORDS / sizeof TYPE_WORDS[0])
/* room for one sidecar */
#define SIDECAR_BYTES 1024

int32_t ft_write_share_metadata(FILE* file, const ft_job* job, int32_t group) {
  /* the group's target: the first included frame's */
  double share_pixels = job->share_pixels;
  for(uint32_t index = 0; index < job->frame_count; index++) {
    const ft_job_frame* frame = &job->frames[index];
    if(frame->is_included && frame->group == group) {
      share_pixels = ft_get_frame_share_pixels(job, frame);
      break;
    }
  }
  fprintf(file, "film=%s\nholder=%s\nshare=%g\nhf_half_res=%d\n",
          job->film == FT_FILM_SLIDE ? "slide" : "negative", job->holder,
          share_pixels, job->half_frames_at_full_frame_ppi ? 1 : 0);
  if(job->has_film_base) {
    fprintf(file, "film_base=%.4f %.4f %.4f\n", job->film_base_density[0],
            job->film_base_density[1], job->film_base_density[2]);
  }
  for(uint32_t index = 0; index < job->frame_count; index++) {
    const ft_job_frame* frame = &job->frames[index];
    /* the cast to unsigned also catches values below 0 */
    if(frame->is_included == false ||
       frame->group != group ||
       (unsigned)frame->type >= TYPE_COUNT) {
      continue;
    }
    fprintf(file, "share_frame=%u %s %.3f %.3f %.3f %.3f\n",
            (unsigned)frame->index, TYPE_WORDS[frame->type],
            frame->along_start_mm, frame->length_mm, frame->across_start_mm,
            frame->width_mm);
  }
  return ferror(file) ? -1 : 0;
}

int32_t ft_write_share_sidecar(FILE* file, const ft_share_metadata* metadata,
                               const ft_share_cut_frame* frame) {
  char text[SIDECAR_BYTES];
  int32_t length = ft_format_share_sidecar(text, sizeof text, metadata, frame);
  if(length < 0 || length >= (int32_t)sizeof text) return -1;
  if(fputs(text, file) == EOF || ferror(file)) return -1;
  return 0;
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The share text formats into streams (box tier: stdio); the formats are
 * ft_share_cut.c's. */
#include "ft_share_cut.h"
#include <stdio.h>

static const char *TYPES[] = {"full", "half", "panorama", "partial"};

int ft_share_meta_write(FILE *out, const ft_job *j, int g) {
  double share = j->share; /* the group's target */
  int i;
  for(i = 0; i < j->nframes; i++)
    if(j->f[i].include && j->f[i].group == g) {
      share = ft_job_share_px(j, &j->f[i]);
      break;
    }
  fprintf(
      out, "film=%s\nholder=%s\nshare=%g\nhf_half_res=%d\n",
      j->film == FT_FILM_SLIDE ? "slide" : "negative", j->holder, share,
      j->hf_half_res ? 1 : 0
  );
  if(j->has_film_base)
    fprintf(
        out, "film_base=%.4f %.4f %.4f\n", j->film_base[0], j->film_base[1],
        j->film_base[2]
    );
  for(i = 0; i < j->nframes; i++) {
    const ft_job_frame *f = &j->f[i];
    if(!f->include || f->group != g || (unsigned)f->type > 3)
      continue;
    fprintf(
        out, "share_frame=%d %s %.3f %.3f %.3f %.3f\n", f->index,
        TYPES[f->type], f->y0, f->len, f->x0, f->width
    );
  }
  return ferror(out) ? -1 : 0;
}

int ft_share_sidecar_write(
    FILE *out, const ft_share_meta *m, const ft_share_cut_frame *c
) {
  char buf[1024];
  int n = ft_share_sidecar_write_buf(buf, sizeof buf, m, c);
  if(n < 0 || n >= (int)sizeof buf)
    return -1;
  return fputs(buf, out) == EOF || ferror(out) ? -1 : 0;
}

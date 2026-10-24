/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * libft: platform-independent scanner logic for ftpoc. No hardware I/O.
 * Functions that can fail take an ft_err * and return -1 after filling it
 * with ft_fail(); 0 (or a documented value) on success.
 */
#ifndef FT_H
#define FT_H

typedef struct {
  char msg[200];
} ft_err;
#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
int ft_fail(ft_err* e, const char* fmt, ...);

typedef enum { FT_FILM_NEGATIVE, FT_FILM_SLIDE } ft_film;
typedef enum {
  FT_FRAME_FULL,
  FT_FRAME_HALF,
  FT_FRAME_PANORAMA,
  FT_FRAME_PARTIAL
} ft_frame_type;
typedef enum { FT_FOCUS_SHIFTED, FT_FOCUS_PER_FRAME } ft_focus_mode;

#define FT_MAX_FRAMES 64

/* Transmittance floor, 10^-4.5 (density 4.5): just below the ~3.6 per-read
 * Dmax, so log10 of a noisy dense pixel stays finite */
#define FT_T_FLOOR 3.16227766e-5

#endif

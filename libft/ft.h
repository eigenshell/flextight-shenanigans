/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libft: the scanner logic that runs on every platform. No hardware I/O.
 *
 * Functions that can fail take an ft_err* and, on failure, fill it with
 * ft_fail() and return -1. On success they return 0, or a value their
 * header documents. */
#ifndef FT_H
#define FT_H

#include <stdint.h>

/* What went wrong, as text for the user. */
typedef struct ft_err {
  /* the message; empty when nothing failed */
  char message[200];
} ft_err;

/* Fill error with a printf-style message and return -1.
 *
 * error   receives the message; may be NULL (the message is dropped)
 * format  a printf format, followed by its values
 *
 * Always returns -1, so a caller can write `return ft_fail(...)`. */
#ifdef __GNUC__
/* lets the compiler check format against the values, like printf */
__attribute__((format(printf, 2, 3)))
#endif
int32_t ft_fail(ft_err* error, const char* format, ...);

/* The kind of film in the holder. */
typedef enum ft_film {
  /* colour or black-and-white negative: inverted when rendered */
  FT_FILM_NEGATIVE,
  /* slide (positive): rendered as it is */
  FT_FILM_SLIDE
} ft_film;

/* How much of a full frame a detected frame covers. */
typedef enum ft_frame_type {
  /* a whole frame */
  FT_FRAME_FULL,
  /* half a frame (half-frame camera) */
  FT_FRAME_HALF,
  /* a panorama: longer than a full frame */
  FT_FRAME_PANORAMA,
  /* a frame cut off by the strip's end */
  FT_FRAME_PARTIAL
} ft_frame_type;

/* How the focus is set for a strip. */
typedef enum ft_focus_mode {
  /* one focus for the strip, shifted from the measured one */
  FT_FOCUS_SHIFTED,
  /* a focus measured for every frame */
  FT_FOCUS_PER_FRAME
} ft_focus_mode;

/* the most frames one strip can hold */
#define FT_MAX_FRAMES 64

/* Transmittance floor, 10^-4.5 (density 4.5): just below the ~3.6 per-read
 * Dmax, so log10 of a noisy dense pixel stays finite. */
#define FT_TRANSMITTANCE_FLOOR 3.16227766e-5

#endif

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Dmax estimate for a scan at a given ppi: the most light any usable bin
 * gives (ft_plan's model: ft_light_rel, ft_dmax_est), within optics
 * limits the caller passes (today FT_SCALE_MIN, FT_SCALE_MAX and
 * ft_lens_default; the focus floor may move). A rough estimate from the
 * magnification only: no estimate where no bin fits, never a fallback. */
#ifndef FT_DMAX_H
#define FT_DMAX_H
#include "ft_focus.h"

typedef struct {
  double scale_min, scale_max; /* optical scale (ppi x bin) range */
  const ft_lens *lens;
} ft_optics_limits;

/* 1 and *dmax if some bin in {12,6,4,3,2,1} puts the scale ppi x bin in
 * [scale_min, scale_max] (1e-6 tolerance, as ft_bins_for_ppi): the
 * per-read Dmax (R) at the lowest such scale (the most light); 0 and
 * *dmax untouched otherwise (also non-finite or <= 0 ppi, no limits or
 * no lens) */
int ft_dmax_for_ppi(const ft_optics_limits *lim, double ppi, double *dmax);

#endif

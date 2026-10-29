/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Dmax estimate for a scan at a given ppi.
 *
 * Dmax is the densest film the scan can still see through. It depends on
 * how much light reaches each sample, and that depends on the optical
 * scale (ppi x bin) the scanner reads at. This is a rough estimate from
 * the magnification only, using ft_plan's light model
 * (ft_estimate_relative_light, ft_estimate_dmax_for_light). Where no bin fits
 * the optics limits there is no estimate, and never a fallback value. */
#ifndef FT_DMAX_H
#define FT_DMAX_H

#include <stdbool.h>

#include "ft_focus.h"

/* The optical scales the scanner can reach. The caller passes them,
 * because they can change (today FT_SCALE_MIN_PPI, FT_SCALE_MAX_PPI and
 * ft_lens_default; the focus floor may move). */
typedef struct ft_optics_limits {
  /* the lowest optical scale (ppi x bin) the optics can do */
  double min_scale_ppi;
  /* the highest optical scale (ppi x bin) the optics can do */
  double max_scale_ppi;
  /* the lens, for the light model */
  const ft_lens* lens;
} ft_optics_limits;

/* Estimate the per-read Dmax (R) of a scan at ppi.
 *
 * Tries the bins 1, 2, 3, 4, 6 and 12, smallest first, and uses the
 * first one whose scale ppi x bin lies within limits (with the same
 * 1e-6 tolerance as ft_find_bins_for_ppi). The smallest scale gives the most
 * light, so the best Dmax.
 *
 * limits  the optics limits to stay within
 * ppi     the output resolution of the scan
 * dmax    receives the estimate
 *
 * Returns true and fills in *dmax when a bin fits. Returns false and
 * leaves *dmax untouched when no bin fits, when ppi is not a positive
 * number, or when limits or its lens is NULL. */
bool ft_estimate_dmax(const ft_optics_limits* limits, double ppi, double* dmax);

#endif

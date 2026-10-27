/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_dmax.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ft_plan.h"

/* Slack when comparing a scale with the limits, the same as
 * ft_bins_for_ppi uses: a scale of 1500.0000001 still counts as 1500. */
#define SCALE_TOLERANCE 1e-6

/* The bins the scanner can read with, smallest first: the smallest bin
 * gives the lowest scale, so the most light. */
static const uint32_t BINS[] = {1, 2, 3, 4, 6, 12};

bool ft_estimate_dmax(const ft_optics_limits* limits, double ppi,
                      double* dmax) {
  if(limits == NULL || limits->lens == NULL) return false;
  /* isfinite is false for NAN and INFINITY */
  if(isfinite(ppi) == false || ppi <= 0) return false;

  for(size_t index = 0; index < sizeof BINS / sizeof BINS[0]; index++) {
    double scale_ppi = ppi * BINS[index];
    bool is_too_low = scale_ppi < limits->min_scale_ppi - SCALE_TOLERANCE;
    bool is_too_high = scale_ppi > limits->max_scale_ppi + SCALE_TOLERANCE;
    if(is_too_low == false && is_too_high == false) {
      *dmax = ft_dmax_est(ft_light_rel(limits->lens, scale_ppi));
      return true;
    }
  }
  return false;
}

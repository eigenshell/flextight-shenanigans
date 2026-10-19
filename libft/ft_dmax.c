/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stddef.h>
#include "ft_dmax.h"
#include "ft_plan.h"

int ft_dmax_for_ppi(const ft_optics_limits *lim, double ppi, double *dmax)
{
    /* smallest bin first: the lowest scale, the most light */
    static const int all[] = { 1, 2, 3, 4, 6, 12 };
    int i;
    if (!lim || !lim->lens || !isfinite(ppi) || !(ppi > 0))
        return 0;
    for (i = 0; i < 6; i++) {
        double scale = ppi * all[i];
        if (scale >= lim->scale_min - 1e-6 &&
            scale <= lim->scale_max + 1e-6) {
            *dmax = ft_dmax_est(ft_light_rel(lim->lens, scale));
            return 1;
        }
    }
    return 0;
}

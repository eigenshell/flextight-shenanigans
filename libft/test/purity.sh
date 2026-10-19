#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# The render core and the 2b encoders also run in the browser (WASM), so
# their objects may import
# only:
#   - libm (clang lowers exp/pow to exp2 and the like, so the list is wide)
#   - mem*, strcmp/strtod (ft_curve's parser) and strlen (ft_icc); WASM
#     libcs provide them
#   - libft's own render, share, plan, output, ICC, a-trous, NR and print
#     units and ft_fail
#   - __stack_chk_fail, the native stack protector (WASM builds use
#     -fno-stack-protector)
# never malloc, stdio or threads. Reads Linux/ELF `nm -u` output (macOS nm
# prefixes symbols with `_`: not supported).
# Usage: sh test/purity.sh file.o...
bad=$(nm -u "$@" | awk 'NF == 2 { print $2 }' |
      grep -Ev '^(ft_fail|ft_curve_[a-z_]+|ft_recipe_[a-z_]+|ft_render_[a-z_]+|ft_dens_[a-z0-9_]+|ft_dplan_[a-z_]+|ft_dscan_[a-z_]+|ft_share_[a-z_]+|ft_(space|mat3|rgb|xyz|oklab|output|icc|resize)_[a-z0-9_]+|ft_atrous(_[a-z_]+)?|FT_ATROUS_[A-Z]+|ft_nr_[a-z_]+|ft_(print|pm|emulsion)_[a-z0-9_]+|ft_srgb_(en|de)code|ft_(bin_ok|bins_for_ppi|scan_seconds|line_seconds|mm_to_ccd|ccd_to_mm|out_to_ccd|light_rel|dmax_est|dmax_for_ppi)|mem(cpy|set|move|cmp)|strcmp|strlen|strtod|(exp|exp2|expm1|log|log2|log10|log1p|pow|sqrt|cbrt|ceil|floor|round|lround|trunc|rint|lrint|nearbyint|fabs|fmin|fmax|fmod|sin|cos|sincos|tan|atan|atan2)f?|__stack_chk_fail)$' |
      sort -u)
if [ -n "$bad" ]; then
    echo "purity: forbidden imports in $*:" $bad
    exit 1
fi
echo "purity: $* call only libm, mem*, strcmp/strlen/strtod and libft"

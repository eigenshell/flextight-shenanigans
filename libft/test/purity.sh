#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# The render core and the 2b encoders also run in the browser (WASM), so
# their objects may import only:
#   - libm (clang lowers exp/pow to exp2 and the like, so the list is wide)
#   - mem*, strcmp/strtod (ft_curve's parser) and strlen (ft_icc); WASM
#     libcs provide them
#   - each other: any symbol one of the checked objects defines, and
#     ft_fail
#   - __stack_chk_fail, the native stack protector (WASM builds use
#     -fno-stack-protector)
# never malloc, stdio or threads. Reads Linux/ELF `nm` output (macOS nm
# prefixes symbols with `_`: not supported).
# Usage: sh test/purity.sh file.o...
defined=$(nm --defined-only "$@" | awk 'NF == 3 { print $3 }' | sort -u)
bad=$(nm -u "$@" | awk 'NF == 2 { print $2 }' |
      grep -Ev '^(ft_fail|mem(cpy|set|move|cmp)|strcmp|strlen|strtod|(exp|exp2|expm1|log|log2|log10|log1p|pow|sqrt|cbrt|ceil|floor|round|lround|trunc|rint|lrint|nearbyint|fabs|fmin|fmax|fmod|sin|cos|sincos|tan|atan|atan2)f?|__stack_chk_fail)$' |
      sort -u | comm -23 - "$(printf '%s\n' "$defined" > "${TMPDIR:-/tmp}/purity.$$"; echo "${TMPDIR:-/tmp}/purity.$$")")
rm -f "${TMPDIR:-/tmp}/purity.$$"
if [ -n "$bad" ]; then
    echo "purity: forbidden imports in $*:" $bad
    exit 1
fi
echo "purity: $* call only libm, mem*, strcmp/strlen/strtod and libft"

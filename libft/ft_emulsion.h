/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The emulsions a user can pick for a roll (ft_print.h): ids and names
 * are all a user sees; the chains are developers' science (ft_pm.h).
 * Change an entry, a module or a .cube (libft/emulsions/, `make luts`)
 * and bump FT_EMULSION_REV: every print records it, so an old JPEG says
 * which science made it. Pure C99. */
#ifndef FT_EMULSION_H
#define FT_EMULSION_H
#include "ft_print.h"

#define FT_EMULSION_REV 1

/* NULL if no emulsion has that id */
const ft_emulsion* ft_emulsion_find(const char* id);
/* the i-th emulsion (0..), NULL past the last */
const ft_emulsion* ft_emulsion_at(int i);

#endif

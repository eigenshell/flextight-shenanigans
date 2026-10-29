/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The emulsions a user can pick for a roll (ft_print.h): ids and names
 * are all a user sees; the chains are developers' science (ft_pm.h).
 * Change an entry, a module or a .cube (libft/emulsions/, `make luts`)
 * and bump FT_EMULSION_REV: every print records it, so an old JPEG says
 * which science made it. Pure C99. */
#ifndef FT_EMULSION_H
#define FT_EMULSION_H

#include <stdint.h>

#include "ft_print.h"

/* bump when an emulsion, a module or a .cube changes */
#define FT_EMULSION_REV 1

/* Find an emulsion by id. Returns NULL when no emulsion has that id. */
const ft_emulsion* ft_find_emulsion(const char* id);

/* The index-th emulsion (from 0); NULL past the last. */
const ft_emulsion* ft_get_emulsion_at(uint32_t index);

#endif

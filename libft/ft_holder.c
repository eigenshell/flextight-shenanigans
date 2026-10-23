/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_holder.h"
#include <string.h>

static const ft_holder HOLDERS[] = {
    {"24x217", 231.0, 32.0, 2.02, 38.02, 40.0, 6,
     "Filmchrome 24x36x6 (25.2 x 226.8 mm), 6 frames 35 mm with borders", 5.5,
     25.5, 1.5, 29.5},
};
#define NHOLDERS (int)(sizeof HOLDERS / sizeof HOLDERS[0])

const ft_holder *ft_holder_at(int i) {
  return i >= 0 && i < NHOLDERS ? &HOLDERS[i] : NULL;
}

const ft_holder *ft_holder_find(const char *name) {
  size_t i;
  if(!name)
    return NULL;
  for(i = 0; i < (size_t)NHOLDERS; i++)
    if(strcmp(HOLDERS[i].name, name) == 0)
      return &HOLDERS[i];
  return NULL;
}

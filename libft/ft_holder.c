/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_holder.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Every holder we know. The grid values are FlexColor's 3f autoframe
 * converted from 630 ppi lines to mm (lines / 630 * 25.4):
 * 50 lines = 2.02 mm, 943 lines = 38.02 mm, 992 lines = 40.0 mm. */
static const ft_holder HOLDERS[] = {
  {
    .name = "24x217",
    .scan_length_mm = 231.0,
    .scan_width_mm = 32.0,
    .grid_first_frame_mm = 2.02,
    .grid_pitch_mm = 38.02,
    .grid_frame_length_mm = 40.0,
    .grid_frame_count = 6,
    .label = "Filmchrome 24x36x6 (25.2 x 226.8 mm), "
             "6 frames 35 mm with borders",
    .band_start_mm = 5.5,
    .band_end_mm = 25.5,
    .image_start_mm = 1.5,
    .image_end_mm = 29.5,
  },
};

/* The number of holders in the list: the size of the whole array in
 * bytes divided by the size of one holder. */
#define HOLDER_COUNT (sizeof HOLDERS / sizeof HOLDERS[0])

const ft_holder* ft_get_holder_at(uint32_t index) {
  if(index >= HOLDER_COUNT) return NULL;
  return &HOLDERS[index];
}

const ft_holder* ft_find_holder(const char* name) {
  if(name == NULL) return NULL;
  for(uint32_t index = 0; index < HOLDER_COUNT; index++) {
    /* strcmp returns 0 when the two texts are the same */
    if(strcmp(HOLDERS[index].name, name) == 0) return &HOLDERS[index];
  }
  return NULL;
}

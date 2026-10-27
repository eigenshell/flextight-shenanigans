/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FT_HOLDER_H
#define FT_HOLDER_H

#include <stdint.h>

/* A film holder: the strip carrier that goes into the scanner.
 * All sizes are in mm. "Along" is the scan direction (the way the drum
 * moves), "across" is along the CCD line. */
typedef struct ft_holder {
  /* short id used in job files and on the command line, e.g. "24x217" */
  const char* name;
  /* along: crop windows must stay inside 0..scan_length_mm */
  double scan_length_mm;
  /* across: crop windows must stay inside 0..scan_width_mm */
  double scan_width_mm;

  /* The grid_* fields are a fixed frame grid, used when detection finds
   * no frame edge. It is FlexColor's "3f autoframe": first frame at line
   * 50, pitch 943 lines, frames 992 lines long, all at 630 ppi. The last
   * grid frame runs past scan_length_mm (FlexColor does the same), so
   * callers clip grid frames to the film. */

  /* where the first grid frame starts */
  double grid_first_frame_mm;
  /* distance from the start of one grid frame to the start of the next */
  double grid_pitch_mm;
  /* length of one grid frame */
  double grid_frame_length_mm;
  /* how many frames the grid has */
  uint32_t grid_frame_count;

  /* the description the page shows */
  const char* label;

  /* The band_* and image_* fields are across, for the streaming detector
   * (ft_stream_detect.h). The film's place across changes from load to
   * load: the image started at 2.0 mm and at 4.6 mm on two strips
   * (hardware, 2026-10-18). */

  /* start of the band that is image on every load; lines are classified
   * on this band */
  double band_start_mm;
  /* end of that band */
  double band_end_mm;
  /* start of the zone where the image may lie; a frame's extent across
   * is looked for here */
  double image_start_mm;
  /* end of that zone */
  double image_end_mm;
} ft_holder;

/* Find a holder by its name, e.g. "24x217".
 * Returns NULL when name is NULL or no holder has that name. */
const ft_holder* ft_find_holder(const char* name);

/* The holder at position index in the list, counting from 0.
 * Returns NULL past the last holder, so a caller can list them all by
 * counting up until it gets NULL. */
const ft_holder* ft_get_holder_at(uint32_t index);

#endif

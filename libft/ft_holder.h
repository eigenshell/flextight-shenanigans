/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FT_HOLDER_H
#define FT_HOLDER_H

/* Holder geometry, all mm. scan_* are the limits crop windows must stay
 * in; grid_* is the fixed frame grid used when detection finds no edge
 * (FlexColor's 3f autoframe: first frame 50 lines, pitch 943 lines,
 * 992-line frames at 630 ppi). The last grid frame overshoots scan_len_mm
 * (FlexColor does the same); callers clip grid frames to the film extent. */
typedef struct {
    const char *name;
    double scan_len_mm, scan_width_mm;
    double grid_first_mm, grid_pitch_mm, grid_frame_mm;
    int grid_frames;
    const char *label;          /* what the page shows */
} ft_holder;

const ft_holder *ft_holder_find(const char *name);
/* the i-th holder (0..), NULL past the last */
const ft_holder *ft_holder_at(int i);

#endif

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Auto-estimates that fill in a render recipe (ft_render.h). They never
 * render anything: every value they set is visible and can be
 * overridden. Statistics come from the developed frame(s), 2 % in from
 * each edge, as histograms of 4x4 block densities (0.001 D bins), reading
 * at most ~4 M sampled pixels, so no allocation. Blocks that are not film
 * are skipped: holder metal (a block mean at 4 D, or >= 4 of its 16 pixels
 * at T <= 0.002 in every channel, which also catches a block on a metal
 * edge; the slide base only skips the T floor), blocks with a non-finite
 * value and, for the film base, any clear pixel (open beam, T > 0.9 in
 * every channel). Limit: film denser than ~2.8 D looks like metal to the
 * scanner and cannot be told apart.
 * Several frames pool their statistics (one setting for a roll). The film
 * base normally comes from the preview (ft_layout's base_transmittance,
 * job key film_base); ft_estimate_base_from_frames is the fallback when a scan
 * has none. */
#ifndef FT_RENDER_AUTO_H
#define FT_RENDER_AUTO_H

#include <stdint.h>

#include "ft_image.h"
#include "ft_render.h"

/* Estimate the film base per channel from the frames themselves:
 * negative and b&w the 0.5th percentile of density (the thinnest film),
 * slide the 99.5th (the densest; only blocks at the T floor are
 * skipped). Clamped to 0..4.5 D.
 *
 * images       image_count frames, pooled
 * density      receives the estimate
 *
 * Returns 0, or -1 with error filled when the frames have no pixels. */
int32_t ft_estimate_base_from_frames(const ft_image* const* images,
                                     uint32_t image_count, ft_render_mode mode,
                                     float density[3], ft_err* error);

/* Fill in a recipe's range, exposure and tint from the frames.
 *
 * Negative and b&w: the brightest 0.5 % of the blocks (densest film),
 * picked on the mean density of the three channels (which favours blocks
 * dense in the high-gamma, blue layer); range = the median density,
 * measured on a block 2 blocks to the right of each pick, above base
 * (0.05 .. 4.5; about the 99.75th percentile). A channel with no usable
 * neighbour falls back to its own unfiltered 99.5th percentile. Exposure
 * puts that highlight at paper white + 0.03 D (clamped to +-6; 0 for a
 * grade <= 0). Slide: exposure 0. Tint is set to 0. The recipe's mode
 * and base must be set.
 *
 * Returns 0, or -1 with error filled when the frames have no pixels. */
int32_t ft_estimate_recipe(const ft_image* const* images, uint32_t image_count,
                           ft_recipe* recipe, ft_err* error);

/* Negative and b&w: the exposure that puts the highlight (the frame's
 * range above base) at paper white + 0.03 D, from the recipe's
 * range_density[1], grade and paper; clamped to +-6 stops.
 * ft_estimate_recipe sets this after the range. */
float ft_estimate_auto_exposure(const ft_recipe* recipe);

#endif

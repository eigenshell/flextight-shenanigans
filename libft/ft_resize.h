/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Downscale in linear light: separable Lanczos-3, the kernel stretched by
 * the reduction (an antialiasing filter), weights renormalised at the
 * edges. Linear light keeps the mean: a fine black/white pattern shrinks
 * to its 0.5 grey, where a resize of gamma-encoded values darkens it.
 * Never upscales. Pure: the caller provides the scratch memory (the
 * horizontal pass: h x out_w pixels). */
#ifndef FT_RESIZE_H
#define FT_RESIZE_H
#include <stddef.h>

#include "ft.h"

/* output size for scale s (0 < s <= 1): round(w s) x round(h s), each at
 * least 1 */
void ft_resize_dims(int w, int h, double s, int* ow, int* oh);
size_t ft_resize_scratch_bytes(int w, int h, int ow, int oh);
/* in: w x h RGB floats, out: ow x oh; -1 with e filled if ow > w,
 * oh > h or a size is < 1. ow == w and oh == h copies. */
int ft_resize(const float* in, int w, int h, float* out, int ow, int oh,
              void* scratch, ft_err* e);

#endif

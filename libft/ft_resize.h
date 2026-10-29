/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Downscale in linear light: separable Lanczos-3, the kernel stretched by
 * the reduction (an antialiasing filter), weights renormalised at the
 * edges. Linear light keeps the mean: a fine black/white pattern shrinks
 * to its 0.5 grey, where a resize of gamma-encoded values darkens it.
 * Never upscales. Pure: the caller provides the scratch memory (the
 * horizontal pass: height x output width pixels). */
#ifndef FT_RESIZE_H
#define FT_RESIZE_H

#include <stddef.h>
#include <stdint.h>

#include "ft.h"

/* The output size for scale (0 < scale <= 1): round(width x scale) x
 * round(height x scale), each at least 1. */
void ft_get_resized_size(uint32_t width, uint32_t height, double scale,
                         uint32_t* output_width, uint32_t* output_height);

/* The scratch memory ft_resize needs, in bytes. */
size_t ft_get_resize_scratch_bytes(uint32_t width, uint32_t height,
                                   uint32_t output_width,
                                   uint32_t output_height);

/* Downscale width x height RGB floats into output_width x output_height.
 * The same size copies.
 * Returns 0, or -1 with error filled when a size is 0 or the output is
 * larger than the input. */
int32_t ft_resize(const float* input, uint32_t width, uint32_t height,
                  float* output, uint32_t output_width, uint32_t output_height,
                  void* scratch, ft_err* error);

#endif

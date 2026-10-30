#ifndef FT_DRAW_FRAMES
#define FT_DRAW_FRAMES

#include <stddef.h>
#include <stdint.h>

#include "ft_detect.h"
#include "ft_job.h"

typedef struct {
  uint8_t r, g, b;
} rgb_8bit;

void draw_pixel(uint8_t* image, int32_t width, int32_t height, int32_t x,
                int32_t y, rgb_8bit color);

void draw_number(uint8_t* image, int32_t width, int32_t height, int32_t x,
                 int32_t y, int32_t value);

int32_t get_index_from_mm(_Float64 target_mm, _Float64 origin_mm,
                          _Float64 pitch, int32_t limit);

int32_t draw_frames(const uint8_t* filename, const _Float32* input_stream,
                    uint32_t lines, uint32_t width,
                    const ft_axes* axis_configuration,
                    const ft_job* detections);

#endif

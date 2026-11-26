#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Optional 4x edge-aware enlargement of the composed 320x200 game image.
 * Scale2x is applied twice, yielding 1280x800 without changing game logic or
 * altering source art on disk. All buffers use tightly packed RGBA8888.
 *
 * Zero-initialize this struct before its first init call. It owns its output
 * and scratch buffers; initialize once and reuse it for every rendered frame.
 */
typedef struct NkHdUpscaler {
    uint8_t *scratch;
    uint8_t *pixels;
    int source_width;
    int source_height;
    int width;
    int height;
    int pitch;
} NkHdUpscaler;

int nk_hd_upscaler_init(NkHdUpscaler *upscaler, int width, int height);
const uint8_t *nk_hd_upscaler_apply(NkHdUpscaler *upscaler,
                                    const uint8_t *source,
                                    size_t source_pitch);
void nk_hd_upscaler_destroy(NkHdUpscaler *upscaler);

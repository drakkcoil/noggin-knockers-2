#include "hd_upscale.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int same_pixel(const uint8_t *left, const uint8_t *right)
{
    return memcmp(left, right, 4) == 0;
}

/* Standard Scale2x/EPX neighbourhood rule, including edge replication. */
static void scale2x_rgba(const uint8_t *source, int width, int height,
                         size_t source_pitch, uint8_t *destination,
                         size_t destination_pitch)
{
    int y;
    for (y = 0; y < height; ++y) {
        const uint8_t *row = source + (size_t)y * source_pitch;
        const uint8_t *north = source + (size_t)(y > 0 ? y - 1 : y) * source_pitch;
        const uint8_t *south = source + (size_t)(y + 1 < height ? y + 1 : y) * source_pitch;
        uint8_t *top = destination + (size_t)(y * 2) * destination_pitch;
        uint8_t *bottom = top + destination_pitch;
        int x;
        for (x = 0; x < width; ++x) {
            const uint8_t *center = row + (size_t)x * 4;
            const uint8_t *above = north + (size_t)x * 4;
            const uint8_t *below = south + (size_t)x * 4;
            const uint8_t *left = row + (size_t)(x > 0 ? x - 1 : x) * 4;
            const uint8_t *right = row + (size_t)(x + 1 < width ? x + 1 : x) * 4;
            const uint8_t *a = center;
            const uint8_t *b = center;
            const uint8_t *c = center;
            const uint8_t *d = center;
            uint8_t *top_pair = top + (size_t)x * 8;
            uint8_t *bottom_pair = bottom + (size_t)x * 8;
            if (!same_pixel(above, below) && !same_pixel(left, right)) {
                if (same_pixel(left, above)) a = left;
                if (same_pixel(above, right)) b = right;
                if (same_pixel(left, below)) c = left;
                if (same_pixel(below, right)) d = right;
            }
            memcpy(top_pair, a, 4);
            memcpy(top_pair + 4, b, 4);
            memcpy(bottom_pair, c, 4);
            memcpy(bottom_pair + 4, d, 4);
        }
    }
}

int nk_hd_upscaler_init(NkHdUpscaler *upscaler, int width, int height)
{
    size_t pixels;
    size_t scratch_bytes;
    size_t output_bytes;
    if (!upscaler || width <= 0 || height <= 0 ||
        width > INT_MAX / 16 || height > INT_MAX / 4 ||
        upscaler->scratch || upscaler->pixels)
        return 0;
    if ((size_t)width > (size_t)-1 / (size_t)height)
        return 0;
    pixels = (size_t)width * (size_t)height;
    if (pixels > (size_t)-1 / 64)
        return 0;
    scratch_bytes = pixels * 16;
    output_bytes = pixels * 64;
    upscaler->scratch = (uint8_t *)malloc(scratch_bytes);
    upscaler->pixels = (uint8_t *)malloc(output_bytes);
    if (!upscaler->scratch || !upscaler->pixels) {
        nk_hd_upscaler_destroy(upscaler);
        return 0;
    }
    upscaler->source_width = width;
    upscaler->source_height = height;
    upscaler->width = width * 4;
    upscaler->height = height * 4;
    upscaler->pitch = width * 16;
    return 1;
}

const uint8_t *nk_hd_upscaler_apply(NkHdUpscaler *upscaler,
                                    const uint8_t *source,
                                    size_t source_pitch)
{
    if (!upscaler || !upscaler->scratch || !upscaler->pixels || !source ||
        source_pitch < (size_t)upscaler->source_width * 4)
        return NULL;
    scale2x_rgba(source, upscaler->source_width, upscaler->source_height,
                 source_pitch, upscaler->scratch,
                 (size_t)upscaler->source_width * 8);
    scale2x_rgba(upscaler->scratch, upscaler->source_width * 2,
                 upscaler->source_height * 2,
                 (size_t)upscaler->source_width * 8, upscaler->pixels,
                 (size_t)upscaler->pitch);
    return upscaler->pixels;
}

void nk_hd_upscaler_destroy(NkHdUpscaler *upscaler)
{
    if (!upscaler) return;
    free(upscaler->scratch);
    free(upscaler->pixels);
    memset(upscaler, 0, sizeof(*upscaler));
}

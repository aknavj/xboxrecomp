#ifndef NV2A_TEXTURE_DEPTH_H
#define NV2A_TEXTURE_DEPTH_H

#include "d3d8_swizzle.h"

static inline uint32_t nv_texture_depth_bytes(uint32_t format)
{
    switch (format) {
    case 0x2C: case 0x30: return 2;
    case 0x2E: return 4;
    default: return 0;
    }
}

static inline float nv_texture_depth_max(uint32_t format)
{
    return format == 0x2E ? 16777215.0f : 65535.0f;
}

static inline int nv_texture_depth_texel(const uint8_t *source, uint64_t source_bytes,
                                         uint32_t width, uint32_t height, uint32_t pitch,
                                         uint32_t format, uint32_t x, uint32_t y, float *depth)
{
    uint32_t bytes = nv_texture_depth_bytes(format), value = 0;
    uint64_t offset;
    if (!source || !depth || !bytes || x >= width || y >= height) return 0;
    if (format == 0x2C) {
        if ((width & (width - 1)) || (height & (height - 1))) return 0;
        offset = (uint64_t)swizzle_offset(x, y, width, height) * bytes;
    } else {
        if ((uint64_t)pitch < (uint64_t)width * bytes) return 0;
        offset = (uint64_t)y * pitch + (uint64_t)x * bytes;
    }
    if (offset > source_bytes || bytes > source_bytes - offset) return 0;
    memcpy(&value, source + (size_t)offset, bytes);
    if (format == 0x2E) value >>= 8;
    *depth = (float)value / nv_texture_depth_max(format);
    return 1;
}

/* NV shadow comparisons place the sampled depth on the left of the operator. */
static inline int nv_texture_depth_compare(uint32_t function, float sampled, float reference)
{
    switch (function) {
    case 0: return 0;
    case 1: return sampled < reference;
    case 2: return sampled == reference;
    case 3: return sampled <= reference;
    case 4: return sampled > reference;
    case 5: return sampled != reference;
    case 6: return sampled >= reference;
    case 7: return 1;
    default: return 0;
    }
}

#endif

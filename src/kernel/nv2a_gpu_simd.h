#ifndef NV2A_GPU_SIMD_H
#define NV2A_GPU_SIMD_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(__SSE2__)
#include <emmintrin.h>
#define NV2A_GPU_SIMD_SSE2 1
#endif

namespace nv2a_gpu_simd {
enum class Mode { scalar, sse2, avx2 };
struct ChangedPixels {
    uint32_t first, end;
};

bool available(Mode mode);
Mode selected_mode();
const char *mode_name(Mode mode);
ChangedPixels changed_pixels(const uint8_t *source, const uint8_t *cached, uint32_t columns, Mode mode);
void rotate_depth(const uint8_t *source, uint8_t *destination, uint32_t columns, bool to_guest, Mode mode);
bool indices_valid(const uint32_t *indices, uint32_t count, uint32_t vertex_count, Mode mode);

inline bool finite4(const void *values, Mode mode)
{
#ifdef NV2A_GPU_SIMD_SSE2
    if (mode != Mode::scalar) {
        __m128i exponent = _mm_set1_epi32(0x7F800000);
        __m128i bits = _mm_loadu_si128(static_cast<const __m128i *>(values));
        return _mm_movemask_epi8(_mm_cmpeq_epi32(_mm_and_si128(bits, exponent), exponent)) == 0;
    }
#endif
    const auto *bytes = static_cast<const uint8_t *>(values);
    for (uint32_t component = 0; component < 4; component++) {
        uint32_t bits;
        std::memcpy(&bits, bytes + component * 4, 4);
        if ((bits & 0x7F800000u) == 0x7F800000u) return false;
    }
    return true;
}
}

#endif

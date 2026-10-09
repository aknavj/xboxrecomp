#include "nv2a_gpu_simd.h"
#include <immintrin.h>
#include <climits>

#if defined(_MSC_VER)
#define NV2A_SIMD_NOINLINE __declspec(noinline)
#else
#define NV2A_SIMD_NOINLINE __attribute__((noinline))
#endif

namespace nv2a_gpu_simd {
// Keep AVX2 instructions behind the runtime dispatch even with link-time optimization.
NV2A_SIMD_NOINLINE ChangedPixels changed_pixels_avx2(const uint8_t *source, const uint8_t *cached, uint32_t columns)
{
    uint32_t first = 0, end = columns;
    for (; end - first >= 8; first += 8) {
        __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(source + (size_t)first * 4));
        __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(cached + (size_t)first * 4));
        if (_mm256_movemask_epi8(_mm256_cmpeq_epi32(a, b)) != -1) break;
    }
    while (first < end && !std::memcmp(source + (size_t)first * 4, cached + (size_t)first * 4, 4)) first++;
    if (first == end) return {columns, columns};
    for (; end - first >= 8; end -= 8) {
        __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(source + (size_t)(end - 8) * 4));
        __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(cached + (size_t)(end - 8) * 4));
        if (_mm256_movemask_epi8(_mm256_cmpeq_epi32(a, b)) != -1) break;
    }
    while (end > first && !std::memcmp(source + (size_t)(end - 1) * 4, cached + (size_t)(end - 1) * 4, 4)) end--;
    return {first, end};
}

NV2A_SIMD_NOINLINE bool indices_valid_avx2(const uint32_t *indices, uint32_t count, uint32_t vertex_count)
{
    uint32_t index = 0;
    const __m256i sign = _mm256_set1_epi32(INT_MIN);
    const __m256i limit = _mm256_xor_si256(_mm256_set1_epi32(static_cast<int>(vertex_count - 1)), sign);
    for (; count - index >= 8; index += 8) {
        __m256i value = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(indices + index));
        if (_mm256_movemask_epi8(_mm256_cmpgt_epi32(_mm256_xor_si256(value, sign), limit))) return false;
    }
    for (; index < count; index++)
        if (indices[index] >= vertex_count) return false;
    return true;
}
}

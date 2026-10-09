#include "nv2a_gpu_simd.h"
#include <climits>
#include <cstdio>
#include <cstdlib>
#if defined(NV2A_GPU_SIMD_AVX2) && defined(_MSC_VER)
#include <intrin.h>
#endif

namespace nv2a_gpu_simd {
#ifdef NV2A_GPU_SIMD_AVX2
ChangedPixels changed_pixels_avx2(const uint8_t *, const uint8_t *, uint32_t);
bool indices_valid_avx2(const uint32_t *, uint32_t, uint32_t);
#endif

static bool avx2_available()
{
#if defined(NV2A_GPU_SIMD_AVX2) && defined(_MSC_VER)
    int registers[4];
    __cpuid(registers, 0);
    if (registers[0] < 7) return false;
    __cpuidex(registers, 1, 0);
    constexpr int xsave_osxsave_avx = (1 << 26) | (1 << 27) | (1 << 28);
    if ((registers[2] & xsave_osxsave_avx) != xsave_osxsave_avx || (_xgetbv(0) & 6) != 6)
        return false;
    __cpuidex(registers, 7, 0);
    return (registers[1] & (1 << 5)) != 0;
#elif defined(NV2A_GPU_SIMD_AVX2) && (defined(__GNUC__) || defined(__clang__))
    return __builtin_cpu_supports("avx2") != 0;
#else
    return false;
#endif
}

bool available(Mode mode)
{
    if (mode == Mode::scalar) return true;
#ifdef NV2A_GPU_SIMD_SSE2
    if (mode == Mode::sse2) return true;
#endif
    static const bool avx2 = avx2_available();
    return mode == Mode::avx2 && avx2;
}

const char *mode_name(Mode mode)
{
    switch (mode) {
    case Mode::scalar: return "scalar";
    case Mode::sse2: return "sse2";
    case Mode::avx2: return "avx2";
    }
    std::fprintf(stderr, "[GPU-D3D11] invalid CPU SIMD mode\n");
    std::_Exit(EXIT_FAILURE);
}

Mode selected_mode()
{
    static const Mode mode = []() {
        const char *value = std::getenv("RECOMP_NV2A_CPU_SIMD");
        Mode result;
        if (!value || !*value || std::strcmp(value, "auto") == 0)
            result = available(Mode::avx2) ? Mode::avx2 : available(Mode::sse2) ? Mode::sse2 : Mode::scalar;
        else if (std::strcmp(value, "0") == 0 || std::strcmp(value, "scalar") == 0) result = Mode::scalar;
        else if (std::strcmp(value, "sse2") == 0) result = Mode::sse2;
        else if (std::strcmp(value, "avx2") == 0) result = Mode::avx2;
        else {
            std::fprintf(stderr, "[GPU-D3D11] invalid RECOMP_NV2A_CPU_SIMD value: %s\n", value);
            std::_Exit(EXIT_FAILURE);
        }
        if (!available(result)) {
            std::fprintf(stderr, "[GPU-D3D11] CPU SIMD mode %s is not supported by this build/CPU/OS\n", mode_name(result));
            std::_Exit(EXIT_FAILURE);
        }
        std::fprintf(stderr, "[GPU-D3D11] CPU SIMD: %s%s\n", mode_name(result),
                     result == Mode::avx2 ? " (float4/D24: sse2)" : "");
        return result;
    }();
    return mode;
}

static bool equal_pixel(const uint8_t *source, const uint8_t *cached, uint32_t column)
{
    return std::memcmp(source + (size_t)column * 4, cached + (size_t)column * 4, 4) == 0;
}

ChangedPixels changed_pixels(const uint8_t *source, const uint8_t *cached, uint32_t columns, Mode mode)
{
    if (!columns || !std::memcmp(source, cached, (size_t)columns * 4)) return {columns, columns};
#ifdef NV2A_GPU_SIMD_AVX2
    if (mode == Mode::avx2) return changed_pixels_avx2(source, cached, columns);
#endif
    uint32_t first = 0, end = columns;
#ifdef NV2A_GPU_SIMD_SSE2
    if (mode != Mode::scalar) {
        for (; end - first >= 4; first += 4) {
            __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i *>(source + (size_t)first * 4));
            __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i *>(cached + (size_t)first * 4));
            if (_mm_movemask_epi8(_mm_cmpeq_epi32(a, b)) != 0xFFFF) break;
        }
    }
#endif
    while (first < end && equal_pixel(source, cached, first)) first++;
    if (first == end) return {columns, columns};
#ifdef NV2A_GPU_SIMD_SSE2
    if (mode != Mode::scalar) {
        for (; end - first >= 4; end -= 4) {
            __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i *>(source + (size_t)(end - 4) * 4));
            __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i *>(cached + (size_t)(end - 4) * 4));
            if (_mm_movemask_epi8(_mm_cmpeq_epi32(a, b)) != 0xFFFF) break;
        }
    }
#endif
    while (end > first && equal_pixel(source, cached, end - 1)) end--;
    return {first, end};
}

template<bool ToGuest>
static void rotate_depth_impl(const uint8_t *source, uint8_t *destination, uint32_t columns, Mode mode)
{
    uint32_t column = 0;
#ifdef NV2A_GPU_SIMD_SSE2
    if (mode != Mode::scalar)
        for (; columns - column >= 4; column += 4) {
            __m128i value = _mm_loadu_si128(reinterpret_cast<const __m128i *>(source + (size_t)column * 4));
            if constexpr (ToGuest) value = _mm_or_si128(_mm_slli_epi32(value, 8), _mm_srli_epi32(value, 24));
            else value = _mm_or_si128(_mm_srli_epi32(value, 8), _mm_slli_epi32(value, 24));
            _mm_storeu_si128(reinterpret_cast<__m128i *>(destination + (size_t)column * 4), value);
        }
#endif
    for (; column < columns; column++) {
        uint32_t value;
        std::memcpy(&value, source + (size_t)column * 4, 4);
        if constexpr (ToGuest) value = (value << 8) | (value >> 24);
        else value = (value >> 8) | (value << 24);
        std::memcpy(destination + (size_t)column * 4, &value, 4);
    }
}

void rotate_depth(const uint8_t *source, uint8_t *destination, uint32_t columns, bool to_guest, Mode mode)
{
    if (to_guest) rotate_depth_impl<true>(source, destination, columns, mode);
    else rotate_depth_impl<false>(source, destination, columns, mode);
}

bool indices_valid(const uint32_t *indices, uint32_t count, uint32_t vertex_count, Mode mode)
{
    if (!vertex_count) return count == 0;
#ifdef NV2A_GPU_SIMD_AVX2
    if (mode == Mode::avx2) return indices_valid_avx2(indices, count, vertex_count);
#endif
    uint32_t index = 0;
#ifdef NV2A_GPU_SIMD_SSE2
    if (mode != Mode::scalar) {
        const __m128i sign = _mm_set1_epi32(INT_MIN);
        const __m128i limit = _mm_xor_si128(_mm_set1_epi32(static_cast<int>(vertex_count - 1)), sign);
        for (; count - index >= 4; index += 4) {
            __m128i value = _mm_loadu_si128(reinterpret_cast<const __m128i *>(indices + index));
            if (_mm_movemask_epi8(_mm_cmpgt_epi32(_mm_xor_si128(value, sign), limit))) return false;
        }
    }
#endif
    for (; index < count; index++)
        if (indices[index] >= vertex_count) return false;
    return true;
}
}

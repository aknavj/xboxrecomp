#include "kernel/nv2a_gpu_simd.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

using nv2a_gpu_simd::Mode;
static unsigned failures;
static volatile uint64_t benchmark_sink;

static void check(const char *name, bool passed)
{
    if (!passed) {
        std::printf("FAIL: %s\n", name);
        failures++;
    }
}

static uint32_t random_word(uint32_t &state)
{
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return state;
}

static uint32_t read_word(const uint8_t *memory)
{
    uint32_t value;
    std::memcpy(&value, memory, 4);
    return value;
}

static nv2a_gpu_simd::ChangedPixels reference_range(const uint8_t *source, const uint8_t *cached, uint32_t columns)
{
    uint32_t first = columns, end = columns;
    for (uint32_t column = 0; column < columns; column++)
        if (read_word(source + column * 4) != read_word(cached + column * 4)) {
            if (first == columns) first = column;
            end = column + 1;
        }
    return {first, end};
}

static void check_range(const uint8_t *source, const uint8_t *cached, uint32_t columns, Mode mode)
{
    auto expected = reference_range(source, cached, columns);
    auto actual = nv2a_gpu_simd::changed_pixels(source, cached, columns, mode);
    check("exact changed-pixel bounds", expected.first == actual.first && expected.end == actual.end);
}

static void test_rows(Mode mode)
{
    uint32_t seed = 0x12345678;
    for (uint32_t alignment = 0; alignment < 32; alignment++)
        for (uint32_t columns = 0; columns <= 65; columns++) {
            std::vector<uint8_t> source(columns * 4 + 64), cached, actual, expected;
            for (auto &byte : source) byte = static_cast<uint8_t>(random_word(seed));
            cached = source;
            check_range(source.data() + alignment, cached.data() + alignment, columns, mode);
            for (uint32_t pixel = 0; pixel < columns; pixel++) {
                source[alignment + pixel * 4 + pixel % 4] ^= 0x80;
                check_range(source.data() + alignment, cached.data() + alignment, columns, mode);
                source[alignment + pixel * 4 + pixel % 4] ^= 0x80;
            }
            for (uint32_t pixel = 0; pixel < columns; pixel++)
                if (random_word(seed) & 1) source[alignment + pixel * 4] ^= 1;
            check_range(source.data() + alignment, cached.data() + alignment, columns, mode);
            for (bool to_guest : {false, true}) {
                for (uint32_t destination_alignment : {0u, 1u, 7u, 15u, 31u}) {
                    actual.assign(columns * 4 + 64, 0xA5); expected = actual;
                    for (uint32_t column = 0; column < columns; column++) {
                        uint32_t value = read_word(source.data() + alignment + column * 4);
                        value = to_guest ? (value << 8) | (value >> 24) : (value >> 8) | (value << 24);
                        std::memcpy(expected.data() + destination_alignment + column * 4, &value, 4);
                    }
                    nv2a_gpu_simd::rotate_depth(source.data() + alignment, actual.data() + destination_alignment,
                                               columns, to_guest, mode);
                    check("unaligned depth conversion and untouched surrounding bytes", actual == expected);
                }
                actual = source; expected = source;
                for (uint32_t column = 0; column < columns; column++) {
                    uint32_t value = read_word(source.data() + alignment + column * 4);
                    value = to_guest ? (value << 8) | (value >> 24) : (value >> 8) | (value << 24);
                    std::memcpy(expected.data() + alignment + column * 4, &value, 4);
                }
                nv2a_gpu_simd::rotate_depth(actual.data() + alignment, actual.data() + alignment, columns, to_guest, mode);
                check("in-place depth conversion", actual == expected);
            }
        }
}

struct GuardedPage {
    uint8_t *memory;
    GuardedPage() : memory(static_cast<uint8_t *>(VirtualAlloc(nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))) {
        DWORD previous;
        if (!memory || !VirtualProtect(memory + 4096, 4096, PAGE_NOACCESS, &previous)) {
            std::fprintf(stderr, "SIMD guard fixture failed: %lu\n", GetLastError());
            std::_Exit(EXIT_FAILURE);
        }
    }
    ~GuardedPage() {
        if (!VirtualFree(memory, 0, MEM_RELEASE)) {
            std::fprintf(stderr, "SIMD guard fixture cleanup failed: %lu\n", GetLastError());
            std::_Exit(EXIT_FAILURE);
        }
    }
    GuardedPage(const GuardedPage &) = delete;
    GuardedPage &operator=(const GuardedPage &) = delete;
    uint8_t *tail(uint32_t columns) { return memory + 4096 - columns * 4; }
};

static void test_guarded_tails(Mode mode)
{
    GuardedPage source, cached, destination;
    std::memset(source.memory, 0x39, 4096);
    std::memset(cached.memory, 0x39, 4096);
    for (uint32_t columns : {0u, 1u, 2u, 3u, 4u, 5u, 7u, 8u, 9u, 15u, 16u, 17u, 31u, 32u, 33u, 639u, 640u, 641u}) {
        auto *a = source.tail(columns), *b = cached.tail(columns), *out = destination.tail(columns);
        check_range(a, b, columns, mode);
        if (columns) {
            a[(columns / 2) * 4] ^= 1;
            check_range(a, b, columns, mode);
        }
        nv2a_gpu_simd::rotate_depth(a, out, columns, true, mode);
        nv2a_gpu_simd::rotate_depth(out, out, columns, false, mode);
        check("guarded depth round trip", !columns || !std::memcmp(a, out, columns * 4));
        auto *indices = reinterpret_cast<uint32_t *>(a);
        for (uint32_t index = 0; index < columns; index++) indices[index] = index % 7;
        check("guarded index validation", nv2a_gpu_simd::indices_valid(indices, columns, 7, mode));
        if (columns) {
            indices[columns - 1] = UINT32_MAX;
            check("guarded final invalid index", !nv2a_gpu_simd::indices_valid(indices, columns, 7, mode));
        }
        std::memset(source.memory, 0x39, 4096);
    }
    const uint32_t finite[] = {0, 0x80000000u, 1, 0x7F7FFFFFu};
    std::memcpy(source.tail(4), finite, 16);
    check("guarded finite float4", nv2a_gpu_simd::finite4(source.tail(4), mode));
}

static void test_validation(Mode mode)
{
    uint32_t words[4] = {};
    for (uint32_t lane = 0; lane < 4; lane++)
        for (uint32_t exponent = 0; exponent < 256; exponent++)
            for (uint32_t mantissa : {0u, 1u, 0x400000u, 0x7FFFFFu})
                for (uint32_t sign : {0u, 0x80000000u}) {
                    std::memset(words, 0, sizeof words);
                    words[lane] = sign | (exponent << 23) | mantissa;
                    check("float4 finite classification preserves NaN/Inf/subnormal bits",
                          nv2a_gpu_simd::finite4(words, mode) == (exponent != 255));
                }
    uint32_t seed = 0xABCDE123;
    for (uint32_t iteration = 0; iteration < 65536; iteration++) {
        bool expected = true;
        for (auto &word : words) {
            word = random_word(seed);
            expected = expected && ((word & 0x7F800000u) != 0x7F800000u);
        }
        check("random float4 exact finite classification", nv2a_gpu_simd::finite4(words, mode) == expected);
    }
    std::array<uint32_t, 66> indices = {};
    for (uint32_t columns = 0; columns < indices.size(); columns++)
        for (uint32_t limit : {0u, 1u, 7u, 16384u, 65536u, 0x80000000u, UINT32_MAX}) {
            for (uint32_t index = 0; index < columns; index++) indices[index] = limit ? index % limit : 0;
            check("index validation handles zero counts and limits",
                  nv2a_gpu_simd::indices_valid(indices.data(), columns, limit, mode) == (limit || !columns));
            for (uint32_t index = 0; index < columns; index++)
                for (uint32_t invalid : {limit, UINT32_MAX}) {
                    uint32_t saved = indices[index]; indices[index] = invalid;
                    check("index validation rejects every invalid lane including unsigned extremes",
                          !nv2a_gpu_simd::indices_valid(indices.data(), columns, limit, mode));
                    indices[index] = saved;
                }
        }
}

static void test_depth_precision(Mode mode)
{
    std::array<uint32_t, 4096> native, guest, roundtrip;
    uint64_t mismatches = 0;
    for (uint32_t base = 0; base < 0x1000000u; base += static_cast<uint32_t>(native.size())) {
        for (uint32_t index = 0; index < native.size(); index++) {
            uint32_t depth = base + index;
            native[index] = depth | ((depth & 255u) << 24);
        }
        nv2a_gpu_simd::rotate_depth(reinterpret_cast<const uint8_t *>(native.data()),
            reinterpret_cast<uint8_t *>(guest.data()), static_cast<uint32_t>(native.size()), true, mode);
        nv2a_gpu_simd::rotate_depth(reinterpret_cast<const uint8_t *>(guest.data()),
            reinterpret_cast<uint8_t *>(roundtrip.data()), static_cast<uint32_t>(guest.size()), false, mode);
        for (uint32_t index = 0; index < native.size(); index++) {
            uint32_t value = native[index];
            if (guest[index] != ((value << 8) | (value >> 24)) || roundtrip[index] != value) mismatches++;
        }
    }
    std::printf("SIMD precision %s: %llu mismatches; all 16777216 D24 codes and 256 stencil values\n",
                nv2a_gpu_simd::mode_name(mode), static_cast<unsigned long long>(mismatches));
    check("exhaustive integer depth/stencil precision", mismatches == 0);
}

template<typename Work>
static void benchmark(const char *name, Mode mode, const Work &work)
{
    work();
    auto start = std::chrono::steady_clock::now();
    uint64_t result = work();
    auto end = std::chrono::steady_clock::now();
    benchmark_sink = result;
    std::printf("SIMD benchmark %s %s: %.3f ms; checksum %llu\n", nv2a_gpu_simd::mode_name(mode), name,
                std::chrono::duration<double, std::milli>(end - start).count(), static_cast<unsigned long long>(result));
}

static void benchmarks(Mode mode)
{
    constexpr uint32_t width = 640, height = 480, pitch = width * 4 + 16;
    std::vector<uint8_t> source(pitch * height, 0x39), cached = source, destination(source.size());
    for (uint32_t row = 0; row < height; row++) {
        if (row % 4 == 1) source[row * pitch + width * 2] ^= 1;
        if (row % 4 == 2) { source[row * pitch] ^= 1; source[row * pitch + (width - 1) * 4] ^= 1; }
    }
    benchmark("changed-bounds", mode, [&]() {
        uint64_t sum = 0;
        for (uint32_t pass = 0; pass < 512; pass++)
            for (uint32_t row = 0; row < height; row++) {
                auto range = nv2a_gpu_simd::changed_pixels(source.data() + row * pitch, cached.data() + row * pitch, width, mode);
                sum += range.first + range.end;
            }
        return sum;
    });
    destination = cached;
    benchmark("unchanged-bounds", mode, [&]() {
        uint64_t sum = 0;
        for (uint32_t pass = 0; pass < 512; pass++)
            for (uint32_t row = 0; row < height; row++) {
                auto range = nv2a_gpu_simd::changed_pixels(cached.data() + row * pitch, destination.data() + row * pitch, width, mode);
                sum += range.first + range.end;
            }
        return sum;
    });
    benchmark("D24-rotate", mode, [&]() {
        for (uint32_t pass = 0; pass < 512; pass++)
            for (uint32_t row = 0; row < height; row++)
                nv2a_gpu_simd::rotate_depth(source.data() + row * pitch, destination.data() + row * pitch, width, true, mode);
        return read_word(destination.data());
    });
    std::vector<uint8_t> snapshot(source.size());
    for (bool depth : {false, true}) {
        benchmark(depth ? "D24-publication-split" : "color-publication-split", mode, [&]() {
            for (uint32_t pass = 0; pass < 512; pass++)
                for (uint32_t row = 0; row < height; row++) {
                    const auto *input = source.data() + row * pitch;
                    auto *output = destination.data() + row * pitch;
                    auto *cached_row = snapshot.data() + row * pitch;
                    if (depth) nv2a_gpu_simd::rotate_depth(input, cached_row, width, true, mode);
                    else std::memcpy(cached_row, input, width * 4);
                    std::memcpy(output, cached_row, width * 4);
                }
            return read_word(destination.data()) + static_cast<uint64_t>(read_word(snapshot.data()));
        });
        for (uint32_t row = 0; row < height; row++)
            for (uint32_t column = 0; column < width; column++) {
                uint32_t expected = read_word(source.data() + row * pitch + column * 4);
                if (depth) expected = (expected << 8) | (expected >> 24);
                check("publication benchmark exact destination", read_word(destination.data() + row * pitch + column * 4) == expected);
                check("publication benchmark exact snapshot", read_word(snapshot.data() + row * pitch + column * 4) == expected);
            }
    }
    std::vector<uint32_t> indices(65536);
    for (uint32_t index = 0; index < indices.size(); index++) indices[index] = index % 16384;
    benchmark("indices", mode, [&]() {
        uint64_t sum = 0;
        for (uint32_t pass = 0; pass < 4096; pass++)
            sum += nv2a_gpu_simd::indices_valid(indices.data(), static_cast<uint32_t>(indices.size()), 16384, mode);
        return sum;
    });
    std::vector<uint8_t> vertices(404 * 4096, 0);
    benchmark("finite4-strided", mode, [&]() {
        uint64_t sum = 0;
        for (uint32_t pass = 0; pass < 4096; pass++)
            for (uint32_t vertex = 0; vertex < 4096; vertex++)
                sum += nv2a_gpu_simd::finite4(vertices.data() + vertex * 404, mode);
        return sum;
    });
}

int main(int argc, char **argv)
{
    if (argc == 2 && !std::strcmp(argv[1], "--mode-only")) {
        nv2a_gpu_simd::selected_mode();
        return 0;
    }
    if (argc == 2 && !std::strcmp(argv[1], "--bench")) {
        benchmarks(nv2a_gpu_simd::selected_mode());
        return failures ? 1 : 0;
    }
    for (Mode mode : {Mode::scalar, Mode::sse2, Mode::avx2}) {
        if (!nv2a_gpu_simd::available(mode)) {
            std::printf("SIMD %s: unavailable, not executed\n", nv2a_gpu_simd::mode_name(mode));
            continue;
        }
        test_rows(mode); test_guarded_tails(mode); test_validation(mode); test_depth_precision(mode);
    }
    std::printf("nv2a_gpu_simd_smoke: %s\n", failures ? "FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}

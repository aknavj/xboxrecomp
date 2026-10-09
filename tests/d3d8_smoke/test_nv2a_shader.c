#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <process.h>
#include "kernel/nv2a_gpu.h"
#include "kernel/nv2a_gpu_memory.h"
#include "kernel/xbox_memory_layout.h"
#include "d3d/d3d8_swizzle.h"
#include "d3d/nv2a_texture_depth.h"

void nv2a_pb_exec_method(uint32_t subchannel, uint32_t method, uint32_t parameter);
void nv2a_pb_exec_report(void);
void nv2a_pb_scan(uint32_t start_address, uint32_t end_address);

static uint32_t memory[32768];
static uint32_t nv2a_registers[0x1000000 / sizeof(uint32_t)];
static int failures;
uint32_t g_xbox_image_lo, g_xbox_image_hi;
size_t g_xbox_total_ram = sizeof memory;
size_t g_xbox_map_size;

ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)memory; }
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
uint32_t xbox_ContiguousBlockSize(uint32_t guest_va)
{
    (void)guest_va;
    return 0; /* The harness has no contiguous allocations. */
}

uint8_t *xbox_DmaPhysicalPointer(uint64_t physical, uint32_t bytes)
{
    if (physical >= sizeof memory || bytes > sizeof memory - physical)
        return NULL;
    return (uint8_t *)memory + (size_t)physical;
}

volatile uint32_t *xbox_Nv2aRegisterPointer(uint32_t offset, uint32_t bytes)
{
    if ((offset & 3u) || offset >= sizeof nv2a_registers
            || bytes > sizeof nv2a_registers - offset)
        return NULL;
    return nv2a_registers + offset / sizeof(uint32_t);
}

int xbox_Nv2aSoftwareMethod(uint32_t parameter, uint32_t depth_clear,
                            uint32_t color_clear)
{
    fprintf(stderr, "Unexpected guest software method in shader harness: "
            "0x%08X (depth=0x%08X, color=0x%08X)\n",
            parameter, depth_clear, color_clear);
    exit(EXIT_FAILURE);
}

void xbox_Nv2aSoftwareMethodReport(void)
{
    /* No guest software callbacks are registered by this harness. */
}
void xbox_FramebufferWindowSet(uint32_t address, uint32_t pitch) { (void)address; (void)pitch; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowPresent(uint32_t address, uint32_t pitch) { (void)address; (void)pitch; }
void xbox_Nv2aFrameCounterFlip(void) {}

typedef struct TestVertex {
    float position[4], diffuse[4], texture0[4], texture1[4];
} TestVertex;

static void check(const char *name, int condition)
{
    if (!condition) { printf("FAIL: %s\n", name); failures++; }
}

static void method(uint32_t address, uint32_t parameter)
{
    nv2a_pb_exec_method(0, address, parameter);
}

static void float_method(uint32_t address, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    method(address, bits);
}

static void upload_program(void)
{
    const uint32_t program[4][4] = {
        {0, (3u << 21) | 0x1Bu, (2u << 26) | (0x1Bu << 2), (3u << 28) | 0xF800u},
        {0, (1u << 21) | (3u << 9) | 0x1Bu, 2u << 26, 0xF800u | (3u << 3)},
        {0, (1u << 21) | (9u << 9) | 0x1Bu, 2u << 26, 0xF800u | (9u << 3)},
        {0, (1u << 21) | (10u << 9) | 0x1Bu, 2u << 26, 0xF800u | (10u << 3) | 1u}
    };
    uint32_t instruction, component;
    method(0x1E9C, 0);
    for (instruction = 0; instruction < 4; instruction++)
        for (component = 0; component < 4; component++)
            method(0x0B00 + component * 4, program[instruction][component]);
    method(0x1EA0, 0);
    method(0x1E94, 6);
    method(0x1EA4, 0);
    float_method(0x0B80, 2);
    float_method(0x0B84, 2);
    float_method(0x0B88, 0);
    float_method(0x0B8C, 0);
}

static void bind_texture(uint32_t stage, uint32_t address, uint32_t color)
{
    uint32_t base = 0x1B00 + stage * 64;
    memory[address / 4] = color;
    method(base, address);
    method(base + 4, 0x12u << 8);
    method(base + 8, 0x303);
    method(base + 12, 1u << 30);
    method(base + 16, 4u << 16);
    method(base + 28, 0x00010001);
}

static void draw(uint32_t primitive, uint32_t count)
{
    method(0x17FC, primitive);
    method(0x1810, (count - 1) << 24);
    method(0x17FC, 0);
    {
        extern void nv2a_pb_exec_flush(void);
        nv2a_pb_exec_flush();
    }
}

static void gpu_texture_cache_setup(Nv2aGpuDraw *state, Nv2aGpuVertex *vertices, uint32_t *color);

static void test_ramht_entry(uint32_t entry, uint32_t handle, uint32_t instance)
{
    nv2a_registers[(0x700000 + entry * 8) / 4] = handle;
    nv2a_registers[(0x700004 + entry * 8) / 4] = 0x80000000u | (instance >> 4);
}

static int test_effect_commands(const char *invalid_case)
{
    static const uint32_t formats[] = {1, 4, 6, 7, 0xA, 0xB};
    uint8_t *source = (uint8_t *)memory + 0x6003, *destination = (uint8_t *)memory + 0x9001;
    uint8_t expected[4096], snapshot[4096];
    uint32_t *source_dma = nv2a_registers + 0x700400 / 4;
    uint32_t *destination_dma = nv2a_registers + 0x700420 / 4;
    _putenv_s("RECOMP_NV2A_NATIVE_FENCES", "1");
    test_ramht_entry(0, 0x62, 0x100); test_ramht_entry(1, 0x9F, 0x120);
    test_ramht_entry(2, 0x162, 0x140);
    test_ramht_entry(3, 0x19, 0x400); test_ramht_entry(4, 0x1A, 0x420);
    nv2a_registers[0x700100 / 4] = 0x62;
    nv2a_registers[0x700120 / 4] = 0x9F;
    nv2a_registers[0x700140 / 4] = 0x62;
    source_dma[0] = 0x3D | (3u << 20); source_dma[1] = 4095; source_dma[2] = 0x6000;
    destination_dma[0] = 0x3D | (1u << 20); destination_dma[1] = 4095; destination_dma[2] = 0x9000;
    nv2a_pb_exec_method(2, 0, 0x62);
    nv2a_pb_exec_method(2, 0x184, 0x19); nv2a_pb_exec_method(2, 0x188, 0x1A);
    nv2a_pb_exec_method(2, 0x304, 40 | (48u << 16));
    nv2a_pb_exec_method(2, 0x308, 11); nv2a_pb_exec_method(2, 0x30C, 7);
    nv2a_pb_exec_method(3, 0, 0x9F);
    nv2a_pb_exec_method(3, 0x19C, 0x62); nv2a_pb_exec_method(3, 0x2FC, 3);
    nv2a_pb_exec_method(3, 0x300, 2 | (1u << 16));
    nv2a_pb_exec_method(3, 0x304, 3 | (2u << 16));
    for (uint32_t format = 0; format < sizeof formats / sizeof formats[0]; format++) {
        uint32_t bytes = format == 0 ? 1 : format == 1 ? 2 : 4;
        for (uint32_t i = 0; i < 4096; i++) source[i] = (uint8_t)(i * 37 + 13);
        memset(destination, 0xCC, 4096); memset(expected, 0xCC, sizeof expected);
        nv2a_pb_exec_method(2, 0x300, formats[format]);
        for (uint32_t row = 0; row < 4; row++)
            memcpy(expected + 7 + (row + 2) * 48 + 3 * bytes,
                   source + 11 + (row + 1) * 40 + 2 * bytes, 5 * bytes);
        nv2a_pb_exec_method(3, 0x308, 5 | (4u << 16));
        check("2D SRCCOPY preserves exact format bytes, pitch, offsets and surrounding memory",
              memcmp(destination, expected, sizeof expected) == 0);
    }
    nv2a_pb_exec_method(2, 0, 0x162);
    nv2a_pb_exec_method(2, 0x300, 1);
    nv2a_pb_exec_method(2, 0, 0x62);
    memset(destination, 0xCC, 4096);
    nv2a_pb_exec_method(3, 0x308, 5 | (4u << 16));
    check("2D surface object state survives rebinding", memcmp(destination, expected, sizeof expected) == 0);
    if (invalid_case) {
        if (!strcmp(invalid_case, "--effects-invalid-limit")) destination_dma[1] = 127;
        else if (!strcmp(invalid_case, "--effects-invalid-pitch")) nv2a_pb_exec_method(2, 0x304, 40 | (12u << 16));
        else if (!strcmp(invalid_case, "--effects-invalid-operation")) nv2a_pb_exec_method(3, 0x2FC, 2);
        else if (!strcmp(invalid_case, "--effects-invalid-address")) source_dma[2] = 0xFFFFF000;
        else return 2;
        nv2a_pb_exec_method(3, 0x308, 5 | (4u << 16));
        return 0;
    }
    nv2a_pb_exec_method(2, 0x188, 0x19);
    nv2a_pb_exec_method(2, 0x304, 40 | (40u << 16));
    nv2a_pb_exec_method(2, 0x308, 0); nv2a_pb_exec_method(2, 0x30C, 0);
    nv2a_pb_exec_method(3, 0x300, 0); nv2a_pb_exec_method(3, 0x304, 1 | (1u << 16));
    memcpy(expected, source, sizeof expected); memcpy(snapshot, source, sizeof snapshot);
    for (uint32_t row = 0; row < 4; row++)
        memcpy(expected + (row + 1) * 40 + 4, snapshot + row * 40, 20);
    nv2a_pb_exec_method(3, 0x308, 5 | (4u << 16));
    check("overlapping 2D rectangle copies preserve original source rows", memcmp(source, expected, sizeof expected) == 0);

    check("effect command D3D11 device", nv2a_gpu_available());
    Nv2aGpuDraw state = {0};
    state.color = (uint8_t *)memory + 0x6000;
    state.width = state.height = state.clip_width = state.clip_height = 8;
    state.pitch = 40; state.bytes_per_pixel = 4;
    source_dma[0] = 0x3D; destination_dma[0] = 0x3D;
    check("queue auxiliary source clear", nv2a_gpu_clear(&state, 0xF0, 0xFF204080, 0) == 1);
    state.color = (uint8_t *)memory + 0x9000; state.pitch = 48;
    check("queue auxiliary destination clear", nv2a_gpu_clear(&state, 0xF0, 0xFF112233, 0) == 1);
    nv2a_pb_exec_method(2, 0x188, 0x1A);
    nv2a_pb_exec_method(2, 0x304, 40 | (48u << 16));
    nv2a_pb_exec_method(3, 0x308, 5 | (4u << 16));
    for (uint32_t row = 0; row < 8; row++)
        for (uint32_t column = 0; column < 8; column++)
            check("blit publishes pending GPU source and preserves destination exterior",
                  memory[0x9000 / 4 + row * 12 + column] ==
                  (row >= 1 && row < 5 && column >= 1 && column < 6 ? 0xFF204080u : 0xFF112233u));

    uint32_t sampled[64] = {0};
    Nv2aGpuDraw sampling = {0};
    Nv2aGpuVertex vertices[3] = {0};
    gpu_texture_cache_setup(&sampling, vertices, sampled);
    sampling.textures[0].source = (uint8_t *)memory + 0x9000;
    sampling.textures[0].source_bytes = 48 * 8;
    sampling.textures[0].width = sampling.textures[0].height = 8;
    sampling.textures[0].pitch = 48; sampling.textures[0].linear = 1; sampling.textures[0].format = 0x12;
    sampling.textures[0].decode_context = memory + 0x9000 / 4;
    for (uint32_t vertex = 0; vertex < 3; vertex++)
        vertices[vertex].texture[0][0] = vertices[vertex].texture[0][1] = 3.5f;
    check("sample auxiliary target after CPU copy", nv2a_gpu_draw(&sampling, vertices, 3) == 1);
    nv2a_gpu_flush();
    check("cached render-target sampling sees copied bytes rather than stale GPU image", sampled[3 * 8 + 3] == 0xFF204080);

    method(0x0200, 8u << 16); method(0x0204, 8u << 16);
    method(0x0208, 0x28); method(0x020C, 32 | (32u << 16));
    method(0x0210, 0x1000); method(0x0214, 0x2000);
    method(0x1D90, 0xFFAABBCC); method(0x1D8C, 0x12345678);
    method(0x1D98, 2 | (4u << 16)); method(0x1D9C, 1 | (3u << 16));
    for (uint32_t i = 0; i < 64; i++) {
        memory[0x1000 / 4 + i] = 0x11223344;
        memory[0x2000 / 4 + i] = 0xABCDEF9A;
    }
    method(0x1D94, 0x11); /* Red and depth only. */
    for (uint32_t row = 0; row < 8; row++)
        for (uint32_t column = 0; column < 8; column++) {
            int inside = row >= 1 && row <= 3 && column >= 2 && column <= 4;
            check("clear rectangle has inclusive endpoints and preserves color channels",
                  memory[0x1000 / 4 + row * 8 + column] == (inside ? 0x11AA3344u : 0x11223344u));
            check("clear rectangle preserves stencil and exterior depth",
                  memory[0x2000 / 4 + row * 8 + column] == (inside ? 0x1234569Au : 0xABCDEF9Au));
        }
    method(0x1D98, 6 | (4u << 16)); method(0x1D94, 0xF3);
    check("inverted clear rectangle is an empty operation", memory[0x1000 / 4 + 3 * 8 + 3] == 0x11AA3344);
    method(0x1D98, 7 | (9u << 16)); method(0x1D9C, 7 | (9u << 16)); method(0x1D94, 2);
    check("stencil-only clear clips at known target extent",
          memory[0x2000 / 4 + 63] == 0xABCDEF78 && memory[0x2000 / 4 + 62] == 0xABCDEF9A);
    method(0x0208, 0x23); method(0x020C, 16 | (32u << 16));
    uint16_t *color16 = (uint16_t *)((uint8_t *)memory + 0x1000);
    for (uint32_t pixel = 0; pixel < 64; pixel++) color16[pixel] = 0x1234;
    method(0x1D98, 2 | (2u << 16)); method(0x1D9C, 3 | (3u << 16)); method(0x1D94, 0x20);
    check("RGB565 clear preserves red/blue channels and rectangle exterior",
          color16[3 * 8 + 2] == (uint16_t)((0x1234 & ~0x07E0) | ((0xFFAABBCC >> 5) & 0x07E0)) &&
          color16[3 * 8 + 3] == 0x1234);
    method(0x0208, 0x28); method(0x020C, 32 | (32u << 16));
    method(0x1D98, 0xFFFF0000); method(0x1D9C, 0xFFFF0000);
    method(0x1D90, 0xDEADBEEF); method(0x1D8C, 0xFFFFFF6A);
    method(0x1D94, 0xF3);
    nv2a_gpu_flush();
    for (uint32_t pixel = 0; pixel < 64; pixel++)
        check("full clear rectangle clips to extent and preserves exact color/depth/stencil bits",
              memory[0x1000 / 4 + pixel] == 0xDEADBEEF && memory[0x2000 / 4 + pixel] == 0xFFFFFF6A);
    nv2a_gpu_invalidate();
    nv2a_pb_exec_report();
    printf("nv2a_effect_commands: %s\n", failures ? "FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}

static void test_effect_command_processes(void)
{
    static const char *cases[] = {"--effects", "--effects-invalid-limit", "--effects-invalid-pitch",
                                 "--effects-invalid-operation", "--effects-invalid-address"};
    char *executable = NULL;
    if (_get_pgmptr(&executable) || !executable) {
        check("effect command executable path", 0); return;
    }
    for (uint32_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        check(cases[i], _spawnl(_P_WAIT, executable, executable, cases[i], NULL) == (i ? EXIT_FAILURE : 0));
}

static int invalid_pushbuffer_case(const char *name)
{
    const uint32_t address = 0x18000;
    uint32_t *packet = memory + address / 4;
    if (!strcmp(name, "--pb-discontinuous")) {
        packet[0] = (2u << 18) | 0x1D90;
        nv2a_pb_scan(address, address + 4);
        nv2a_pb_scan(address + 8, address + 12);
    } else if (!strcmp(name, "--pb-invalid-jump")) {
        packet[0] = 0x1FFFFFFD;
        nv2a_pb_scan(address, address + 4);
    } else if (!strcmp(name, "--pb-cycle")) {
        packet[0] = address | 1u;
        nv2a_pb_scan(address, address + 4);
    } else if (!strcmp(name, "--pb-nested-call")) {
        packet[0] = (address + 8) | 2u;
        packet[2] = (address + 16) | 2u;
        nv2a_pb_scan(address, address + 4);
    } else if (!strcmp(name, "--pb-invalid-return")) {
        packet[0] = 0x00020000;
        nv2a_pb_scan(address, address + 4);
    } else return 2;
    return 0;
}

static void test_invalid_pushbuffer_packets(void)
{
    static const char *cases[] = {"--pb-discontinuous", "--pb-invalid-jump", "--pb-cycle", "--pb-nested-call", "--pb-invalid-return"};
    char *executable = NULL;
    size_t index;
    if (_get_pgmptr(&executable) || !executable) {
        check("pushbuffer negative test executable path", 0);
        return;
    }
    for (index = 0; index < sizeof cases / sizeof cases[0]; index++)
        check(cases[index], _spawnl(_P_WAIT, executable, executable, cases[index], NULL) == EXIT_FAILURE);
}

static void test_split_pushbuffer_packet(void)
{
    const uint32_t address = 0x18000;
    uint32_t *packet = memory + address / 4;
    uint32_t split, index;
    method(0x0200, 8u << 16); method(0x0204, 8u << 16);
    method(0x0208, 0x28); method(0x020C, 32u | (32u << 16));
    method(0x0210, 0x1000); method(0x0214, 0x2000);
    packet[0] = (3u << 18) | 0x1D8C;
    packet[1] = 0xFFFFFFA6; packet[2] = 0xFFA1B2C3; packet[3] = 0xF0;
    for (split = 1; split < 4; split++) {
        for (index = 0; index < 64; index++) memory[0x1000 / 4 + index] = 0x11223344;
        nv2a_pb_scan(address, address + split * 4);
        check("partial method packet does not clear prematurely", memory[0x1000 / 4] == 0x11223344);
        nv2a_pb_scan(address + split * 4, address + 16);
        check("split incrementing packet preserves parameters", memory[0x1000 / 4] == 0xFFA1B2C3 && memory[0x1000 / 4 + 63] == 0xFFA1B2C3);
    }
    packet[0] = 0x40000000u | (2u << 18) | 0x1D90;
    packet[1] = 0xFF123456; packet[2] = 0xFFABCDEF;
    packet[3] = (1u << 18) | 0x1D94; packet[4] = 0xF0;
    nv2a_pb_scan(address, address + 4);
    nv2a_pb_scan(address + 4, address + 8);
    nv2a_pb_scan(address + 8, address + 20);
    check("split nonincrementing packet retains method", memory[0x1000 / 4] == 0xFFABCDEF && memory[0x1000 / 4 + 63] == 0xFFABCDEF);
    for (index = 0; index < 2; index++) {
        uint32_t *tail = packet + 16;
        tail[0] = (3u << 18) | 0x1D8C;
        tail[1] = 0xFFFFFFA6; tail[2] = 0xFF102030; tail[3] = 0xF0;
        tail[4] = index ? 0x20000000u | address : address | 1u;
        packet[0] = (1u << 18) | 0x1D90; packet[1] = 0xFF506070;
        packet[2] = (1u << 18) | 0x1D94; packet[3] = 0xF0;
        nv2a_pb_scan(address + 64, address + 72);
        nv2a_pb_scan(address + 72, address + 16);
        check("ring jump completes tail packet and wrapped head", memory[0x1000 / 4] == 0xFF506070 && memory[0x1000 / 4 + 63] == 0xFF506070);
    }
    packet[0] = (address + 64) | 2u;
    packet[1] = (1u << 18) | 0x1D90; packet[2] = 0xFFABCDEF;
    packet[3] = (1u << 18) | 0x1D94; packet[4] = 0xF0;
    packet[16] = (1u << 18) | 0x1D90; packet[17] = 0xFF102030;
    packet[18] = (1u << 18) | 0x1D94; packet[19] = 0xF0; packet[20] = 0x00020000;
    nv2a_pb_scan(address, address + 4);
    check("DMA call executes subroutine before reaching caller PUT", memory[0x1000 / 4] == 0xFF102030);
    nv2a_pb_scan(address + 4, address + 20);
    check("DMA return resumes caller packet", memory[0x1000 / 4] == 0xFFABCDEF);
}

static uint32_t pixel(uint32_t horizontal, uint32_t vertical)
{
    return memory[0x1000 / 4 + vertical * 8 + horizontal];
}

static uint32_t gpu_decode_calls;

static int gpu_decode(void *context, uint32_t horizontal, uint32_t vertical, uint32_t *color)
{
    (void)horizontal; (void)vertical;
    gpu_decode_calls++;
    memcpy(color, context, sizeof *color);
    return 1;
}

static void test_gpu_depth_minimal(void)
{
    uint32_t color[64], depth[64], iteration, vertex;
    uint32_t saved_color[64], saved_depth[64];
    uint32_t texel = 0xFFFF0000;
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    state.color = (uint8_t *)color; state.depth = (uint8_t *)depth;
    state.width = state.height = state.clip_width = state.clip_height = 8;
    state.pitch = state.depth_pitch = 32; state.bytes_per_pixel = 4;
    state.depth_format = 2; state.depth_enable = state.depth_mask = 1; state.depth_function = 0x201;
    state.color_mask = 0x01010101;
    state.textures[0].source = (const uint8_t *)&texel; state.textures[0].source_bytes = 4;
    state.textures[0].width = state.textures[0].height = 1; state.textures[0].pitch = 4; state.textures[0].linear = 1;
    state.textures[0].decode_context = &texel; state.textures[0].decode = gpu_decode;
    vertices[1].position[0] = 8; vertices[2].position[1] = 8;
    for (vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].position[2] = 100; vertices[vertex].position[3] = 1;
        vertices[vertex].diffuse[0] = 1; vertices[vertex].diffuse[3] = 1;
        vertices[vertex].texture[0][3] = 1;
    }
    for (iteration = 0; iteration < 20; iteration++) {
        uint32_t pixel;
        uint32_t scenario = iteration % 4, path = iteration / 4;
        state.width = state.clip_width = 2u + iteration % 7;
        state.combiners.control = path ? 1 : 0; state.stage_program = path >= 2 ? 1 : 0;
        state.combiners.rgb_input[0] = path == 4 ? 0x08040000 : path >= 2 ? 0x08200000 : 0x04200000;
        state.combiners.final_input[0] = path >= 3 ? 0x0000000C : 0;
        state.combiners.final_input[1] = path >= 3 ? 0x00001C80 : 0;
        state.combiners.alpha_input[0] = 0x00002014;
        state.combiners.rgb_output[0] = state.combiners.alpha_output[0] = 0xC00;
        for (pixel = 0; pixel < 64; pixel++) { color[pixel] = 0xFF000000; depth[pixel] = 0xFFFFFF5A; }
        state.depth_format = scenario < 2 ? 2 : 1;
        state.control0 = scenario & 1 ? 0x10000 : 0;
        for (vertex = 0; vertex < 3; vertex++) {
            vertices[vertex].position[2] = 100;
            vertices[vertex].position[3] = scenario & 1 ? 100.0f : 1.0f;
        }
        if (path == 4) {
            state.depth_enable = 0;
            for (vertex = 0; vertex < 3; vertex++) vertices[vertex].position[2] = 0;
            check("minimal warm shader without depth", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
            state.depth_enable = 1;
            for (vertex = 0; vertex < 3; vertex++) vertices[vertex].position[2] = 100;
        }
        check("minimal hardware depth draw accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("minimal hardware depth draw color", color[9] == 0xFFFF0000);
        if (state.depth_format == 2) {
            printf("depth path %u Z24/W%u: %08X\n", path, scenario & 1, depth[9]);
            check("minimal hardware Z24/W-depth", depth[9] == 0x0000645A);
        } else {
            uint16_t stored; memcpy(&stored, (uint8_t *)depth + 32 + 2, 2);
            printf("depth path %u Z16/W%u: %u\n", path, scenario & 1, stored);
            check("minimal hardware Z16/W-depth", stored == 100);
        }
                memcpy(saved_color, color, sizeof color); memcpy(saved_depth, depth, sizeof depth);
                nv2a_gpu_flush(); nv2a_gpu_flush();
                check("unchanged repeated flush draw accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
                check("unchanged refresh preserves complete color and pitched depth",
                            memcmp(saved_color, color, sizeof color) == 0 && memcmp(saved_depth, depth, sizeof depth) == 0);
        nv2a_gpu_flush();
        color[9] = 0xFF010203; color[63] = 0xFF123456;
        if (state.depth_format == 2) depth[9] = 0x000032A6;
        else { uint16_t near_depth = 50; memcpy((uint8_t *)depth + 34, &near_depth, 2); }
        for (vertex = 0; vertex < 3; vertex++) {
            vertices[vertex].position[2] = 75;
            vertices[vertex].position[3] = scenario & 1 ? 75.0f : 1.0f;
        }
        check("retained surface observes CPU depth mutation", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("retained CPU color survives updated depth occlusion", color[9] == 0xFF010203 && color[63] == 0xFF123456);
        nv2a_gpu_flush();
        if (state.depth_format == 2) depth[9] = 0x000096A6;
        else { uint16_t far_depth = 150; memcpy((uint8_t *)depth + 34, &far_depth, 2); }
        check("retained surface observes subsequent CPU depth clear", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("retained refreshed depth admits nearer fragment", color[9] == 0xFFFF0000 && color[63] == 0xFF123456);
        if (state.depth_format == 2) check("retained Z24 refresh preserves CPU stencil", depth[9] == 0x00004BA6);
        else { uint16_t stored; memcpy(&stored, (uint8_t *)depth + 34, 2); check("retained Z16 refresh writes guest depth", stored == 75); }
        nv2a_gpu_flush(); state.depth_mask = 0;
        color[9] = 0xFF010203;
        if (state.depth_format == 2) depth[9] = 0x000032A6;
        else { uint16_t near_depth = 50; memcpy((uint8_t *)depth + 34, &near_depth, 2); }
        check("read-only depth observes CPU mutation", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("read-only mutated depth occludes fragment", color[9] == 0xFF010203);
        nv2a_gpu_flush();
        if (state.depth_format == 2) depth[9] = 0x00004BA6;
        else { uint16_t restored_depth = 75; memcpy((uint8_t *)depth + 34, &restored_depth, 2); }
        for (vertex = 0; vertex < 3; vertex++) {
            vertices[vertex].position[2] = 60;
            vertices[vertex].position[3] = scenario & 1 ? 60.0f : 1.0f;
        }
        check("read-only depth restored to old snapshot draw accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("old snapshot cannot hide a subsequent CPU depth change", color[9] == 0xFFFF0000 && color[63] == 0xFF123456);
        if (state.depth_format == 2) check("read-only depth retains restored stencil and depth", depth[9] == 0x00004BA6);
        else { uint16_t stored; memcpy(&stored, (uint8_t *)depth + 34, 2); check("read-only Z16 depth remains restored", stored == 75); }
        state.depth_mask = 1;
        nv2a_gpu_flush();
        state.color_mask = iteration & 1 ? 0xFEFEFEFE : 0;
        check("color-masked depth draw accepted", nv2a_gpu_draw(&state, vertices, 3));
        color[9] = 0xFF314159;
        memcpy(saved_color, color, sizeof color);
        nv2a_gpu_flush();
        check("color-masked draw does not publish stale color", memcmp(saved_color, color, sizeof color) == 0);
        if (state.depth_format == 2) check("color-masked draw still publishes Z24 depth", depth[9] == 0x00003CA6);
        else { uint16_t stored; memcpy(&stored, (uint8_t *)depth + 34, 2); check("color-masked draw still publishes Z16 depth", stored == 60); }
          state.depth_mask = 0;
          check("draw with both output write masks disabled accepted", nv2a_gpu_draw(&state, vertices, 3));
          color[9] = 0xFF271828;
          memcpy(saved_color, color, sizeof color); memcpy(saved_depth, depth, sizeof depth);
          nv2a_gpu_flush(); nv2a_gpu_flush();
          check("output-masked draw preserves complete guest color and depth",
              memcmp(saved_color, color, sizeof color) == 0 && memcmp(saved_depth, depth, sizeof depth) == 0);
          state.depth_mask = 1;
        state.color_mask = 0x01010101;
        for (vertex = 0; vertex < 3; vertex++) {
            vertices[vertex].position[2] = 45;
            vertices[vertex].position[3] = scenario & 1 ? 45.0f : 1.0f;
        }
        check("pending color write draw accepted", nv2a_gpu_draw(&state, vertices, 3));
        state.color_mask = 0;
        check("masked draw after pending color write accepted", nv2a_gpu_draw(&state, vertices, 3));
        nv2a_gpu_flush();
        check("masked draw preserves pending color publication", color[9] == 0xFFFF0000);
        state.color_mask = 0x01010101;
        if (iteration & 1) nv2a_gpu_invalidate();
        state.clip_x = state.clip_y = state.clip_width = state.clip_height = 1;
        for (vertex = 0; vertex < 3; vertex++) {
            vertices[vertex].position[2] = 30;
            vertices[vertex].position[3] = scenario & 1 ? 30.0f : 1.0f;
        }
        check("one-pixel scissor depth draw accepted", nv2a_gpu_draw(&state, vertices, 3));
        color[0] = 0xFF314159;
        color[9] = 0xFF010203;
        memcpy(saved_color, color, sizeof color); saved_color[9] = 0xFFFF0000;
        if (state.depth_format == 2) depth[0] = 0x000005B7;
        else { uint16_t outside_depth = 5; memcpy(depth, &outside_depth, 2); }
        memcpy(saved_depth, depth, sizeof depth);
        if (state.depth_format == 2) saved_depth[9] = 0x00001EA6;
        else { uint16_t inside_depth = 30; memcpy((uint8_t *)saved_depth + 34, &inside_depth, 2); }
        nv2a_gpu_flush();
        check("scissored color publication preserves all outside pixels", memcmp(saved_color, color, sizeof color) == 0);
        check("scissored depth publication preserves all outside pixels", memcmp(saved_depth, depth, sizeof depth) == 0);
        state.clip_x = state.clip_y = 0; state.clip_height = 8;
        state.clip_width = state.width;
        for (vertex = 0; vertex < 3; vertex++) {
            vertices[vertex].position[2] = 15;
            vertices[vertex].position[3] = scenario & 1 ? 15.0f : 1.0f;
        }
        check("CPU mutation outside scissor refreshes retained resources", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
        check("outside CPU depth mutation occludes later fragment", color[0] == 0xFF314159 && color[9] == 0xFFFF0000);
        state.clip_width = state.clip_height = 1;
        for (vertex = 0; vertex < 3; vertex++) {
            vertices[vertex].position[2] = 2;
            vertices[vertex].position[3] = scenario & 1 ? 2.0f : 1.0f;
        }
        check("first accumulated scissor draw accepted", nv2a_gpu_draw(&state, vertices, 3));
        state.clip_x = state.clip_y = 1;
        check("second accumulated scissor draw accepted", nv2a_gpu_draw(&state, vertices, 3));
        color[63] = 0xFF271828;
        color[1] = 0xFF173205;
        if (state.depth_format == 2) depth[1] = 0x000011CD;
        else { uint16_t gap_depth = 17; memcpy((uint8_t *)depth + 2, &gap_depth, 2); }
        nv2a_gpu_flush();
        check("accumulated scissors publish both color writes", color[0] == 0xFFFF0000 && color[9] == 0xFFFF0000 && color[63] == 0xFF271828);
        check("disjoint scissors preserve CPU color in their gap", color[1] == 0xFF173205);
        if (state.depth_format == 2) check("disjoint scissors preserve CPU Z24 and stencil in their gap", depth[1] == 0x000011CD);
        else { uint16_t stored; memcpy(&stored, (uint8_t *)depth + 2, 2); check("disjoint scissors preserve CPU Z16 in their gap", stored == 17); }
        if (state.depth_format == 2) check("accumulated scissors publish both depth writes", depth[0] == 0x000002B7 && depth[9] == 0x000002A6);
        else {
            uint16_t first_depth, second_depth;
            memcpy(&first_depth, depth, 2); memcpy(&second_depth, (uint8_t *)depth + 34, 2);
            check("accumulated scissors publish both Z16 writes", first_depth == 2 && second_depth == 2);
        }
        state.clip_x = state.width + 1; state.clip_y = 0;
        check("scissor outside viewport draw accepted", nv2a_gpu_draw(&state, vertices, 3));
        color[9] = 0xFF161803;
        memcpy(saved_color, color, sizeof color); memcpy(saved_depth, depth, sizeof depth);
        nv2a_gpu_flush(); nv2a_gpu_flush();
        check("empty effective scissor preserves complete guest output",
              memcmp(saved_color, color, sizeof color) == 0 && memcmp(saved_depth, depth, sizeof depth) == 0);
        state.clip_x = state.clip_y = 0; state.clip_height = 8;
        nv2a_gpu_invalidate();
    }
}

static void test_gpu_scissor_coverage(void)
{
    uint32_t color[544], depth[544], expected_color[544], expected_depth[544];
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    uint32_t vertex, pixel, pass;
    state.color = (uint8_t *)color; state.depth = (uint8_t *)depth;
    state.width = 67; state.height = 4; state.pitch = state.depth_pitch = 272;
    state.bytes_per_pixel = 4; state.depth_format = 2;
    state.color_mask = 0x01010101; state.depth_enable = state.depth_mask = 1; state.depth_function = 0x207;
    for (pixel = 0; pixel < 544; pixel++) { color[pixel] = 0xFF000000; depth[pixel] = 0xFFFFFFA5; }
    vertices[1].position[0] = vertices[2].position[1] = 128;
    for (vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].position[2] = 3; vertices[vertex].position[3] = 1;
        vertices[vertex].diffuse[0] = vertices[vertex].diffuse[3] = 1;
    }
    for (pass = 0; pass < 3; pass++) {
        state.clip_x = 61; state.clip_y = 0; state.clip_width = 5; state.clip_height = 1;
        check("word-boundary scissor accepted", nv2a_gpu_draw(&state, vertices, 3));
        state.clip_x = 3; state.clip_y = 1; state.clip_width = 1;
        check("non-word-aligned second row scissor accepted", nv2a_gpu_draw(&state, vertices, 3));
        if (pass == 2) {
            state.clip_x = state.clip_y = 0; state.clip_width = 67; state.clip_height = 2;
            check("containing scissor replaces fragmented coverage", nv2a_gpu_draw(&state, vertices, 3));
        }
        color[60] = 0xFF314159; depth[60] = 0x000011CD;
        color[67] = 0xFF271828; depth[67] = 0x000012B7;
        memcpy(expected_color, color, sizeof color); memcpy(expected_depth, depth, sizeof depth);
        if (pass == 2) {
            uint32_t row;
            for (row = 0; row < 2; row++)
                for (pixel = 0; pixel < 67; pixel++) {
                    expected_color[row * 68 + pixel] = 0xFFFF0000;
                    expected_depth[row * 68 + pixel] = 0x00000300 | (expected_depth[row * 68 + pixel] & 255);
                }
        } else {
            for (pixel = 61; pixel < 66; pixel++) {
                expected_color[pixel] = 0xFFFF0000; expected_depth[pixel] = 0x000003A5;
            }
            expected_color[71] = 0xFFFF0000; expected_depth[71] = 0x000003A5;
        }
        nv2a_gpu_flush();
        check("coverage spans preserve exact color and row padding", memcmp(expected_color, color, sizeof color) == 0);
        check("coverage spans preserve exact depth stencil and row padding", memcmp(expected_depth, depth, sizeof depth) == 0);
    }
    nv2a_gpu_invalidate();
    state.width = 64; state.pitch = 256;
    state.depth = NULL; state.depth_enable = state.depth_mask = 0;
    state.clip_x = 0; state.clip_y = 1; state.clip_width = 64; state.clip_height = 2;
    for (pass = 0; pass < 3; pass++) {
        if (pass == 2) nv2a_gpu_invalidate();
        for (vertex = 0; vertex < 3; vertex++) {
            vertices[vertex].diffuse[0] = pass == 1 ? 0 : 1;
            vertices[vertex].diffuse[1] = pass == 1 ? 1 : 0;
        }
        check("packed full-width partial-height scissor accepted", nv2a_gpu_draw(&state, vertices, 3));
        color[0] = 0xFF314159 + pass; color[255] = 0xFF271828 + pass;
        memcpy(expected_color, color, sizeof color);
        for (pixel = 64; pixel < 192; pixel++) expected_color[pixel] = pass == 1 ? 0xFF00FF00 : 0xFFFF0000;
        nv2a_gpu_flush();
        check("packed publication preserves outside rows and allocation tail", memcmp(expected_color, color, sizeof color) == 0);
    }
    nv2a_gpu_invalidate();
    state.width = state.clip_width = 128; state.pitch = 512;
    state.depth = (uint8_t *)depth; state.depth_enable = state.depth_mask = 1;
    vertices[1].position[0] = vertices[2].position[1] = 256;
    for (uint32_t format = 1; format <= 2; format++) {
        state.depth_format = format; state.depth_pitch = format == 1 ? 256 : 512;
        for (pixel = 0; pixel < 544; pixel++) {
            color[pixel] = 0xFF000000; depth[pixel] = 0xFFFFFF00 | (pixel & 255);
        }
        for (pass = 0; pass < 3; pass++) {
            if (pass == 2) nv2a_gpu_invalidate();
            for (vertex = 0; vertex < 3; vertex++) {
                vertices[vertex].position[2] = (float)(3 + pass);
                vertices[vertex].diffuse[0] = 1; vertices[vertex].diffuse[1] = 0;
            }
            check("packed depth partial-height scissor accepted", nv2a_gpu_draw(&state, vertices, 3));
            color[0] = 0xFF314159 + pass; color[511] = 0xFF271828 + pass;
            if (format == 1) {
                ((uint16_t *)depth)[0] = (uint16_t)(123 + pass);
                ((uint16_t *)depth)[511] = (uint16_t)(456 + pass);
            } else {
                depth[0] = 0x000011CD + pass; depth[511] = 0x000012B7 + pass;
            }
            memcpy(expected_color, color, sizeof color); memcpy(expected_depth, depth, sizeof depth);
            for (pixel = 128; pixel < 384; pixel++) {
                expected_color[pixel] = 0xFFFF0000;
                if (format == 1) ((uint16_t *)expected_depth)[pixel] = (uint16_t)(3 + pass);
                else expected_depth[pixel] = ((3 + pass) << 8) | (expected_depth[pixel] & 255);
            }
            nv2a_gpu_flush();
            check("packed depth draw preserves outside color rows and tail", memcmp(expected_color, color, sizeof color) == 0);
            check("packed depth publication preserves stencil outside rows and tail", memcmp(expected_depth, depth, sizeof depth) == 0);
        }
        nv2a_gpu_invalidate();
    }
}

static void test_gpu_rejected_outputs(void)
{
    uint32_t color[64], depth[64], saved_color[64], saved_depth[64];
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    uint32_t format, scenario, vertex, pixel;
    state.color = (uint8_t *)color; state.depth = (uint8_t *)depth;
    state.width = state.height = state.clip_width = state.clip_height = 8;
    state.pitch = state.depth_pitch = 32; state.bytes_per_pixel = 4;
    state.color_mask = 0x01010101; state.depth_enable = state.depth_mask = 1;
    vertices[1].position[0] = vertices[2].position[1] = 16;
    for (vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].position[2] = 100; vertices[vertex].position[3] = 1;
        vertices[vertex].diffuse[0] = vertices[vertex].diffuse[3] = 1;
    }
    for (format = 1; format <= 2; format++) {
        state.depth_format = format;
        for (pixel = 0; pixel < 64; pixel++) { color[pixel] = 0xFF112233; depth[pixel] = 0xFFFFFFA5; }
        state.depth_function = 0x200;
        check("NEVER depth draw executes", nv2a_gpu_draw(&state, vertices, 3));
        color[9] = 0xFF314159; depth[9] = 0x000011CD;
        memcpy(saved_color, color, sizeof color); memcpy(saved_depth, depth, sizeof depth);
        nv2a_gpu_flush();
        check("NEVER depth preserves concurrent CPU color", !memcmp(color, saved_color, sizeof color));
        check("NEVER depth preserves concurrent CPU depth and stencil", !memcmp(depth, saved_depth, sizeof depth));
        state.depth_function = 0x207;
        check("accepted output before rejected draw", nv2a_gpu_draw(&state, vertices, 3));
        state.depth_function = 0x200;
        check("rejected draw after pending output", nv2a_gpu_draw(&state, vertices, 3));
        nv2a_gpu_flush();
        check("rejection does not discard earlier dirty color", color[9] == 0xFFFF0000);
        if (format == 2) check("rejection does not discard earlier dirty depth/stencil", depth[9] == 0x000064CD);
        else {
            uint16_t stored; memcpy(&stored, (uint8_t *)depth + 34, 2);
            check("rejection does not discard earlier dirty Z16", stored == 100);
        }
        nv2a_gpu_invalidate();
    }
    state.depth_format = 2; state.stencil_enable = 1;
    state.stencil_read_mask = state.stencil_write_mask = 255; state.stencil_reference = 3;
    for (scenario = 0; scenario < 5; scenario++) {
        state.stencil_function = scenario == 0 || scenario == 3 ? 0x200 : 0x207;
        state.depth_function = scenario == 1 || scenario == 4 ? 0x200 : 0x207;
        state.depth_enable = scenario != 2;
        state.color_mask = scenario == 2 ? 0 : 0x01010101;
        state.stencil_fail = scenario == 0 ? 0x1E00 : 0x1E01;
        state.stencil_depth_fail = scenario == 1 ? 0x1E00 : 0x1E01;
        state.stencil_pass = scenario == 2 ? 0x1E00 : 0x1E01;
        for (pixel = 0; pixel < 64; pixel++) { color[pixel] = 0xFF112233; depth[pixel] = 0x000064A5; }
        check("stencil reachability draw executes", nv2a_gpu_draw(&state, vertices, 3));
        color[9] = 0xFF314159;
        if (scenario < 3) depth[9] = 0x000011CD;
        memcpy(saved_color, color, sizeof color); memcpy(saved_depth, depth, sizeof depth);
        if (scenario >= 3) saved_depth[9] = 0x00006403;
        nv2a_gpu_flush();
        check("unreachable stencil operations preserve CPU color", !memcmp(color, saved_color, sizeof color));
        if (scenario < 3)
            check("unreachable stencil operations preserve CPU depth", !memcmp(depth, saved_depth, sizeof depth));
        else check("reachable stencil failure operation still publishes", depth[9] == saved_depth[9]);
        nv2a_gpu_invalidate();
    }
}

static void fill_gpu_occlusion_depth(uint8_t *memory, uint32_t format, uint32_t width,
                                     uint32_t height, uint32_t pitch, uint32_t value)
{
    uint32_t row, column, bytes = format == 1 ? 2 : 4;
    uint32_t stored = format == 1 ? value : (value << 8) | 0xA5;
    for (row = 0; row < height; row++)
        for (column = 0; column < width; column++)
            memcpy(memory + row * pitch + column * bytes, &stored, bytes);
}

static void test_gpu_depth_download_precision(void)
{
    uint32_t format;
    for (format = 1; format <= 2; format++) {
        uint32_t width = format == 1 ? 257 : 1025, height = format == 1 ? 256 : 1024;
        uint32_t bytes = format == 1 ? 2 : 4, pitch = width * bytes + 6;
        uint32_t color_pitch = width * 4 + 12, chunks = format == 1 ? 1 : 16;
        uint8_t *color = (uint8_t *)malloc((size_t)color_pitch * height);
        uint8_t *depth = (uint8_t *)malloc((size_t)pitch * height);
        Nv2aGpuDraw state = {0};
        Nv2aGpuVertex vertices[3] = {0};
        uint32_t chunk, row, column, vertex;
        uint64_t mismatches = 0;
        check("depth precision buffers allocated", color && depth);
        if (!color || !depth) { free(color); free(depth); return; }
        memset(color, 0x35, (size_t)color_pitch * height);
        state.color = color; state.depth = depth;
        state.width = state.clip_width = width; state.height = state.clip_height = height;
        state.pitch = color_pitch; state.depth_pitch = pitch; state.bytes_per_pixel = 4;
        state.depth_format = format; state.depth_enable = state.depth_mask = 1; state.depth_function = 0x207;
        for (vertex = 0; vertex < 3; vertex++) {
            vertices[vertex].position[0] = -100 - (float)vertex;
            vertices[vertex].position[1] = -100;
            vertices[vertex].position[3] = 1;
        }
        // Offscreen draws force publication without changing the uploaded depth/stencil bits.
        for (chunk = 0; chunk < chunks; chunk++) {
            memset(depth, 0xA5, (size_t)pitch * height);
            for (row = 0; row < height; row++)
                for (column = 0; column < width; column++) {
                    uint32_t value = chunk * width * height + row * width + column;
                    uint32_t stored = format == 1 ? value & 65535u : (value << 8) | (value & 255u);
                    memcpy(depth + (size_t)row * pitch + column * bytes, &stored, bytes);
                }
            check("depth precision offscreen draw accepted", nv2a_gpu_draw(&state, vertices, 3));
            nv2a_gpu_flush();
            for (row = 0; row < height; row++) {
                for (column = 0; column < width; column++) {
                    uint32_t value = chunk * width * height + row * width + column, stored = 0;
                    uint32_t expected = format == 1 ? value & 65535u : (value << 8) | (value & 255u);
                    memcpy(&stored, depth + (size_t)row * pitch + column * bytes, bytes);
                    if (stored != expected) {
                        if (mismatches < 8)
                            printf("depth precision mismatch: D%u code %u expected %08X got %08X\n",
                                   format == 1 ? 16 : 24, value, expected, stored);
                        mismatches++;
                    }
                }
                for (column = width * bytes; column < pitch; column++)
                    check("depth precision preserves row padding", depth[(size_t)row * pitch + column] == 0xA5);
                for (column = 0; column < color_pitch; column++)
                    if (color[(size_t)row * color_pitch + column] != 0x35) mismatches++;
            }
        }
        printf("depth download precision D%u: %llu mismatches; all %u depth codes%s checked\n",
               format == 1 ? 16 : 24, (unsigned long long)mismatches, format == 1 ? 65536u : 16777216u,
               format == 1 ? "" : " and all 256 stencil values");
        check("depth download preserves every integer code", mismatches == 0);
        state.pitch = width * 4; state.depth_pitch = width * bytes; state.color_mask = 0x01010101;
        check("contiguous target comparison warm draw", nv2a_gpu_draw(&state, vertices, 3));
        nv2a_gpu_flush();
        for (chunk = 0; chunk < 3; chunk++) {
            size_t last = (size_t)width * height - 1;
            uint32_t mutation = chunk ? 1 : 0;
            uint32_t expected_color = 0xFF314159 ^ mutation;
            uint32_t expected_depth = (format == 1 ? 0xBEEF : 0x1234565A) ^ (mutation << 8);
            uint32_t actual_color, actual_depth = 0;
            if (chunk < 2) {
                memcpy(color + last * 4, &expected_color, 4);
                memcpy(depth + last * bytes, &expected_depth, bytes);
            }
            check("contiguous target comparison draw", nv2a_gpu_draw(&state, vertices, 3));
            nv2a_gpu_flush();
            memcpy(&actual_color, color + last * 4, 4);
            memcpy(&actual_depth, depth + last * bytes, bytes);
            check("contiguous comparison preserves last-pixel CPU color mutation", actual_color == expected_color);
            check("contiguous comparison preserves last-pixel CPU depth mutation", actual_depth == expected_depth);
        }
        nv2a_gpu_invalidate();
        free(color); free(depth);
    }
}

static void gpu_benchmark_flush(void)
{
    nv2a_gpu_flush_reason(getenv("NV2A_GPU_BENCH_IDLE") ? NV2A_GPU_SYNC_IDLE :
                         getenv("NV2A_GPU_BENCH_SEMAPHORE") ? NV2A_GPU_SYNC_SEMAPHORE : NV2A_GPU_SYNC_EXTERNAL);
}

static void test_gpu_occlusion_benchmark(void)
{
    enum { width = 640, height = 480, allocation_pitch = width * 4 + 16, iterations = 256 };
    static uint8_t color[allocation_pitch * height], depth[allocation_pitch * height];
    uint32_t pitch = getenv("NV2A_GPU_BENCH_CONTIGUOUS") ? width * 4 : allocation_pitch;
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    LARGE_INTEGER frequency, start, end;
    uint32_t row, column, vertex, pass, stored;
    state.color = color; state.depth = depth;
    state.width = state.clip_width = width; state.height = state.clip_height = height;
    state.pitch = state.depth_pitch = pitch; state.bytes_per_pixel = 4; state.depth_format = 2;
    state.color_mask = 0x01010101; state.depth_enable = state.depth_mask = 1; state.depth_function = 0x201;
    memset(color, 0x35, sizeof color); memset(depth, 0xA5, sizeof depth);
    fill_gpu_occlusion_depth(depth, 2, width, height, pitch, 50);
    vertices[1].position[0] = width * 2; vertices[2].position[1] = height * 2;
    for (vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].position[2] = 100; vertices[vertex].position[3] = 1;
        vertices[vertex].diffuse[0] = vertices[vertex].diffuse[3] = 1;
    }
    check("occlusion benchmark warm draw", nv2a_gpu_draw(&state, vertices, 3)); gpu_benchmark_flush();
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start);
    for (pass = 0; pass < iterations; pass++) {
        check("occlusion benchmark submission", nv2a_gpu_draw(&state, vertices, 3));
        gpu_benchmark_flush();
    }
    QueryPerformanceCounter(&end);
    printf("occlusion benchmark: %.3f ms, %u synchronized draws\n",
           (double)(end.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart, iterations);
    for (row = 0; row < height; row++) {
        for (column = 0; column < width; column++) {
            memcpy(&stored, color + row * pitch + column * 4, 4);
            check("occlusion benchmark complete color", stored == 0x35353535);
            memcpy(&stored, depth + row * pitch + column * 4, 4);
            check("occlusion benchmark complete depth/stencil", stored == 0x000032A5);
        }
        for (column = width * 4; column < pitch; column++) {
            check("occlusion benchmark color padding", color[row * pitch + column] == 0x35);
            check("occlusion benchmark depth padding", depth[row * pitch + column] == 0xA5);
        }
    }
    nv2a_gpu_invalidate();
    nv2a_gpu_report();
}

static void gpu_publication_benchmark_buffers(uint8_t *color, uint8_t *depth)
{
    enum { width = 640, height = 480, allocation_pitch = width * 4 + 16, iterations = 256 };
    uint32_t pitch = getenv("NV2A_GPU_BENCH_CONTIGUOUS") ? width * 4 : allocation_pitch;
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    uint32_t format, pass, row, column, vertex;
    state.color = color; state.depth = depth;
    state.width = state.clip_width = width; state.height = state.clip_height = height;
    state.pitch = state.depth_pitch = pitch; state.bytes_per_pixel = 4;
    state.color_mask = 0x01010101; state.depth_enable = state.depth_mask = 1;
    state.depth_function = 0x207;
    vertices[1].position[0] = width * 2; vertices[2].position[1] = height * 2;
    for (vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].position[3] = 1;
        vertices[vertex].diffuse[0] = vertices[vertex].diffuse[3] = 1;
    }
    for (format = 1; format <= 2; format++) {
        LARGE_INTEGER frequency, start, end;
        uint32_t bytes = format == 1 ? 2 : 4;
        state.depth_format = format;
        memset(color, 0x35, allocation_pitch * height); memset(depth, 0xA5, allocation_pitch * height);
        for (vertex = 0; vertex < 3; vertex++) vertices[vertex].position[2] = 100;
        check("publication benchmark warm draw", nv2a_gpu_draw(&state, vertices, 3));
        gpu_benchmark_flush();
        QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start);
        for (pass = 0; pass < iterations; pass++) {
            for (vertex = 0; vertex < 3; vertex++) vertices[vertex].position[2] = (float)(100 + pass);
            if (pass % 16 == 0) {
                for (row = 0; row < height; row++)
                    memset(depth + row * pitch, 0xA5, width * bytes);
            }
            check("publication benchmark draw", nv2a_gpu_draw(&state, vertices, 3));
            gpu_benchmark_flush();
        }
        QueryPerformanceCounter(&end);
        printf("publication benchmark D%u: %.3f ms, %u synchronized draws\n",
               format == 1 ? 16 : 24, (double)(end.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart,
               iterations);
        for (row = 0; row < height; row++) {
            for (column = 0; column < width; column++) {
                uint32_t stored_color, stored_depth = 0;
                memcpy(&stored_color, color + row * pitch + column * 4, 4);
                memcpy(&stored_depth, depth + row * pitch + column * bytes, bytes);
                check("publication benchmark exact color", stored_color == 0xFFFF0000);
                check("publication benchmark exact depth/stencil",
                      stored_depth == (format == 1 ? 100 + iterations - 1 : ((100 + iterations - 1) << 8) | 0xA5));
            }
            for (column = width * 4; column < pitch; column++)
                check("publication benchmark color padding", color[row * pitch + column] == 0x35);
            for (column = width * bytes; column < pitch; column++)
                check("publication benchmark depth padding", depth[row * pitch + column] == 0xA5);
        }
        nv2a_gpu_invalidate();
    }
    nv2a_gpu_report();
    {
        LARGE_INTEGER frequency, start, end;
        state.depth_function = 0x200;
        QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start);
        for (pass = 0; pass < iterations; pass++) {
            check("rejected-output benchmark draw", nv2a_gpu_draw(&state, vertices, 3));
            gpu_benchmark_flush();
        }
        QueryPerformanceCounter(&end);
        printf("publication benchmark NEVER: %.3f ms, %u synchronized draws\n",
               (double)(end.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart, iterations);
        nv2a_gpu_invalidate();
    }
    nv2a_gpu_report();
}

static void test_gpu_publication_benchmark(void)
{
    static uint8_t color[(640 * 4 + 16) * 480], depth[(640 * 4 + 16) * 480];
    gpu_publication_benchmark_buffers(color, depth);
}

typedef struct ResidentAccess {
    volatile uint32_t *memory;
    uint32_t value;
    int write;
} ResidentAccess;

static DWORD WINAPI gpu_resident_access_thread(void *context)
{
    ResidentAccess *access = (ResidentAccess *)context;
    if (access->write) *access->memory = access->value;
    else access->value = *access->memory;
    return 0;
}

static DWORD WINAPI gpu_resident_shutdown_thread(void *context)
{
    (void)context;
    nv2a_gpu_memory_shutdown();
    return 0;
}

static void gpu_resident_complete_access(HANDLE thread, int resident)
{
    if (resident) {
        DWORD start = GetTickCount();
        while (!nv2a_gpu_memory_pending() && GetTickCount() - start < 2000) Sleep(0);
        check("CPU access requests publication from render worker", nv2a_gpu_memory_pending());
        check("CPU access waits until publication completes", WaitForSingleObject(thread, 0) == WAIT_TIMEOUT);
        nv2a_gpu_memory_service();
    }
    DWORD result = WaitForSingleObject(thread, 5000);
    check("CPU access completes after publication", result == WAIT_OBJECT_0);
    if (result != WAIT_OBJECT_0) _Exit(EXIT_FAILURE);
    CloseHandle(thread);
}

static void test_gpu_resident_targets(uint8_t *primary, uint8_t *alias, uint8_t *short_alias,
                                      uint8_t *other, int resident)
{
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    uint32_t vertex, pass;
    state.color = primary; state.depth = primary + 8192;
    state.width = state.height = state.clip_width = state.clip_height = 8;
    state.pitch = state.depth_pitch = 40; state.bytes_per_pixel = 4; state.depth_format = 2;
    state.depth_enable = state.depth_mask = 1; state.depth_function = 0x207;
    state.color_mask = 0x01010101;
    vertices[1].position[0] = vertices[2].position[1] = 16;
    for (vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].position[2] = 100; vertices[vertex].position[3] = 1;
        vertices[vertex].diffuse[0] = vertices[vertex].diffuse[3] = 1;
    }
    memset(primary, 0xA5, 65536);
    check("resident mapped target draw", nv2a_gpu_draw(&state, vertices, 3));
    nv2a_gpu_flush_reason(NV2A_GPU_SYNC_SEMAPHORE);
    *(volatile uint32_t *)(primary + 16384) = 123;
    check("semaphore marker remains CPU-accessible", *(volatile uint32_t *)(primary + 16384) == 123);
    check("primary color alias protected after semaphore", nv2a_gpu_memory_is_protected(primary) == resident);
    check("second color alias protected after semaphore", nv2a_gpu_memory_is_protected(alias) == resident);
    check("truncated color alias protected after semaphore", nv2a_gpu_memory_is_protected(short_alias) == resident);
    check("depth alias protected after semaphore", nv2a_gpu_memory_is_protected(alias + 8192) == resident);
    check("alias read sees completed GPU color", *(volatile uint32_t *)(alias + 132) == 0xFFFF0000);
    check("alias read sees exact GPU depth and stencil", *(volatile uint32_t *)(alias + 8192 + 132) == 0x000064A5);
    check("publication restores both CPU aliases", !nv2a_gpu_memory_is_protected(primary) && !nv2a_gpu_memory_is_protected(alias));
    for (pass = 0; pass < 8; pass++)
        check("mapped publication preserves row padding", *(uint32_t *)(primary + pass * 40 + 32) == 0xA5A5A5A5);
    check("publication preserves unrelated bytes in protected page", *(uint32_t *)(primary + 2048) == 0xA5A5A5A5);

    {
        const char *setting = getenv("RECOMP_NV2A_GPU_RESIDENT_IDLE");
        int idle_resident = resident && setting && *setting && strcmp(setting, "0") != 0;
        const char *asynchronous = getenv("RECOMP_NV2A_GPU_ASYNC_IDLE");
        int asynchronous_idle = idle_resident && (!asynchronous || (*asynchronous && strcmp(asynchronous, "0") != 0));
        Nv2aGpuSyncCounters before = nv2a_gpu_sync_counters();
        vertices[0].position[2] = vertices[1].position[2] = vertices[2].position[2] = 125;
        check("idle resident draw", nv2a_gpu_draw(&state, vertices, 3));
        nv2a_gpu_flush_reason(NV2A_GPU_SYNC_IDLE);
        Nv2aGpuSyncCounters after = nv2a_gpu_sync_counters();
        check("idle skips completion wait only for fully guarded targets",
              after.completion_waits == before.completion_waits + (idle_resident && !asynchronous_idle ? 1 : 0));
        check("idle asynchronous counter matches enabled guarded scheduling",
              after.asynchronous_idle_boundaries == before.asynchronous_idle_boundaries + asynchronous_idle);
        check("idle retains primary guard only when enabled", nv2a_gpu_memory_is_protected(primary) == idle_resident);
        check("idle retains alias guard only when enabled", nv2a_gpu_memory_is_protected(alias) == idle_resident);
        check("idle retains depth guard only when enabled", nv2a_gpu_memory_is_protected(alias + 8192) == idle_resident);
        nv2a_gpu_flush_reason(NV2A_GPU_SYNC_SEMAPHORE);
        after = nv2a_gpu_sync_counters();
        check("semaphore completes outstanding idle work without a new draw",
              after.completion_waits == before.completion_waits + (idle_resident ? 1 : 0));
        nv2a_gpu_flush_reason(NV2A_GPU_SYNC_SEMAPHORE);
        check("completed semaphore does not repeat its GPU completion wait",
              nv2a_gpu_sync_counters().completion_waits == after.completion_waits);
        check("idle CPU read sees exact completed depth", *(volatile uint32_t *)(alias + 8192 + 132) == 0x00007DA5);
        check("idle CPU read sees completed color", *(volatile uint32_t *)(alias + 132) == 0xFFFF0000);
        check("idle CPU access releases guards", !nv2a_gpu_memory_is_protected(primary));
        vertices[0].position[2] = vertices[1].position[2] = vertices[2].position[2] = 100;
    }

    for (pass = 0; pass < 8; pass++) {
        check("repeated resident draw", nv2a_gpu_draw(&state, vertices, 3));
        nv2a_gpu_flush_reason(pass & 1 ? NV2A_GPU_SYNC_SEMAPHORE : NV2A_GPU_SYNC_IDLE);
    }
    ResidentAccess access = {(volatile uint32_t *)(alias + 132), 0xFF314159, 1};
    HANDLE thread = CreateThread(NULL, 0, gpu_resident_access_thread, &access, 0, NULL);
    check("CPU alias writer thread created", thread != NULL);
    if (!thread) _Exit(EXIT_FAILURE);
    gpu_resident_complete_access(thread, resident);
    check("CPU write occurs after GPU publication", *(uint32_t *)(primary + 132) == 0xFF314159);
    state.clip_x = state.clip_y = 4; state.clip_width = state.clip_height = 4;
    for (vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].diffuse[0] = 0; vertices[vertex].diffuse[1] = 1;
    }
    check("resident draw after CPU alias write", nv2a_gpu_draw(&state, vertices, 3));
    nv2a_gpu_flush_reason(NV2A_GPU_SYNC_SEMAPHORE);
    check("CPU write outside next scissor survives refresh", *(volatile uint32_t *)(alias + 132) == 0xFF314159);
    check("next scissor publishes new GPU color", *(uint32_t *)(primary + 220) == 0xFF00FF00);
    state.clip_x = state.clip_y = 0; state.clip_width = state.clip_height = 8;
    check("mapped clear accepted", nv2a_gpu_clear(&state, 0xF0, 0xFF102030, 0) == 1);
    nv2a_gpu_flush_reason(NV2A_GPU_SYNC_SEMAPHORE);
    check("mapped clear publication on alias access", *(volatile uint32_t *)(alias + 132) == 0xFF102030);

    state.color = alias; state.depth = alias + 8192;
    check("draw through second mapped target alias", nv2a_gpu_draw(&state, vertices, 3));
    nv2a_gpu_flush_reason(NV2A_GPU_SYNC_SEMAPHORE);
    check("GPU alias draw visible through primary mapping", *(volatile uint32_t *)(primary + 132) == 0xFF00FF00);
    state.color = primary; state.depth = primary + 8192;
    check("draw before simultaneous CPU reads", nv2a_gpu_draw(&state, vertices, 3));
    nv2a_gpu_flush_reason(NV2A_GPU_SYNC_SEMAPHORE);
    ResidentAccess readers[4];
    HANDLE threads[4];
    for (pass = 0; pass < 4; pass++) {
        readers[pass] = (ResidentAccess){(volatile uint32_t *)(alias + 132), 0, 0};
        threads[pass] = CreateThread(NULL, 0, gpu_resident_access_thread, &readers[pass], 0, NULL);
        check("simultaneous CPU reader created", threads[pass] != NULL);
        if (!threads[pass]) _Exit(EXIT_FAILURE);
    }
    if (resident) {
        DWORD start = GetTickCount();
        while (!nv2a_gpu_memory_pending() && GetTickCount() - start < 2000) Sleep(0);
        check("simultaneous readers request publication", nv2a_gpu_memory_pending());
        Sleep(5);
    }
    memset(memory + 0x18000 / 4, 0, 4096);
    nv2a_pb_scan(0x18000, 0x19000);
    check("pushbuffer scan services waiting CPU readers", !nv2a_gpu_memory_pending());
    check("pushbuffer scan preserves command consumption", nv2a_registers[0x800044 / 4] == 0x19000);
    DWORD result = WaitForMultipleObjects(4, threads, TRUE, 5000);
    check("all CPU readers resume after publication", result == WAIT_OBJECT_0);
    if (result != WAIT_OBJECT_0) _Exit(EXIT_FAILURE);
    for (pass = 0; pass < 4; pass++) {
        check("all CPU readers observe exact completed output", readers[pass].value == 0xFF00FF00);
        CloseHandle(threads[pass]);
    }
    check("draw before protection metadata query", nv2a_gpu_draw(&state, vertices, 3));
    nv2a_gpu_flush_reason(NV2A_GPU_SYNC_SEMAPHORE);
    MEMORY_BASIC_INFORMATION information = {0};
    check("guest protection query publishes before exposing metadata",
          nv2a_gpu_memory_virtual_query(alias, &information, sizeof information) && information.Protect == PAGE_READWRITE);
    check("draw before CPU protection change", nv2a_gpu_draw(&state, vertices, 3));
    nv2a_gpu_flush_reason(NV2A_GPU_SYNC_SEMAPHORE);
    DWORD previous;
    check("CPU protection change publishes protected target first",
          nv2a_gpu_memory_virtual_protect(alias, 4096, PAGE_READONLY, &previous));
    check("CPU protection change retains completed bytes", *(uint32_t *)(alias + 132) == 0xFF00FF00);
    check("restore test alias write protection", nv2a_gpu_memory_virtual_protect(alias, 4096, PAGE_READWRITE, &previous));
    nv2a_gpu_invalidate();

    uint32_t untracked_depth[80];
    memset(untracked_depth, 0xA5, sizeof untracked_depth);
    state.depth = (uint8_t *)untracked_depth;
    check("mixed tracked color and untracked depth draw", nv2a_gpu_draw(&state, vertices, 3));
    nv2a_gpu_flush_reason(NV2A_GPU_SYNC_SEMAPHORE);
    check("untracked depth is eagerly published at semaphore", untracked_depth[33] == 0x000064A5);
    check("mixed tracked color remains guarded", nv2a_gpu_memory_is_protected(primary) == resident);
    check("truncated alias reads exact mixed target output", *(volatile uint32_t *)(short_alias + 132) == 0xFF00FF00);
    nv2a_gpu_invalidate();

    state.depth = primary + 8192;
    check("source draw before mapped texture resolve", nv2a_gpu_draw(&state, vertices, 3));
    nv2a_gpu_flush_reason(NV2A_GPU_SYNC_SEMAPHORE);
    state.color = other; state.depth_enable = state.depth_mask = 0;
    state.stage_program = 1; state.combiners.control = 1;
    state.combiners.rgb_input[0] = 0x08200000; state.combiners.alpha_input[0] = 0x00002018;
    state.combiners.rgb_output[0] = state.combiners.alpha_output[0] = 0xC00;
    state.combiners.final_input[0] = 0xC; state.combiners.final_input[1] = 0x1C80;
    state.textures[0].source = alias; state.textures[0].source_bytes = 320;
    state.textures[0].width = state.textures[0].height = 8;
    state.textures[0].pitch = 40; state.textures[0].linear = 1; state.textures[0].format = 0x12;
    state.textures[0].decode = gpu_decode; state.textures[0].decode_context = alias + 132;
    for (vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].texture[0][0] = vertices[vertex].texture[0][1] = 0.5f;
        vertices[vertex].texture[0][3] = 1;
        vertices[vertex].diffuse[0] = vertices[vertex].diffuse[2] = 1;
    }
    check("texture alias of resident target resolves into second section", nv2a_gpu_draw(&state, vertices, 3));
    nv2a_gpu_flush_reason(NV2A_GPU_SYNC_SEMAPHORE);
    check("second section color remains guarded", nv2a_gpu_memory_is_protected(other) == resident);
    check("mapped texture alias resolves exact GPU color", *(volatile uint32_t *)(other + 132) == 0xFF00FF00);

    Nv2aGpuDraw source = state;
    source.color = primary; source.stage_program = 0;
    memset(&source.combiners, 0, sizeof source.combiners);
    memset(source.textures, 0, sizeof source.textures);
    check("source redraw before owner-thread decoder access", nv2a_gpu_draw(&source, vertices, 3));
    nv2a_gpu_flush_reason(NV2A_GPU_SYNC_SEMAPHORE);
    state.textures[0].source = alias + 132; state.textures[0].source_bytes = 4;
    state.textures[0].width = state.textures[0].height = 1;
    state.textures[0].pitch = 4; state.textures[0].format = 0;
    check("owner-thread decoder reads guarded source without deadlock", nv2a_gpu_draw(&state, vertices, 3));
    nv2a_gpu_flush_reason(NV2A_GPU_SYNC_SEMAPHORE);
    check("owner-thread decoder sees exact published source", *(volatile uint32_t *)(other + 132) == 0xFFFFFFFF);
    nv2a_gpu_invalidate();
}

static void test_gpu_resident_eligibility(uint8_t *primary, uint8_t *alias)
{
    DWORD previous;
    check("residency rejects empty span", !nv2a_gpu_memory_can_reside(primary, 0));
    check("residency accepts unaligned writable span", nv2a_gpu_memory_can_reside(primary + 1, 8191));
    check("residency fixture readonly boundary", nv2a_gpu_memory_virtual_protect(primary + 4096, 4096, PAGE_READONLY, &previous));
    check("residency rejects span crossing readonly boundary", !nv2a_gpu_memory_can_reside(primary, 12288));
    check("residency accepts writable tail after readonly boundary", nv2a_gpu_memory_can_reside(primary + 8192, 8192));
    check("residency protects mixed original permissions", nv2a_gpu_memory_protect(primary + 4096, 4096));
    check("residency rejects saved readonly permission", !nv2a_gpu_memory_can_reside(primary + 4096, 4096));
    check("residency accepts writable alias saved permission", nv2a_gpu_memory_can_reside(alias + 4096, 4096));
    for (uint32_t pass = 0; pass < 16; pass++) {
        check("cached writable eligibility remains alias-specific", nv2a_gpu_memory_can_reside(alias + 4096, 4096));
        check("cached eligibility does not accept readonly primary", !nv2a_gpu_memory_can_reside(primary + 4096, 4096));
        check("already-guarded protection preserves saved permissions", nv2a_gpu_memory_protect(alias + 4096, 4096));
    }
    nv2a_gpu_memory_release();
    check("cached alias protection can change after publication",
          nv2a_gpu_memory_virtual_protect(alias + 4096, 4096, PAGE_READONLY, &previous));
    check("publication invalidates cached writable eligibility", !nv2a_gpu_memory_can_reside(alias + 4096, 4096));
    check("eligibility fixture restores alias permission",
          nv2a_gpu_memory_virtual_protect(alias + 4096, 4096, PAGE_READWRITE, &previous));
    nv2a_gpu_memory_release();
    check("residency fixture restores writable boundary", nv2a_gpu_memory_virtual_protect(primary + 4096, 4096, PAGE_READWRITE, &previous));
    check("residency protects middle page", nv2a_gpu_memory_protect(primary + 8192, 4096));
    check("residency walks writable and already-protected regions", nv2a_gpu_memory_can_reside(primary + 1, 16383));
    nv2a_gpu_memory_release();
    if (getenv("NV2A_GPU_RESIDENT_LARGE_MAPPING")) {
        enum { iterations = 256, target_bytes = 640 * 480 * 4 };
        LARGE_INTEGER frequency, start, end;
        uint32_t pass;
        memset(primary, 0, 64 * 1024 * 1024);
        QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start);
        for (pass = 0; pass < iterations; pass++)
            check("large-region residency eligibility", nv2a_gpu_memory_can_reside(primary, target_bytes));
        QueryPerformanceCounter(&end);
        printf("resident eligibility benchmark: %.3f ms, %u checks, %u target pages in 64 MiB mapping\n",
               (double)(end.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart,
               iterations, target_bytes / 4096);
    }
}

static void test_gpu_projected_target(uint8_t *primary, uint8_t *alias, uint8_t *other, int resident)
{
    Nv2aGpuDraw source = {0}, destination;
    Nv2aGpuVertex vertices[3] = {0};
    memset(primary, 0xA5, 65536);
    source.color = primary; source.width = source.height = source.clip_width = 8;
    source.clip_height = 1; source.pitch = 40; source.bytes_per_pixel = 4;
    source.color_mask = 0x01010101;
    vertices[1].position[0] = vertices[2].position[1] = 16;
    for (uint32_t vertex = 0; vertex < 3; vertex++) vertices[vertex].position[3] = 1;
    for (uint32_t row = 0; row < 8; row++) {
        source.clip_y = row;
        for (uint32_t vertex = 0; vertex < 3; vertex++)
            nv_cpu_unpack_argb(row & 1 ? 0x7706080A : 0x11020406, vertices[vertex].diffuse);
        check("projected fixture draws alternating padded target rows", nv2a_gpu_draw(&source, vertices, 3));
    }
    destination = source; destination.color = other;
    destination.clip_y = 0; destination.clip_height = 8;
    destination.stage_program = 1; destination.combiners.control = 1;
    destination.combiners.rgb_input[0] = 0x08200000; destination.combiners.alpha_input[0] = 0x00002018;
    destination.combiners.rgb_output[0] = destination.combiners.alpha_output[0] = 0xC00;
    destination.combiners.final_input[0] = 0xC; destination.combiners.final_input[1] = 0x1C80;
    destination.textures[0].source = resident ? alias : primary; destination.textures[0].source_bytes = 320;
    destination.textures[0].width = destination.textures[0].height = 8;
    destination.textures[0].pitch = 40; destination.textures[0].linear = 1;
    destination.textures[0].format = 0x1E; destination.textures[0].filter = 0x02020000;
    destination.textures[0].decode = gpu_decode; destination.textures[0].decode_context = primary;
    uint32_t decode_before = gpu_decode_calls;
    for (uint32_t row = 0; row <= 8; row++) {
        float q = row & 1 ? 2 : 1;
        for (uint32_t vertex = 0; vertex < 3; vertex++) {
            vertices[vertex].position[3] = 1;
            vertices[vertex].texture[0][0] = 3.5f * q;
            vertices[vertex].texture[0][1] = (row == 8 ? 3.0f : row + 0.5f) * q;
            vertices[vertex].texture[0][3] = q;
        }
        check("projective LIN_X8R8G8B8 target sampling accepted", nv2a_gpu_draw(&destination, vertices, 3));
        nv2a_gpu_flush_reason(NV2A_GPU_SYNC_IDLE);
        uint32_t expected = row == 8 ? 0xFF040608 : row & 1 ? 0xFF06080A : 0xFF020406;
        uint32_t actual = *(volatile uint32_t *)(other + 132);
        if (actual != expected)
            fprintf(stderr, "projected target row %u: actual %08X expected %08X\n", row, actual, expected);
        check("projective padded target keeps rows, filtering, Q division and opaque X8 alpha",
              actual == expected);
    }
    check("projected target sampling avoids CPU texture decoding", gpu_decode_calls == decode_before);
    nv2a_gpu_invalidate();
    for (uint32_t row = 0; row < 8; row++)
        check("projected target publication preserves row padding",
              *(uint32_t *)(primary + row * 40 + 32) == 0xA5A5A5A5);
}

static void test_gpu_shadow_far_plane(void)
{
    enum { width = 256, height = 256 };
    uint32_t *color = (uint32_t *)malloc(width * height * sizeof *color);
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[6] = {0};
    const float positions[6][2] = {{0,0}, {256,0}, {0,256}, {256,0}, {256,256}, {0,256}};
    const float depths[] = {16777215.0f, 16777214.0f, 8388607.0f};
    const float weights[] = {1.0f, 16777.2f, 3.14159f};
    check("shadow-map fixture allocated", color != NULL);
    if (!color) _Exit(EXIT_FAILURE);
    state.color = (uint8_t *)color; state.width = state.height = 255;
    state.pitch = 1024; state.bytes_per_pixel = 4; state.depth_format = 2;
    state.clip_x = state.clip_y = 1; state.clip_width = state.clip_height = 254;
    state.color_mask = 0x01010101; state.zmin_max_control = 1;
    for (uint32_t depth = 0; depth < 3; depth++)
        for (uint32_t weight = 0; weight < 3; weight++) {
            for (uint32_t pixel = 0; pixel < width * height; pixel++) color[pixel] = 0xFF102030;
            for (uint32_t vertex = 0; vertex < 6; vertex++) {
                vertices[vertex].position[0] = positions[vertex][0] + 0.53125f;
                vertices[vertex].position[1] = positions[vertex][1] + 0.53125f;
                vertices[vertex].position[2] = depths[depth];
                vertices[vertex].position[3] = weights[weight];
                vertices[vertex].diffuse[0] = vertices[vertex].diffuse[3] = 1;
            }
            check("far-plane shadow-map draw accepted", nv2a_gpu_draw(&state, vertices, 6));
            nv2a_gpu_flush();
            uint32_t holes = 0;
            for (uint32_t y = 2; y < 253; y++)
                for (uint32_t x = 2; x < 253; x++)
                    if (color[y * width + x] != 0xFFFF0000) holes++;
            printf("shadow far plane: Z %.9g W %.9g, %u interior holes\n", depths[depth], weights[weight], holes);
            check("constant-depth shadow silhouette has no interior stripes", holes == 0);
            nv2a_gpu_invalidate();
        }
    free(color);
}

static void test_gpu_shadow_source(void)
{
    uint32_t color[64] = {0}, depth[64], texel = 0xFF203040;
    uint32_t *texture = (uint32_t *)malloc(128 * 128 * sizeof *texture);
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    check("swizzled shadow-source fixture allocated", texture != NULL);
    if (!texture) _Exit(EXIT_FAILURE);
    for (uint32_t pixel = 0; pixel < 128 * 128; pixel++) texture[pixel] = texel;
    for (uint32_t pixel = 0; pixel < 64; pixel++) depth[pixel] = 0xFFFFFFA5;
    state.color = (uint8_t *)color; state.depth = (uint8_t *)depth;
    state.width = state.height = state.clip_width = state.clip_height = 8;
    state.pitch = state.depth_pitch = 32; state.bytes_per_pixel = 4;
    state.depth_enable = state.depth_mask = 1; state.depth_format = 2; state.depth_function = 0x207;
    state.color_mask = 0x01010101; state.stage_program = 1;
    state.combiners.control = 1; state.combiners.rgb_input[0] = 0x08200000;
    state.combiners.alpha_input[0] = 0x00002018;
    state.combiners.rgb_output[0] = state.combiners.alpha_output[0] = 0xC00;
    state.combiners.final_input[0] = 0xC; state.combiners.final_input[1] = 0x1C80;
    state.textures[0].source = (uint8_t *)texture; state.textures[0].source_bytes = 128 * 128 * 4;
    state.textures[0].width = state.textures[0].height = 128; state.textures[0].format = 0x06;
    state.textures[0].decode = gpu_decode; state.textures[0].decode_context = &texel;
    vertices[1].position[0] = vertices[2].position[1] = 16;
    for (uint32_t vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].position[2] = 100; vertices[vertex].position[3] = 1;
        vertices[vertex].texture[0][0] = vertices[vertex].texture[0][1] = 0.5f;
        vertices[vertex].texture[0][3] = 1;
    }
    const uint32_t indices[] = {0, 0, 1, 0, 1, 2};
    state.indices = indices; state.index_count = 6;
    check("swizzled projected source draw accepted", nv2a_gpu_draw(&state, vertices, 3));
    nv2a_gpu_flush();
    check("shadow-source diagnostics preserve color", color[9] == texel);
    check("shadow-source diagnostics preserve depth and stencil", depth[9] == 0x000064A5);
    nv2a_gpu_invalidate();
    free(texture);
}

static void test_gpu_depth_textures(void)
{
    const uint32_t formats[] = {0x2C, 0x30, 0x2E};
    uint32_t color[64] = {0}, cases = 0;
    uint8_t source[80];
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    gpu_texture_cache_setup(&state, vertices, color);
    state.stage_program = 2;
    state.textures[0].source = source;
    state.textures[0].decode = NULL;
    state.textures[0].width = state.textures[0].height = 4;
    state.textures[0].address_u = state.textures[0].address_v = 3;
    state.textures[0].filter = 0x01010000;
    for (uint32_t format = 0; format < 3; format++) {
        Nv2aGpuTexture *texture = &state.textures[0];
        uint32_t bytes = nv_texture_depth_bytes(formats[format]);
        uint32_t raw = bytes == 4 ? 0x803456 : 12345;
        texture->format = formats[format]; texture->linear = format != 0;
        texture->pitch = texture->linear ? 4 * bytes + 4 : 0;
        texture->source_bytes = texture->linear ? texture->pitch * 4 : 4 * 4 * bytes;
        memset(source, 0xA5, sizeof source);
        for (uint32_t y = 0; y < 4; y++)
            for (uint32_t x = 0; x < 4; x++) {
                uint32_t value = x == 2 && y == 0 ? raw : (y * 4 + x) * 100;
                uint32_t offset = texture->linear ? y * texture->pitch + x * bytes :
                    swizzle_offset(x, y, 4, 4) * bytes;
                if (bytes == 4) value = (value << 8) | 0xA5;
                memcpy(source + offset, &value, bytes);
            }
        float decoded;
        check("depth texel decoder accepts padded/Morton data",
              nv_texture_depth_texel(source, texture->source_bytes, 4, 4, texture->pitch,
                                     texture->format, 2, 0, &decoded));
        check("depth texture preserves adjacent integer precision", decoded * nv_texture_depth_max(texture->format) == raw);
        check("depth texel decoder rejects truncated spans",
              !nv_texture_depth_texel(source, 1, 4, 4, texture->pitch, texture->format, 2, 0, &decoded));
        const float references[] = {0, (float)raw - 1, (float)raw, (float)raw + 1,
                                     nv_texture_depth_max(texture->format)};
        for (uint32_t function = 0; function < 8; function++)
            for (uint32_t reference = 0; reference < 5; reference++) {
                state.shadow_depth_function = function;
                for (uint32_t vertex = 0; vertex < 3; vertex++) {
                    vertices[vertex].texture[0][0] = (texture->linear ? 2.5f : 2.5f / 4) * 2;
                    vertices[vertex].texture[0][1] = (texture->linear ? 0.5f : 0.5f / 4) * 2;
                    vertices[vertex].texture[0][2] = references[reference] * 2;
                    vertices[vertex].texture[0][3] = 2;
                }
                check("projective fixed-depth draw accepted", nv2a_gpu_draw(&state, vertices, 3));
                nv2a_gpu_flush();
                uint32_t expected = nv_texture_depth_compare(function, (float)raw, references[reference]) ?
                    0xFFFFFFFFu : 0;
                check("all shadow comparisons preserve depth precision and Q division", color[9] == expected);
                cases++;
            }
        uint32_t maximum = (uint32_t)nv_texture_depth_max(texture->format);
        for (uint32_t y = 0; y < 4; y++)
            for (uint32_t x = 0; x < 4; x++) {
                uint32_t value = (x & 1) ? maximum : 0;
                uint32_t offset = texture->linear ? y * texture->pitch + x * bytes :
                    swizzle_offset(x, y, 4, 4) * bytes;
                if (bytes == 4) value = (value << 8) | 0x5A;
                memcpy(source + offset, &value, bytes);
            }
        state.shadow_depth_function = 4;
        texture->filter = 0x02020000;
        for (uint32_t vertex = 0; vertex < 3; vertex++) {
            vertices[vertex].texture[0][0] = texture->linear ? 1.0f : 0.25f;
            vertices[vertex].texture[0][1] = texture->linear ? 0.5f : 0.125f;
            vertices[vertex].texture[0][2] = maximum * 0.25f;
            vertices[vertex].texture[0][3] = 1;
        }
        check("bilinear depth texture draw accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
        check("depth is filtered before comparison, not comparison-filtered", color[9] == 0xFFFFFFFFu);
        texture->filter = 0x01010000;
        state.shadow_depth_function = 2;
        for (uint32_t boundary = 0; boundary < 2; boundary++) {
            for (uint32_t vertex = 0; vertex < 3; vertex++) {
                vertices[vertex].texture[0][0] = texture->linear ? boundary + 0.5f : (boundary + 0.5f) / 4;
                vertices[vertex].texture[0][2] = boundary ? maximum * 2.0f : -1.0f;
            }
            check("out-of-range shadow reference draw accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
            check("shadow reference clamps exactly at zero and maximum depth", color[9] == 0xFFFFFFFFu);
        }
        state.stage_program = 1;
        state.shadow_depth_function = 3;
        check("PROJECT2D fixed-depth draw accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
        check("PROJECT2D depth compares against zero rather than coordinate Z", color[9] == 0);
        state.stage_program = 2;
        texture->filter = 0x01010000;
        nv2a_gpu_invalidate();
    }
    printf("fixed-depth textures: %u projective comparison cases checked\n", cases);
}

static void test_depth_texture_commands(void)
{
    float (*vertices)[3][4] = (float (*)[3][4])((uint8_t *)memory + 0x4000);
    memset(vertices, 0, 3 * sizeof vertices[0]);
    vertices[1][0][0] = vertices[2][0][1] = 8;
    for (uint32_t vertex = 0; vertex < 3; vertex++) {
        vertices[vertex][0][3] = 1;
        vertices[vertex][2][0] = vertices[vertex][2][1] = 0.5f;
        vertices[vertex][2][2] = 12000; vertices[vertex][2][3] = 2;
    }
    method(0x0200, 8u << 16); method(0x0204, 8u << 16);
    method(0x0208, 0x28);
    method(0x020C, 32); method(0x0210, 0x1000);
    method(0x0290, 0); method(0x0300, 0); method(0x0304, 0); method(0x0308, 0);
    method(0x030C, 0); method(0x032C, 0); method(0x0358, 0x01010101);
    method(0x1D98, 7u << 16); method(0x1D9C, 7u << 16);
    const uint32_t attributes[] = {0, 3, 9};
    for (uint32_t attribute = 0; attribute < 3; attribute++) {
        method(0x1720 + attributes[attribute] * 4, 0x4000 + attribute * 16);
        method(0x1760 + attributes[attribute] * 4, (sizeof vertices[0] << 8) | 0x42);
    }
    upload_program();
    method(0x1E60, 1); method(0x1E70, 2);
    method(0x0AC0, 0x08200000); method(0x0260, 0x00002018);
    method(0x1E40, 0xC00); method(0x0AA0, 0xC00);
    method(0x0288, 0xC); method(0x028C, 0x1C80);
    uint16_t *depth = (uint16_t *)((uint8_t *)memory + 0x3000);
    for (uint32_t texel = 0; texel < 16; texel++) depth[texel] = 10000;
    method(0x1B00, 0x3000);
    method(0x1B04, (0x2Cu << 8) | (2u << 4) | (1u << 16) | (2u << 20) | (2u << 24));
    method(0x1B08, 0x303); method(0x1B0C, 1u << 30); method(0x1B14, 0x01010000);
    for (uint32_t function = 0; function < 8; function++) {
        method(0x1E6C, function);
        draw(5, 3);
        int pass = nv_texture_depth_compare(function, 10000, 6000);
        uint32_t actual = memory[0x1000 / 4 + 3 * 8 + 3];
        check("pushbuffer shadow control and depth format reach native renderer",
              actual == (pass ? 0xFFFFFFFFu : 0));
    }
    nv2a_gpu_invalidate();
    printf("depth texture pushbuffer: D3D11, eight comparisons checked\n");
}

static void test_gpu_depth_mips_and_alias(void)
{
    uint16_t source[21] = {0};
    uint32_t color[64] = {0};
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    gpu_texture_cache_setup(&state, vertices, color);
    state.stage_program = 2; state.shadow_depth_function = 4;
    Nv2aGpuTexture *texture = &state.textures[0];
    texture->source = (uint8_t *)source; texture->source_bytes = sizeof source;
    texture->width = texture->height = 4; texture->format = 0x2C; texture->pitch = 0;
    texture->decode = NULL; texture->mip_levels = 3; texture->filter = 0x02060000;
    texture->control0_valid = 1; texture->control0 = (1u << 30) | (256u << 18) | (256u << 6);
    for (uint32_t pixel = 16; pixel < 20; pixel++) source[pixel] = 65535;
    for (uint32_t vertex = 0; vertex < 3; vertex++) vertices[vertex].texture[0][2] = 32767;
    check("swizzled D16 mip-chain draw accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
    check("depth sampler selects populated first mip", color[9] == 0xFFFFFFFFu);
    texture->control0 = (1u << 30) | (512u << 18) | (512u << 6);
    check("D16 sampler switches mip without changing resource", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
    check("depth sampler selects independent final mip", color[9] == 0);
    nv2a_gpu_invalidate();

    uint32_t depth[20] = {0};
    Nv2aGpuDraw target = {0};
    target.depth = (uint8_t *)depth; target.width = target.height = 4;
    target.clip_width = target.clip_height = 4; target.depth_pitch = 20; target.depth_format = 2;
    check("native depth/stencil source clear accepted", nv2a_gpu_clear(&target, 3, 0, 0xFFFFFFA5));
    texture->source = (uint8_t *)depth; texture->source_bytes = sizeof depth;
    texture->format = 0x2E; texture->linear = 1; texture->pitch = 20; texture->mip_levels = 1;
    texture->control0_valid = 0; texture->filter = 0x01010000;
    for (uint32_t vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].texture[0][0] = vertices[vertex].texture[0][1] = 1.5f;
        vertices[vertex].texture[0][2] = 8388607;
    }
    check("dirty depth target can be sampled as D24 texture", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
    check("depth alias publication preserves mask depth and stencil", color[9] == 0xFFFFFFFFu && depth[6] == 0xFFFFFFA5);
    depth[6] = 0xFFFFFF5A;
    check("D24 stencil-only source mutation accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
    check("stencil bits do not enter shadow comparison", color[9] == 0xFFFFFFFFu);
    nv2a_gpu_invalidate();
}

typedef struct ShadowMaskGrid {
    uint32_t pixels[32 * 16], opaque;
} ShadowMaskGrid;

static int gpu_decode_shadow_grid(void *context, uint32_t x, uint32_t y, uint32_t *color)
{
    const ShadowMaskGrid *grid = (const ShadowMaskGrid *)context;
    if (x >= 32 || y >= 16) return 0;
    *color = grid->pixels[swizzle_offset(x, y, 32, 16)] | (grid->opaque ? 0xFF000000u : 0);
    return 1;
}

static void test_gpu_structured_shadow_mask(void)
{
    ShadowMaskGrid grid = {0};
    uint32_t color[64] = {0};
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[6] = {0};
    gpu_texture_cache_setup(&state, vertices, color);
    const float positions[6][2] = {{0,0},{8,0},{0,8},{8,0},{8,8},{0,8}};
    for (uint32_t vertex = 0; vertex < 6; vertex++) {
        vertices[vertex].position[0] = positions[vertex][0];
        vertices[vertex].position[1] = positions[vertex][1];
        vertices[vertex].position[3] = 1;
        vertices[vertex].texture[0][0] = positions[vertex][0] * 3 / 8;
        vertices[vertex].texture[0][1] = positions[vertex][1] * 3 / 8;
        vertices[vertex].texture[0][3] = 3;
    }
    Nv2aGpuTexture *texture = &state.textures[0];
    texture->source = (uint8_t *)grid.pixels; texture->source_bytes = sizeof grid.pixels;
    texture->width = 32; texture->height = 16; texture->pitch = 0;
    texture->address_u = texture->address_v = 3; texture->filter = 0x01010000;
    texture->decode = gpu_decode_shadow_grid; texture->decode_context = &grid;
    check("non-square Morton reference offsets", swizzle_offset(2, 1, 32, 16) == 6 &&
          swizzle_offset(31, 15, 32, 16) == 511);
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < 32; x++)
            grid.pixels[swizzle_offset(x, y, 32, 16)] =
                ((y & 1) ? 0x40000000u : 0xC0000000u) | (x << 16) | (y << 8) | ((x ^ y) * 4);
    for (uint32_t opaque = 0; opaque < 2; opaque++) {
        grid.opaque = opaque; texture->format = opaque ? 0x07 : 0x06;
        check("structured swizzled RGBA mask draw accepted", nv2a_gpu_draw(&state, vertices, 6)); nv2a_gpu_flush();
        uint32_t mismatches = 0;
        for (uint32_t y = 0; y < 8; y++)
            for (uint32_t x = 0; x < 8; x++) {
                uint32_t sx = x * 4 + 2, sy = y * 2 + 1;
                uint32_t expected = (opaque ? 0xFF000000u : 0x40000000u) |
                    (sx << 16) | (sy << 8) | ((sx ^ sy) * 4);
                if (color[y * 8 + x] != expected) mismatches++;
            }
        check("every structured mask texel preserves rows, channels, alpha and Q", mismatches == 0);
        printf("structured shadow mask format %X: %u mismatched pixels\n", texture->format, mismatches);
        nv2a_gpu_invalidate();
    }
}

static void test_gpu_resident(void)
{
    enum { depth_offset = 2 * 1024 * 1024 };
    uint32_t bytes = getenv("NV2A_GPU_RESIDENT_LARGE_MAPPING") ? 64 * 1024 * 1024 : 4 * 1024 * 1024;
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, bytes, NULL);
    check("resident backing section created", mapping != NULL);
    if (!mapping) _Exit(EXIT_FAILURE);
    uint8_t *primary = (uint8_t *)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
    uint8_t *alias = (uint8_t *)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
    uint8_t *short_alias = (uint8_t *)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 4096);
    HANDLE other_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 65536, NULL);
    uint8_t *other = other_mapping ? (uint8_t *)MapViewOfFile(other_mapping, FILE_MAP_ALL_ACCESS, 0, 0, 65536) : NULL;
    check("resident CPU aliases and second section mapped", primary && alias && short_alias && other);
    if (!primary || !alias || !short_alias || !other) _Exit(EXIT_FAILURE);
    nv2a_gpu_memory_set_thread();
    int resident = nv2a_gpu_memory_initialize();
    if (resident) {
        Nv2aGpuMemoryView views[3] = {{primary, bytes}, {alias, bytes}, {short_alias, 4096}};
        Nv2aGpuMemoryView other_view = {other, 65536};
        check("all CPU aliases registered", nv2a_gpu_memory_add_mapping(mapping, bytes, views, 3));
        check("second backing section registered", nv2a_gpu_memory_add_mapping(other_mapping, 65536, &other_view, 1));
        test_gpu_resident_eligibility(primary, alias);
    }
    if (!getenv("NV2A_GPU_RESIDENT_BENCH_ONLY")) {
        test_gpu_resident_targets(primary, alias, short_alias, other, resident);
        test_gpu_projected_target(primary, alias, other, resident);
    }
    _putenv_s("NV2A_GPU_BENCH_SEMAPHORE", "1");
    gpu_publication_benchmark_buffers(primary, primary + depth_offset);
    Nv2aGpuDraw shutdown_state = {0};
    shutdown_state.color = primary; shutdown_state.width = shutdown_state.height = 8;
    shutdown_state.clip_width = shutdown_state.clip_height = 8;
    shutdown_state.pitch = 32; shutdown_state.bytes_per_pixel = 4; shutdown_state.color_mask = 0x01010101;
    check("clear before cross-thread shutdown", nv2a_gpu_clear(&shutdown_state, 0xF0, 0xFF123456, 0));
    nv2a_gpu_flush_reason(NV2A_GPU_SYNC_SEMAPHORE);
    if (resident)
        check("cache eligibility before tracker shutdown", nv2a_gpu_memory_can_reside(primary, 256));
    HANDLE shutdown = CreateThread(NULL, 0, gpu_resident_shutdown_thread, NULL, 0, NULL);
    check("cross-thread tracker shutdown created", shutdown != NULL);
    if (!shutdown) _Exit(EXIT_FAILURE);
    if (resident) {
        DWORD start = GetTickCount();
        while (!nv2a_gpu_memory_pending() && GetTickCount() - start < 2000) Sleep(0);
        check("shutdown requests owner-thread publication", nv2a_gpu_memory_pending());
        check("shutdown disables new residency before publication",
              !nv2a_gpu_memory_can_reside(primary, 256));
    }
    gpu_resident_complete_access(shutdown, resident);
    check("tracker disabled after cross-thread shutdown", !nv2a_gpu_memory_active());
    check("cross-thread shutdown publishes exact outstanding color", *(uint32_t *)(alias + 36) == 0xFF123456);
    check("clear after shutdown remains eager", nv2a_gpu_clear(&shutdown_state, 0xF0, 0xFF654321, 0));
    nv2a_gpu_flush_reason(NV2A_GPU_SYNC_SEMAPHORE);
    check("post-shutdown semaphore publishes exact output", *(uint32_t *)(short_alias + 36) == 0xFF654321);
    nv2a_gpu_invalidate();
    if (resident) {
        Nv2aGpuMemoryView rebound[2] = {{primary, bytes}, {alias, bytes}};
        Nv2aGpuMemoryView other_view = {other, 65536};
        DWORD previous;
        check("tracker reinitializes with retained lookup caches", nv2a_gpu_memory_initialize());
        check("rebound second section registers first", nv2a_gpu_memory_add_mapping(other_mapping, 65536, &other_view, 1));
        check("rebound main section registers fewer views", nv2a_gpu_memory_add_mapping(mapping, bytes, rebound, 2));
        check("cached lookup follows current mapping and view indices",
              nv2a_gpu_memory_canonical(alias + 2048, 4) == primary + 2048);
        check("removed truncated alias is not resolved from stale indices",
              nv2a_gpu_memory_canonical(short_alias, 4) == NULL);
        check("rebound fixture changes primary permission",
              nv2a_gpu_memory_virtual_protect(primary, 4096, PAGE_READONLY, &previous));
        check("rebound fixture guards saved permissions", nv2a_gpu_memory_protect(primary, 4096));
        check("cached eligibility cannot cross tracker lifetimes", !nv2a_gpu_memory_can_reside(primary, 256));
        check("rebound writable alias remains eligible", nv2a_gpu_memory_can_reside(alias, 256));
        nv2a_gpu_memory_release();
        check("rebound fixture restores primary permission",
              nv2a_gpu_memory_virtual_protect(primary, 4096, PAGE_READWRITE, &previous));
        nv2a_gpu_memory_shutdown();
    }
    check("resident first alias unmapped", UnmapViewOfFile(primary));
    check("resident second alias unmapped", UnmapViewOfFile(alias));
    check("resident truncated alias unmapped", UnmapViewOfFile(short_alias));
    check("resident second section unmapped", UnmapViewOfFile(other));
    CloseHandle(other_mapping);
    CloseHandle(mapping);
}

static int gpu_decode_cube(void *context, uint32_t face, uint32_t horizontal, uint32_t vertical, uint32_t *color)
{
    (void)horizontal; (void)vertical;
    *color = ((const uint32_t *)context)[face * 32];
    return 1;
}

static int gpu_decode_grid(void *context, uint32_t horizontal, uint32_t vertical, uint32_t *color)
{
    *color = ((const uint32_t *)context)[vertical * 4 + horizontal];
    return 1;
}

static void test_gpu_cube(void)
{
    static const float directions[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    static const uint32_t colors[6] = {0xFFFF0000,0xFF00FF00,0xFF0000FF,0xFFFFFF00,0xFF00FFFF,0xFFFF00FF};
    uint32_t color[64] = {0}, cube[192] = {0}, face, vertex;
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    state.color = (uint8_t *)color; state.width = state.height = state.clip_width = state.clip_height = 8;
    state.pitch = 32; state.bytes_per_pixel = 4; state.color_mask = 0x01010101;
    state.stage_program = 3; state.combiners.control = 1;
    state.combiners.rgb_input[0] = 0x08200000; state.combiners.alpha_input[0] = 0x00002018;
    state.combiners.rgb_output[0] = state.combiners.alpha_output[0] = 0xC00;
    state.combiners.final_input[0] = 0xC; state.combiners.final_input[1] = 0x1C80;
    state.textures[0].source = (const uint8_t *)cube; state.textures[0].source_bytes = sizeof cube;
    state.textures[0].width = state.textures[0].height = 1; state.textures[0].format = 6;
    state.textures[0].cube = 1; state.textures[0].face_stride = 128;
    state.textures[0].decode_context = cube; state.textures[0].decode_face = gpu_decode_cube;
    for (face = 0; face < 6; face++) cube[face * 32] = colors[face];
    for (vertex = 0; vertex < 3; vertex++) vertices[vertex].position[3] = 1;
    vertices[1].position[0] = 8; vertices[2].position[1] = 8;
    for (face = 0; face < 6; face++) {
        for (vertex = 0; vertex < 3; vertex++) memcpy(vertices[vertex].texture[0], directions[face], sizeof directions[face]);
        check("native cube face accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("native cube face orientation and color", color[9] == colors[face]);
    }
    cube[5 * 32] = 0xFF804020;
    check("native mutated cube accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("native cube cache observes source mutation", color[9] == 0xFF804020);
    nv2a_gpu_invalidate();
    {
        const float projective_coordinates[5][4] = {{1.5f,0,0,2},{0,4,0,2},{0,-4,0,2},{-4,0,0,2},{4,0,0,2}};
        const uint32_t expected_faces[5] = {0,2,3,4,5};
        state.stage_program = 1;
        for (face = 0; face < 5; face++) {
            for (vertex = 0; vertex < 3; vertex++) memcpy(vertices[vertex].texture[0], projective_coordinates[face], sizeof projective_coordinates[face]);
            check("projective coordinates on cube accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
            check("projective cube coordinates divide by Q", color[9] == cube[expected_faces[face] * 32]);
        }
        state.stage_program = 3;
        nv2a_gpu_invalidate();
    }
    {
        const float projected_directions[6][3] = {{2,0.5f,-0.5f},{-2,0.5f,1},{0.5f,2,-1},
                                                  {1,-2,1},{0.5f,0.5f,2},{-1,0.5f,-2}};
        const uint32_t expected_texels[6] = {5,6,9,10,5,6};
        for (face = 0; face < 16; face++) cube[face] = 0xFF000000u | ((face + 1) * 0x00090703u);
        state.textures[0].cube = 0; state.textures[0].face_stride = 0;
        state.textures[0].width = state.textures[0].height = 4; state.textures[0].pitch = 16;
        state.textures[0].source_bytes = 64; state.textures[0].decode = gpu_decode_grid;
        for (face = 0; face < 6; face++) {
            for (vertex = 0; vertex < 3; vertex++) memcpy(vertices[vertex].texture[0], projected_directions[face], sizeof projected_directions[face]);
            check("cube coordinates on 2D texture accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
            check("cube coordinates project to correct 2D texel", color[9] == cube[expected_texels[face]]);
        }
        cube[6] = 0x00804020; state.textures[0].format = 0x1E;
        check("projected XRGB texture accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("projected XRGB texture forces opaque alpha", color[9] == 0xFF804020);
        nv2a_gpu_invalidate();
        state.stage_program = 1; state.textures[0].format = 6;
        for (vertex = 0; vertex < 3; vertex++) {
            vertices[vertex].texture[0][0] = 1.375f; vertices[vertex].texture[0][1] = -0.375f;
            vertices[vertex].texture[0][3] = 1;
        }
        for (uint32_t repetition = 0; repetition < 2; repetition++) {
            for (uint32_t address_u = 1; address_u <= 3; address_u++) {
                for (uint32_t address_v = 1; address_v <= 3; address_v++) {
                    const uint32_t columns[3] = {1,2,3}, rows[3] = {2,1,0};
                    state.textures[0].address_u = address_u; state.textures[0].address_v = address_v;
                    check("cached sampler transition accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
                    check("cached sampler preserves wrap mirror clamp", color[9] == cube[rows[address_v - 1] * 4 + columns[address_u - 1]]);
                }
            }
        }
        nv2a_gpu_invalidate();
    }
}

static void test_gpu_dxt(void)
{
    const uint32_t formats[3] = {0xC,0xE,0xF};
    uint32_t color[64] = {0}, decoded = 0xFFFF00FF, format, vertex;
    uint8_t blocks[768] = {0};
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    state.color = (uint8_t *)color; state.width = state.height = state.clip_width = state.clip_height = 8;
    state.pitch = 32; state.bytes_per_pixel = 4; state.color_mask = 0x01010101;
    state.stage_program = 1; state.combiners.control = 1;
    state.combiners.rgb_input[0] = 0x08200000; state.combiners.alpha_input[0] = 0x00002018;
    state.combiners.rgb_output[0] = state.combiners.alpha_output[0] = 0xC00;
    state.combiners.final_input[0] = 0xC; state.combiners.final_input[1] = 0x1C80;
    state.textures[0].source = blocks; state.textures[0].width = state.textures[0].height = 4;
    state.textures[0].decode = gpu_decode; state.textures[0].decode_context = &decoded;
    for (vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].position[3] = vertices[vertex].texture[0][3] = 1;
        vertices[vertex].texture[0][0] = vertices[vertex].texture[0][1] = 0.5f;
    }
    vertices[1].position[0] = 8; vertices[2].position[1] = 8;
    for (format = 0; format < 3; format++) {
        uint32_t offset = format ? 8 : 0;
        memset(blocks, 0, sizeof blocks);
        if (format == 1) memset(blocks, 255, 8);
        if (format == 2) blocks[0] = blocks[1] = 255;
        blocks[offset + 1] = 0xF8;
        state.textures[0].format = formats[format]; state.textures[0].source_bytes = format ? 16 : 8;
        check("native DXT texture accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("native DXT samples raw blocks without CPU expansion", color[9] == 0xFFFF0000);
        blocks[offset] = 0x1F; blocks[offset + 1] = 0;
        check("native mutated DXT accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("native DXT source mutation updates pixels", color[9] == 0xFF0000FF);
    }
    memset(blocks, 0, sizeof blocks);
    for (format = 0; format < 6; format++) blocks[format * 128 + 1] = 0xF8;
    state.stage_program = 3; state.textures[0].format = 0xC;
    state.textures[0].source_bytes = sizeof blocks; state.textures[0].cube = 1; state.textures[0].face_stride = 128;
    state.textures[0].decode_face = gpu_decode_cube;
    state.textures[0].decode_context = blocks;
    for (vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].texture[0][0] = vertices[vertex].texture[0][1] = 0;
        vertices[vertex].texture[0][2] = -1;
    }
    check("native DXT cube accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("native DXT cube samples aligned raw face", color[9] == 0xFFFF0000);
    blocks[5 * 128] = 0x1F; blocks[5 * 128 + 1] = 0;
    check("native mutated DXT cube accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("native DXT cube observes changed face", color[9] == 0xFF0000FF);
    nv2a_gpu_invalidate();
}

static void gpu_texture_cache_setup(Nv2aGpuDraw *state, Nv2aGpuVertex *vertices, uint32_t *color)
{
    uint32_t vertex;
    state->color = (uint8_t *)color; state->width = state->height = state->clip_width = state->clip_height = 8;
    state->pitch = 32; state->bytes_per_pixel = 4; state->color_mask = 0x01010101;
    state->stage_program = 1; state->combiners.control = 1;
    state->combiners.rgb_input[0] = 0x08200000; state->combiners.alpha_input[0] = 0x00002018;
    state->combiners.rgb_output[0] = state->combiners.alpha_output[0] = 0xC00;
    state->combiners.final_input[0] = 0xC; state->combiners.final_input[1] = 0x1C80;
    state->textures[0].width = state->textures[0].height = 1;
    state->textures[0].pitch = 4; state->textures[0].format = 6; state->textures[0].source_bytes = 4;
    state->textures[0].decode = gpu_decode;
    vertices[1].position[0] = 8; vertices[2].position[1] = 8;
    for (vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].position[3] = vertices[vertex].texture[0][3] = 1;
        vertices[vertex].texture[0][0] = vertices[vertex].texture[0][1] = 0.5f;
    }
}

static int load_ghost_pixel_shader(const char *directory, const char *name, Nv2aGpuDraw *state, uint32_t mapping[3])
{
    char path[1024];
    uint32_t words[61];
    int length = snprintf(path, sizeof path, "%s\\%s", directory, name);
    if (length < 0 || length >= (int)sizeof path) {
        fprintf(stderr, "Ghost shader fixture path is too long\n");
        check("Ghost shader fixture path", 0); return 0;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror(path); check("open Xbox shader fixture", 0); return 0;
    }
    int complete = fread(words, sizeof words, 1, file) == 1 && fgetc(file) == EOF && !ferror(file);
    if (fclose(file)) complete = 0;
    if (!complete || words[0] != 0x30425350 || (words[54] & 255u) > 8) {
        fprintf(stderr, "Invalid Xbox PSB0 fixture: %s\n", path);
        check("Xbox pixel shader fixture layout", 0); return 0;
    }
    memcpy(state->combiners.alpha_input, words + 1, 32);
    memcpy(state->combiners.final_input, words + 9, 8);
    memcpy(state->combiners.alpha_output, words + 27, 32);
    memcpy(state->combiners.rgb_input, words + 35, 32);
    memcpy(state->combiners.rgb_output, words + 46, 32);
    state->combiners.control = words[54]; state->stage_program = words[55];
    state->shader_clip_mode = words[43]; state->shader_dot_mapping = words[56];
    state->shader_other_stage_input = words[57];
    memcpy(mapping, words + 58, 12);
    return 1;
}

static void ghost_shader_constants(Nv2aCpuCombiners *state, const uint32_t mapping[3],
                                   const float constants[16][4])
{
    for (uint32_t stage = 0; stage < 8; stage++)
        for (uint32_t factor = 0; factor < 2; factor++)
            memcpy(state->factors[stage][factor], constants[(mapping[factor] >> (stage * 4)) & 15u],
                   sizeof state->factors[stage][factor]);
    for (uint32_t factor = 0; factor < 2; factor++)
        memcpy(state->factors[8][factor], constants[(mapping[2] >> (factor * 4)) & 15u],
               sizeof state->factors[8][factor]);
}

static int ghost_image_decode(void *context, uint32_t x, uint32_t y, uint32_t *color)
{
    if (x >= 8 || y >= 8) {
        fprintf(stderr, "Ghost fixture image decode outside 8x8 image: %u,%u\n", x, y);
        return 0;
    }
    *color = ((uint32_t *)context)[y * 8 + x];
    return 1;
}

static void ghost_command_draw(const Nv2aGpuDraw *state, const Nv2aGpuVertex vertices[3], uint32_t instructions)
{
    const uint32_t inputs[] = {0, 1, 2, 3, 5, 6};
    float (*arrays)[6][4] = (float (*)[6][4])((uint8_t *)memory + 0xC000);
    method(0x0200, 8u << 16); method(0x0204, 8u << 16);
    method(0x0208, 0x28); method(0x020C, 32 | (32u << 16));
    method(0x0210, 0x1000); method(0x0214, 0x2000);
    method(0x0290, 0); method(0x0300, 0); method(0x0304, 0); method(0x0308, 0);
    method(0x030C, 0); method(0x032C, 0); method(0x0358, 0x01010101);
    method(0x1D98, 7u << 16); method(0x1D9C, 7u << 16);
    method(0x1D90, 0x01020304); method(0x1D94, 0xF0);
    method(0x1E9C, 0);
    for (uint32_t instruction = 0; instruction < instructions; instruction++)
        for (uint32_t word = 0; word < 4; word++) method(0x0B00 + word * 4, state->vertex_program[instruction][word]);
    method(0x1EA0, 0); method(0x1E94, 6); method(0x1EA4, 0);
    for (uint32_t constant = 0; constant < 192; constant++)
        for (uint32_t channel = 0; channel < 4; channel++) float_method(0x0B80 + channel * 4, state->vertex_constants[constant][channel]);
    for (uint32_t input = 0; input < 16; input++) method(0x1760 + input * 4, 0);
    for (uint32_t input = 0; input < 6; input++) {
        method(0x1720 + inputs[input] * 4, 0xC000 + input * 16);
        method(0x1760 + inputs[input] * 4, (sizeof arrays[0] << 8) | 0x42);
        for (uint32_t vertex = 0; vertex < 3; vertex++)
            memcpy(arrays[vertex][input], vertices[vertex].attributes[inputs[input]], sizeof arrays[vertex][input]);
    }
    method(0x1E60, state->combiners.control); method(0x1E70, state->stage_program);
    method(0x0288, state->combiners.final_input[0]); method(0x028C, state->combiners.final_input[1]);
    for (uint32_t stage = 0; stage < 8; stage++) {
        method(0x0260 + stage * 4, state->combiners.alpha_input[stage]);
        method(0x0AA0 + stage * 4, state->combiners.alpha_output[stage]);
        method(0x0AC0 + stage * 4, state->combiners.rgb_input[stage]);
        method(0x1E40 + stage * 4, state->combiners.rgb_output[stage]);
        for (uint32_t factor = 0; factor < 2; factor++)
            method(0x0A60 + factor * 32 + stage * 4, nv_cpu_pack_argb(state->combiners.factors[stage][factor]));
    }
    for (uint32_t stage = 0; stage < 4; stage++) {
        uint32_t base = 0x1B00 + stage * 64, source = 0xD000 + stage * 256;
        memcpy((uint8_t *)memory + source, state->textures[stage].source, 256);
        method(base, source); method(base + 4, 0x12u << 8); method(base + 8, 0x303);
        method(base + 12, 1u << 30); method(base + 16, 32u << 16);
        method(base + 20, 0); method(base + 28, 0x00080008);
    }
    draw(5, 3);
}

static void test_ghost_vertex_shaders(const char *directory)
{
    const char *names[] = {"cloakRigid0.xvu", "cloakRigid3.xvu", "cloakSkinned3.xvu", "cloakSkinnedSoft3.xvu"};
    for (uint32_t variant = 0; variant < sizeof names / sizeof names[0]; variant++) {
        uint32_t program[136][4] = {{0}}, valid[136] = {0}, mapping[3], header;
        uint32_t images[4][64], reference[64], actual[64];
        float constants[192][4];
        Nv2aGpuDraw state = {0};
        Nv2aGpuVertex vertices[3] = {0}, translated[3] = {0};
        gpu_texture_cache_setup(&state, vertices, reference);
        if (!load_ghost_pixel_shader(directory, "cloak.xpu", &state, mapping)) continue;
        char path[1024];
        int length = snprintf(path, sizeof path, "%s\\%s", directory, names[variant]);
        if (length < 0 || length >= (int)sizeof path) {
            fprintf(stderr, "Ghost vertex fixture path is too long\n"); check("Ghost vertex fixture path", 0); continue;
        }
        FILE *file = fopen(path, "rb");
        if (!file) { perror(path); check("open Xbox vertex fixture", 0); continue; }
        int complete = fread(&header, sizeof header, 1, file) == 1;
        uint32_t instructions = complete ? header >> 16 : 0;
        complete = complete && (header & 65535u) == 0x2078 && instructions > 0 && instructions <= 136 &&
            fread(program, sizeof program[0], instructions, file) == instructions && fgetc(file) == EOF && !ferror(file);
        if (fclose(file)) complete = 0;
        if (!complete || !(program[instructions - 1][3] & 1u)) {
            fprintf(stderr, "Invalid Xbox vertex fixture: %s\n", path); check("Xbox vertex fixture layout", 0); continue;
        }
        for (uint32_t instruction = 0; instruction < instructions; instruction++) valid[instruction] = 15;
        for (uint32_t constant = 0; constant < 192; constant++)
            for (uint32_t channel = 0; channel < 4; channel++) constants[constant][channel] = channel == 3 ? 1 : 0.5f;
        for (uint32_t row = 0; row < 4; row++)
            for (uint32_t column = 0; column < 4; column++) constants[96 + row][column] = row == column ? 1 : 0;
        for (uint32_t row = 0; row < 60; row++)
            for (uint32_t column = 0; column < 4; column++) constants[132 + row][column] = row % 3 == column ? 1 : 0;
        constants[100][0] = 0;
        const float viewport_scale[4] = {4, -4, 16777215, 1}, viewport_offset[4] = {4, 4, 0, 0};
        memcpy(constants[58], viewport_scale, sizeof viewport_scale); memcpy(constants[59], viewport_offset, sizeof viewport_offset);
        const float positions[3][4] = {{-0.5f, 0.5f, 0.1f, 1}, {0.5f, 0.5f, 0.1f, 1}, {-0.5f, -0.5f, 0.1f, 1}};
        float shader_constants[16][4] = {{0}};
        nv_cpu_unpack_argb(0xC0408040, shader_constants[0]); nv_cpu_unpack_argb(0xFF000000, shader_constants[1]);
        ghost_shader_constants(&state.combiners, mapping, shader_constants);
        for (uint32_t image = 0; image < 4; image++) {
            for (uint32_t y = 0; y < 8; y++)
                for (uint32_t x = 0; x < 8; x++)
                    images[image][y * 8 + x] = 0xFF000000u | ((x * 17 + image * 7) << 16) |
                                              ((y * 19 + image * 11) << 8) | (x * 13 + y * 5 + image * 3);
            state.textures[image] = state.textures[0];
            state.textures[image].source = (const uint8_t *)images[image]; state.textures[image].source_bytes = sizeof images[image];
            state.textures[image].width = state.textures[image].height = 8; state.textures[image].pitch = 32;
            state.textures[image].linear = 1; state.textures[image].format = 0x12;
            state.textures[image].address_u = state.textures[image].address_v = 3;
            state.textures[image].decode = ghost_image_decode; state.textures[image].decode_context = images[image];
        }
        int decoded = 1;
        for (uint32_t vertex = 0; vertex < 3; vertex++) {
            memcpy(vertices[vertex].attributes[0], positions[vertex], sizeof positions[vertex]);
            vertices[vertex].attributes[1][2] = 1;
            vertices[vertex].attributes[6][0] = 1;
            for (uint32_t channel = 0; channel < 4; channel++) vertices[vertex].attributes[2][channel] = 1;
            vertices[vertex].attributes[3][0] = vertex == 1 ? 6 : 1;
            vertices[vertex].attributes[3][1] = vertex == 2 ? 6 : 1;
            vertices[vertex].attributes[3][3] = 1;
            Nv2aCpuVertex result;
            float working_constants[192][4];
            memcpy(working_constants, constants, sizeof constants);
            if (!nv_cpu_vertex_execute(program, valid, 0, vertices[vertex].attributes, working_constants, &result)) {
                fprintf(stderr, "Ghost vertex fixture failed: %s vertex %u slot %u constant %d\n",
                        names[variant], vertex, result.failed_slot, result.failed_constant);
                check("Xbox vertex fixture executes on CPU", 0); decoded = 0; break;
            }
            memcpy(translated[vertex].position, result.output[0], sizeof translated[vertex].position);
            for (uint32_t channel = 0; channel < 4; channel++) {
                translated[vertex].diffuse[channel] = nv_cpu_clamp(result.output[3][channel], 0, 1);
                translated[vertex].specular[channel] = nv_cpu_clamp(result.output[4][channel], 0, 1);
            }
            for (uint32_t image = 0; image < 4; image++)
                memcpy(translated[vertex].texture[image], result.output[9 + image], sizeof translated[vertex].texture[image]);
        }
        if (!decoded) continue;
        for (uint32_t pixel = 0; pixel < 64; pixel++) reference[pixel] = actual[pixel] = 0x01020304;
        check("CPU-translated cloak vertex reference draw", nv2a_gpu_draw(&state, translated, 3) == 1);
        nv2a_gpu_flush();
        state.color = (uint8_t *)actual;
        state.vertex_program = program; state.vertex_valid = valid; state.vertex_constants = constants;
        const uint32_t indices[3] = {1, 2, 0};
        state.indices = indices; state.index_count = 3;
        check("compiled Xbox cloak vertex program draw", nv2a_gpu_draw(&state, vertices, 3) == 1);
        nv2a_gpu_flush();
        uint32_t covered = 0;
        for (uint32_t pixel = 0; pixel < 64; pixel++) {
            if (reference[pixel] != 0x01020304) covered++;
            for (uint32_t channel = 0; channel < 4; channel++) {
                int reference_byte = (reference[pixel] >> (channel * 8)) & 255u, actual_byte = (actual[pixel] >> (channel * 8)) & 255u;
                if (abs(reference_byte - actual_byte) > 1) {
                    fprintf(stderr, "Ghost vertex mismatch: %s pixel %u actual %08X reference %08X\n",
                            names[variant], pixel, actual[pixel], reference[pixel]);
                    check("CPU/GPU cloak vertex positions and gradient texture coordinates agree", 0);
                }
            }
        }
        ghost_command_draw(&state, vertices, instructions);
        for (uint32_t pixel = 0; pixel < 64; pixel++)
            for (uint32_t channel = 0; channel < 4; channel++) {
                int reference_byte = (reference[pixel] >> (channel * 8)) & 255u;
                int command_byte = (memory[0x1000 / 4 + pixel] >> (channel * 8)) & 255u;
                if (abs(reference_byte - command_byte) > 1) {
                    fprintf(stderr, "Ghost command vertex mismatch: %s pixel %u actual %08X reference %08X\n",
                            names[variant], pixel, memory[0x1000 / 4 + pixel], reference[pixel]);
                    check("command translation preserves compiled cloak inputs and output", 0);
                }
            }
        if (covered != 28)
            for (uint32_t vertex = 0; vertex < 3; vertex++)
                fprintf(stderr, "Ghost vertex fixture coverage: %s vertex %u position %.6g,%.6g,%.6g,%.6g\n",
                        names[variant], vertex, translated[vertex].position[0], translated[vertex].position[1],
                        translated[vertex].position[2], translated[vertex].position[3]);
        check("compiled cloak vertex fixture covers exactly 28 pixels", covered == 28);
        printf("Ghost compiled vertex %s: %u covered pixels compared through CPU/GPU/commands\n", names[variant], covered);
        nv2a_gpu_invalidate();
    }
}

static int test_ghost_pixel_shaders(const char *directory)
{
    const char *names[3] = {"cloak.xpu", "cloakdissolve.xpu", "cloakdissolveshadow.xpu"};
    const uint32_t stages[3] = {6, 5, 3};
    check("Ghost effect shader D3D11 device", nv2a_gpu_available());
    check("Ghost effect shader compilation", nv2a_gpu_compile());
    test_ghost_vertex_shaders(directory);
    for (uint32_t effect = 0; effect < 3; effect++) {
        uint32_t output[64], texels[4], mapping[3];
        Nv2aGpuDraw state = {0};
        Nv2aGpuVertex vertices[3] = {0};
        gpu_texture_cache_setup(&state, vertices, output);
        if (!load_ghost_pixel_shader(directory, names[effect], &state, mapping)) continue;
        check("Xbox effect uses expected number of combiner stages", (state.combiners.control & 255u) == stages[effect]);
        check("Xbox effect uses independent projective texture reads", state.stage_program == (effect == 2 ? 1u : 0x8421u));
        uint32_t original_final[2];
        uint32_t original_control = state.combiners.control;
        memcpy(original_final, state.combiners.final_input, sizeof original_final);
        for (uint32_t texture = 0; texture < 4; texture++) {
            state.textures[texture] = state.textures[0];
            state.textures[texture].source = (const uint8_t *)&texels[texture];
            state.textures[texture].decode_context = &texels[texture];
            state.textures[texture].format = 0x12; state.textures[texture].linear = 1;
            for (uint32_t vertex = 0; vertex < 3; vertex++)
                memcpy(vertices[vertex].texture[texture], vertices[vertex].texture[0], sizeof vertices[vertex].texture[texture]);
        }
        for (uint32_t test = 0; test < 48; test++) {
            state.combiners.control = original_control;
            memcpy(state.combiners.final_input, original_final, sizeof original_final);
            if (effect != 1 && (test & 1u)) {
                state.combiners.final_input[0] = 0xC;
                state.combiners.final_input[1] = 0x1C80;
            }
            float textures[4][4], constants[16][4] = {{0}}, diffuse[4], specular[4] = {0};
            float expected[4], cpu[4], background[3], base[3];
            uint32_t constant_colors[4] = {0x80104090, 0xA0306080, 0x90702040, 0x604080C0};
            constant_colors[0] = (constant_colors[0] & 0x00FFFFFFu) | ((test * 37u % 256u) << 24);
            constant_colors[1] = (constant_colors[1] & 0x00FFFFFFu) | ((test & 1u ? 255u : 0u) << 24);
            if (effect == 2)
                constant_colors[1] = (constant_colors[1] & 0x00FFFFFFu) | ((test * 13u % 256u) << 24);
            for (uint32_t constant = 0; constant < 4; constant++)
                nv_cpu_unpack_argb(constant_colors[constant], constants[constant]);
            if (test == 47) {
                for (uint32_t constant = 1; constant < 16; constant++)
                    memcpy(constants[constant], constants[0], sizeof constants[constant]);
                state.combiners.control &= ~0x11000u;
            }
            texels[0] = 0xFF183050; texels[1] = 0x80603020;
            texels[2] = ((test * 29u % 256u) << 24) | (test * 17u % 256u);
            texels[3] = 0xC0806040;
            if (effect == 2) {
                uint32_t blue = test * 17u % 256u;
                if (blue == (constant_colors[1] >> 24)) blue = (blue + 1) & 255u;
                texels[0] = (texels[0] & 0xFFFFFF00u) | blue;
            }
            nv_cpu_unpack_argb(0xA080C040, diffuse);
            for (uint32_t vertex = 0; vertex < 3; vertex++) memcpy(vertices[vertex].diffuse, diffuse, sizeof diffuse);
            for (uint32_t texture = 0; texture < 4; texture++) nv_cpu_unpack_argb(texels[texture], textures[texture]);
            ghost_shader_constants(&state.combiners, mapping, constants);
            for (uint32_t channel = 0; channel < 3; channel++) {
                background[channel] = nv_cpu_clamp(textures[1][channel] * textures[1][3] + textures[0][channel], 0, 1);
                base[channel] = textures[3][channel] * (constants[1][3] >= 0.5f ? diffuse[channel] : 1);
                if (effect == 0) {
                    float faded = constants[0][channel] * base[channel] + (1 - constants[0][channel]) * background[channel];
                    expected[channel] = textures[2][3] >= 0.5f ? faded : background[channel];
                } else if (effect == 1) {
                    float selected = textures[2][2] < constants[2][3] ? background[channel] : base[channel];
                    expected[channel] = constants[3][3] * constants[3][channel] + (1 - constants[3][3]) * selected;
                } else expected[channel] = (textures[0][2] >= constants[1][3] ? constants[1][channel] : constants[0][channel]) * constants[0][3];
            }
            expected[3] = effect == 2 ?
                nv_cpu_clamp(nv_cpu_clamp(textures[0][2] - constants[1][3] + 0.5f, -1, 1) * constants[0][3], 0, 1) :
                constants[0][3];
            check("compiled Xbox effect accepted by CPU combiner reference",
                  nv_cpu_combiners_execute(&state.combiners, diffuse, specular, textures, 0, cpu));
            for (uint32_t channel = 0; channel < 4; channel++)
                check("compiled Xbox effect matches independent effect formula",
                      fabsf(nv_cpu_clamp(cpu[channel], 0, 1) - expected[channel]) < 0.00001f);
            memset(output, 0x55, sizeof output);
            check("compiled Xbox effect accepted by D3D11 translator", nv2a_gpu_draw(&state, vertices, 3) == 1);
            nv2a_gpu_flush();
            uint32_t wanted = nv_cpu_pack_argb(expected);
            for (uint32_t channel = 0; channel < 4; channel++) {
                int actual_byte = (output[9] >> (channel * 8)) & 255u;
                int wanted_byte = (wanted >> (channel * 8)) & 255u;
                if (abs(actual_byte - wanted_byte) > 1) {
                    fprintf(stderr, "Ghost effect mismatch: %s case %u actual %08X expected %08X channel %u\n",
                            names[effect], test, output[9], wanted, channel);
                    check("compiled Xbox effect hardware output", 0);
                }
            }
        }
        printf("Ghost compiled effect %s: 48 formula/CPU/D3D11 cases checked\n", names[effect]);
        nv2a_gpu_invalidate();
    }
    nv2a_pb_exec_report();
    printf("nv2a_ghost_effects: %s\n", failures ? "FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}

static void test_gpu_four_targets_layout(int mixed_sizes)
{
    uint32_t targets[4][256], depth[64], output[64] = {0};
    const uint32_t colors[4] = {0xFF100000, 0xFF002000, 0xFF000030, 0xFF010203};
    const uint32_t channel_shifts[4] = {16, 8, 0, 24};
    Nv2aGpuDraw pass = {0}, sampling = {0};
    Nv2aGpuVertex vertices[6] = {0}, sample_vertices[3] = {0};
    const float positions[6][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1}};
    pass.width = pass.height = pass.clip_width = pass.clip_height = 8;
    pass.bytes_per_pixel = 4; pass.color_mask = 0x01010101;
    pass.depth = (uint8_t *)depth; pass.depth_pitch = 32; pass.depth_format = 2;
    pass.depth_enable = pass.depth_mask = !mixed_sizes; pass.depth_function = 0x207;
    pass.stencil_enable = !mixed_sizes; pass.stencil_function = 0x207;
    pass.stencil_read_mask = pass.stencil_write_mask = 255;
    pass.stencil_fail = pass.stencil_depth_fail = 0x1E00; pass.stencil_pass = 0x1E01;
    for (uint32_t vertex = 0; vertex < 6; vertex++) {
        vertices[vertex].position[3] = 1;
    }
    memset(targets, 0x55, sizeof targets); memset(depth, 0x66, sizeof depth);
    for (uint32_t target = 0; target < 4; target++) {
        pass.color = (uint8_t *)targets[target];
        pass.width = pass.clip_width = mixed_sizes && target == 0 ? 16 : 8;
        pass.height = pass.clip_height = mixed_sizes && target == 0 ? 12 : 8;
        pass.pitch = mixed_sizes && target == 0 ? 64 : 32 + target * 8;
        pass.stencil_reference = 0x10 + target;
        check("clear separately retained render target",
              nv2a_gpu_clear(&pass, mixed_sizes ? 0xF0 : 0xF3, 0xFF000000, 0xFFFFFF5A) == 1);
        for (uint32_t vertex = 0; vertex < 6; vertex++) {
            vertices[vertex].position[0] = positions[vertex][0] * pass.width;
            vertices[vertex].position[1] = positions[vertex][1] * pass.height;
            vertices[vertex].position[2] = (float)(100 + target);
            for (uint32_t channel = 0; channel < 4; channel++)
                vertices[vertex].diffuse[channel] = (float)((colors[target] >> channel_shifts[channel]) & 255) / 255.0f;
        }
        check("render pass switches between four independent targets", nv2a_gpu_draw(&pass, vertices, 6) == 1);
    }
    gpu_texture_cache_setup(&sampling, sample_vertices, output);
    sampling.stage_program = 1 | (1u << 5) | (1u << 10) | (1u << 15);
    sampling.combiners.control = 3;
    sampling.combiners.rgb_input[0] = 0x08200920; sampling.combiners.rgb_output[0] = 0xC00;
    sampling.combiners.rgb_input[1] = 0x0A200B20; sampling.combiners.rgb_output[1] = 0xD00;
    sampling.combiners.rgb_input[2] = 0x0C200D20; sampling.combiners.rgb_output[2] = 0xC00;
    for (uint32_t stage = 0; stage < 3; stage++) {
        sampling.combiners.alpha_input[stage] = 0x00002014;
        sampling.combiners.alpha_output[stage] = 0xC00;
    }
    for (uint32_t stage = 0; stage < 4; stage++) {
        sampling.textures[stage] = sampling.textures[0];
        sampling.textures[stage].source = (const uint8_t *)targets[stage];
        sampling.textures[stage].width = mixed_sizes && stage == 0 ? 16 : 8;
        sampling.textures[stage].height = mixed_sizes && stage == 0 ? 12 : 8;
        sampling.textures[stage].pitch = mixed_sizes && stage == 0 ? 64 : 32 + stage * 8;
        sampling.textures[stage].source_bytes = sampling.textures[stage].pitch * sampling.textures[stage].height;
        sampling.textures[stage].linear = 1; sampling.textures[stage].format = stage & 1 ? 0x1E : 0x12;
        sampling.textures[stage].decode_context = targets[stage];
        for (uint32_t vertex = 0; vertex < 3; vertex++) {
            memcpy(sample_vertices[vertex].texture[stage], sample_vertices[vertex].texture[0],
                   sizeof sample_vertices[vertex].texture[stage]);
            sample_vertices[vertex].diffuse[3] = 1;
        }
    }
    uint32_t decode_before = gpu_decode_calls;
    check("four-stage composite samples four queued render targets", nv2a_gpu_draw(&sampling, sample_vertices, 3) == 1);
    nv2a_gpu_flush();
    check("four-stage composite includes each independent target", output[9] == 0xFF112233);
    check("four native render targets sample without CPU texture decoding", gpu_decode_calls == decode_before);
    if (!mixed_sizes)
        check("shared depth/stencil survives four target switches", depth[9] == (103u << 8 | 0x13));
    for (uint32_t target = 0; target < 4; target++) {
        uint32_t width = mixed_sizes && target == 0 ? 16 : 8;
        uint32_t height = mixed_sizes && target == 0 ? 12 : 8;
        uint32_t stride = mixed_sizes && target == 0 ? 16 : (32 + target * 8) / 4;
        for (uint32_t row = 0; row < height; row++) {
            for (uint32_t column = 0; column < width; column++)
                check("render targets preserve independent pixels", targets[target][row * stride + column] == colors[target]);
            for (uint32_t column = width; column < stride; column++)
                check("render targets preserve independent pitch padding", targets[target][row * stride + column] == 0x55555555);
        }
        check("render targets preserve allocation exterior", targets[target][stride * height] == 0x55555555);
    }
    pass.color = (uint8_t *)targets[2]; pass.pitch = 48;
    check("rebind and update only the third target", nv2a_gpu_clear(&pass, 0xF0, 0xFF000080, 0) == 1);
    check("composite refreshes updated target only", nv2a_gpu_draw(&sampling, sample_vertices, 3) == 1);
    nv2a_gpu_flush();
    check("updated four-stage composite preserves other three images", output[9] == 0xFF112283);
    check("unrelated targets survive rebinding",
          targets[0][9] == colors[0] && targets[1][11] == colors[1] && targets[3][15] == colors[3]);
    nv2a_gpu_report();
    nv2a_gpu_invalidate();
}

static void test_gpu_four_targets(void)
{
    test_gpu_four_targets_layout(0);
    test_gpu_four_targets_layout(1);
}

static void test_gpu_texture_cache(void)
{
    enum { entries = 1100 };
    static uint32_t texels[entries + 1], color[64];
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    uint32_t index, before, stage, vertex;
    gpu_texture_cache_setup(&state, vertices, color);
    for (stage = 1; stage < 4; stage++) {
        state.stage_program |= 1u << (stage * 5);
        state.textures[stage] = state.textures[0];
        state.textures[stage].source = (uint8_t *)texels; state.textures[stage].decode_context = texels;
        for (vertex = 0; vertex < 3; vertex++)
            memcpy(vertices[vertex].texture[stage], vertices[vertex].texture[0], sizeof vertices[vertex].texture[stage]);
    }
    for (index = 0; index <= entries; index++) texels[index] = 0xFF000000u | ((index * 7919u) & 0xFFFFFFu);
    before = gpu_decode_calls;
    for (index = 0; index < entries; index++) {
        state.textures[0].source = (uint8_t *)&texels[index]; state.textures[0].decode_context = &texels[index];
        check("texture cache population draw", nv2a_gpu_draw(&state, vertices, 3));
        if (index == 1) {
            state.textures[1].source = (uint8_t *)&texels[1]; state.textures[1].decode_context = &texels[1];
            check("shared source across recent texture stages", nv2a_gpu_draw(&state, vertices, 3));
            state.textures[1].source = (uint8_t *)&texels[0]; state.textures[1].decode_context = &texels[0];
            check("independent recent texture stages", nv2a_gpu_draw(&state, vertices, 3));
        }
        if (index == 1023) {
            state.stage_program = 1;
            state.textures[0].source = (uint8_t *)texels; state.textures[0].decode_context = texels;
            check("remember oldest texture before eviction", nv2a_gpu_draw(&state, vertices, 3));
        }
    }
    nv2a_gpu_flush();
    check("texture cache population decodes every new source", gpu_decode_calls == before + entries);
    check("texture cache vector growth preserves final pixels", color[9] == texels[entries - 1]);
    state.stage_program |= (1u << 5) | (1u << 10) | (1u << 15);
    state.textures[0].source = (uint8_t *)texels; state.textures[0].decode_context = texels;
    before = gpu_decode_calls;
    check("evicted texture draw", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
    check("FIFO-evicted source is decoded again", gpu_decode_calls == before + 1);
    check("FIFO-evicted texture pixels", color[9] == texels[0]);
    state.textures[0].source = (uint8_t *)&texels[entries - 1]; state.textures[0].decode_context = &texels[entries - 1];
    before = gpu_decode_calls;
    check("retained texture draw after eviction", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
    check("retained source is still a cache hit after index shifts", gpu_decode_calls == before);
    check("retained texture pixels after index shifts", color[9] == texels[entries - 1]);
    state.stage_program = 1;
    state.textures[0].pitch = 8;
    before = gpu_decode_calls;
    check("same-source pitch variant draw", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
    check("pitch variant creates a distinct texture", gpu_decode_calls == before + 1);
    state.textures[0].pitch = 4;
    before = gpu_decode_calls;
    check("original layout in same-source bucket", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
    check("same-source bucket preserves original layout hit", gpu_decode_calls == before);
    state.textures[0].source_bytes = 8;
    before = gpu_decode_calls;
    check("same-source byte-extent variant draw", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
    check("source byte extent creates a distinct texture", gpu_decode_calls == before + 1);
    texels[entries] ^= 0x123456;
    before = gpu_decode_calls;
    check("mutation outside decoded pixel draw", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
    check("complete source extent is still compared", gpu_decode_calls == before + 1);
    texels[entries - 1] = 0xFF314159;
    check("cached texel mutation draw", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
    check("cached texel mutation reaches hardware output", color[9] == 0xFF314159);
    nv2a_gpu_invalidate();
}

static int gpu_decode_cache_level(void *context, uint32_t face, uint32_t level,
                                  uint32_t horizontal, uint32_t vertical, uint32_t *color)
{
    if (face >= 6 || level >= 2 || horizontal >= (level ? 1u : 2u) || vertical >= (level ? 1u : 2u)) return 0;
    gpu_decode_calls++;
    *color = ((uint32_t *)context)[face * 32 + (level ? 4 : vertical * 2 + horizontal)];
    return 1;
}

static int gpu_decode_cache_volume(void *context, uint32_t level, uint32_t horizontal,
                                   uint32_t vertical, uint32_t slice, uint32_t *color)
{
    uint32_t size = level ? 1 : 2;
    if (level >= 2 || horizontal >= size || vertical >= size || slice >= size) return 0;
    gpu_decode_calls++;
    *color = ((uint32_t *)context)[level ? 8 : (slice * 2 + vertical) * 2 + horizontal];
    return 1;
}

static void test_gpu_texture_cache_dimensions(void)
{
    uint32_t texels[192] = {0}, color[64] = {0}, face, index, vertex, before;
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    gpu_texture_cache_setup(&state, vertices, color);
    state.textures[0].source = (uint8_t *)texels; state.textures[0].decode_context = texels;
    state.textures[0].width = state.textures[0].height = 2; state.textures[0].pitch = 8;
    state.textures[0].source_bytes = 20; state.textures[0].decode_level = gpu_decode_cache_level;
    for (index = 0; index < 4; index++) texels[index] = 0xFFFF0000;
    texels[4] = 0xFF00FF00;
    check("single-level texture with trailing source bytes", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
    check("single-level texture samples base", color[9] == 0xFFFF0000);
    state.textures[0].mip_levels = 2;
    state.textures[0].filter = (3u << 16) | (1u << 24);
    state.textures[0].control0_valid = 1;
    state.textures[0].control0 = (1u << 30) | (256u << 18) | (256u << 6);
    before = gpu_decode_calls;
    check("same-source mip-count variant", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
    check("mip count creates distinct resource layout", gpu_decode_calls == before + 5);
    check("forced mip samples correct level", color[9] == 0xFF00FF00);
    texels[4] = 0xFF314159;
    check("mip source mutation draw", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
    check("mip source mutation reaches hardware output", color[9] == 0xFF314159);
    for (face = 0; face < 6; face++) {
        for (index = 0; index < 4; index++) texels[face * 32 + index] = 0xFFFF0000;
        texels[face * 32 + 4] = 0xFF000000u | ((face + 1) * 0x00090703u);
    }
    state.stage_program = 3; state.textures[0].cube = 1; state.textures[0].face_stride = 128;
    state.textures[0].source_bytes = sizeof texels; state.textures[0].decode_face = gpu_decode_cube;
    for (vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].texture[0][0] = vertices[vertex].texture[0][1] = 0;
        vertices[vertex].texture[0][2] = -1;
    }
    check("same-source cube mip variant", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
    check("cube mip samples face and level", color[9] == texels[5 * 32 + 4]);
    state.stage_program = 2; state.textures[0].cube = 0; state.textures[0].face_stride = 0;
    state.textures[0].depth = 2; state.textures[0].source_bytes = 36;
    state.textures[0].decode_volume = gpu_decode_cache_volume;
    for (index = 0; index < 8; index++) texels[index] = index < 4 ? 0xFFFF0000 : 0xFF00FF00;
    texels[8] = 0xFF0000FF;
    for (vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].texture[0][0] = vertices[vertex].texture[0][1] = 0.5f;
        vertices[vertex].texture[0][2] = 0.25f;
    }
    check("same-source volume mip variant", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
    check("volume mip samples correct level", color[9] == 0xFF0000FF);
    state.textures[0].mip_levels = 1; state.textures[0].filter = state.textures[0].control0_valid = 0;
    check("same-source base-volume variant", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
    check("volume base samples correct slice", color[9] == 0xFFFF0000);
    for (vertex = 0; vertex < 3; vertex++) vertices[vertex].texture[0][2] = 0.75f;
    before = gpu_decode_calls;
    check("cached volume second-slice draw", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
    check("volume coordinate change reuses resource", gpu_decode_calls == before);
    check("volume second slice has correct pixels", color[9] == 0xFF00FF00);
    nv2a_gpu_invalidate();
}

static void test_gpu_texture_cache_benchmark(void)
{
    enum { entries = 512, iterations = 32768 };
    static uint32_t texels[entries], color[64];
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    LARGE_INTEGER frequency, start, end;
    uint32_t pass, index, before, workload;
    gpu_texture_cache_setup(&state, vertices, color);
    state.color_mask = 0;
    for (index = 0; index < entries; index++) {
        texels[index] = 0xFF000000u | ((index * 7919u) & 0xFFFFFFu);
        state.textures[0].source = (uint8_t *)&texels[index]; state.textures[0].decode_context = &texels[index];
        check("texture benchmark warm draw", nv2a_gpu_draw(&state, vertices, 3));
    }
    nv2a_gpu_flush();
    QueryPerformanceFrequency(&frequency);
    for (workload = 0; workload < 2; workload++) {
        before = gpu_decode_calls;
        QueryPerformanceCounter(&start);
        for (pass = 0; pass < iterations; pass++) {
            index = ((workload ? pass / 16 : pass) * 73u) % entries;
            state.textures[0].source = (uint8_t *)&texels[index]; state.textures[0].decode_context = &texels[index];
            check("texture benchmark cached draw", nv2a_gpu_draw(&state, vertices, 3));
        }
        nv2a_gpu_flush();
        QueryPerformanceCounter(&end);
        printf("texture cache benchmark %s: %.3f ms, %u draws, %u resident sources\n",
               workload ? "clustered" : "round-robin",
               (double)(end.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart, iterations, entries);
        check("texture benchmark reused every decoded resource", gpu_decode_calls == before);
        state.color_mask = 0x01010101;
        check("texture benchmark final visible draw", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
        check("texture benchmark final pixels", color[9] == texels[index]);
        state.color_mask = 0;
    }
    nv2a_gpu_invalidate();
    nv2a_gpu_report();
}

static void test_gpu_vertex(void)
{
    uint32_t color[64] = {0}, valid[136] = {15,15,15,15};
    uint32_t program[136][4] = {
        {0, (3u << 21) | 0x1Bu, (2u << 26) | (0x1Bu << 2), (3u << 28) | 0xF800u},
        {0, (1u << 21) | (3u << 9) | 0x1Bu, 2u << 26, 0xF800u | (3u << 3)},
        {0, (1u << 21) | (9u << 9) | 0x1Bu, 2u << 26, 0xF800u | (9u << 3)},
        {0, (1u << 21) | (10u << 9) | 0x1Bu, 2u << 26, 0xF800u | (10u << 3) | 1u}
    };
    float constants[192][4] = {{2,2,0,0}};
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[4] = {0};
    uint32_t vertex;
    state.color = (uint8_t *)color; state.width = state.height = state.clip_width = state.clip_height = 8;
    state.pitch = 32; state.bytes_per_pixel = 4; state.color_mask = 0x01010101;
    state.vertex_program = program; state.vertex_valid = valid; state.vertex_constants = constants;
    for (vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].attributes[0][3] = 1;
        vertices[vertex].attributes[3][0] = 0.5f; vertices[vertex].attributes[3][1] = 0.25f;
        vertices[vertex].attributes[3][2] = vertices[vertex].attributes[3][3] = 1;
    }
    vertices[1].attributes[0][0] = 4; vertices[2].attributes[0][1] = 4;
    check("native vertex MOV/ADD accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("native vertex constants and diffuse output", color[3 * 8 + 3] == 0xFF8040FF && color[9] == 0);
    nv2a_gpu_flush(); memset(color, 0, sizeof color);
    for (vertex = 0; vertex < 3; vertex++) vertices[vertex].attributes[3][1] = 0.5f;
    check("unchanged constants with changed vertices accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("reused constants still draw changed vertex color", color[3 * 8 + 3] == 0xFF8080FF && color[9] == 0);
    for (vertex = 0; vertex < 3; vertex++) vertices[vertex].attributes[3][1] = 0.25f;
    nv2a_gpu_invalidate(); memset(color, 0, sizeof color);
    constants[0][0] = constants[0][1] = 4;
    check("cached native vertex program accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("cached native vertex program observes constants", color[5 * 8 + 5] == 0xFF8040FF && color[3 * 8 + 3] == 0);
    nv2a_gpu_flush(); memset(color, 0, sizeof color);
    constants[0][0] = constants[0][1] = 2;
    check("restored native constants accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("constant mutation and restoration update geometry", color[3 * 8 + 3] == 0xFF8040FF && color[5 * 8 + 5] == 0);
    nv2a_gpu_invalidate();
    memset(color, 0, sizeof color); constants[0][0] = constants[0][1] = 2;
    for (vertex = 0; vertex < 4; vertex++) constants[1][vertex] = 0.5f;
    program[1][1] = (2u << 21) | (1u << 13) | (3u << 9) | 0x1Bu;
    program[1][2] = (2u << 26) | (0x1Bu << 17) | (3u << 11);
    check("mutated native vertex MUL program accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("native vertex MUL and instruction cache invalidation", color[3 * 8 + 3] == 0x80402080);
    nv2a_gpu_invalidate(); memset(color, 0, sizeof color);
    program[1][1] = (1u << 21) | (3u << 9) | 0x1Bu; program[1][2] = 2u << 26;
    for (vertex = 0; vertex < 3; vertex++) {
        memset(vertices[vertex].attributes[3], 0, sizeof vertices[vertex].attributes[3]);
        vertices[vertex].attributes[3][vertex] = 1; vertices[vertex].attributes[3][3] = 1;
    }
    state.flat_shading = 1;
    check("native flat-shaded vertex program accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("native flat shading uses final provoking vertex", color[3 * 8 + 3] == 0xFF0000FF);
    nv2a_gpu_invalidate();
    {
        uint32_t indices[6] = {0,1,2,2,1,3};
        vertices[3] = vertices[1]; vertices[3].attributes[0][1] = 4;
        state.indices = indices; state.index_count = 6;
        memset(color, 0, sizeof color);
        check("native indexed quad accepted", nv2a_gpu_draw(&state, vertices, 4)); nv2a_gpu_sync();
        check("indexed flat shading preserves provoking vertices", color[3 * 8 + 3] == 0xFF0000FF && color[5 * 8 + 5] == 0xFF00FF00);
        nv2a_gpu_invalidate(); memset(color, 0, sizeof color); indices[5] = 0;
        check("mutated native indices accepted", nv2a_gpu_draw(&state, vertices, 4)); nv2a_gpu_sync();
        check("native indices mutation changes geometry", color[3 * 8 + 3] == 0xFFFF0000 && color[5 * 8 + 5] == 0);
        indices[5] = 4;
        check("native out-of-range vertex index rejected", !nv2a_gpu_draw(&state, vertices, 4));
        state.indices = NULL; state.index_count = 0;
        nv2a_gpu_invalidate();
    }
    {
        uint32_t state_program[136][4] = {{0, (1u << 21) | 0x1Bu, 2u << 26, 0x3001u}};
        float attributes[16][4] = {{9,8,7,6}};
        check("native masked state program accepted", nv2a_gpu_execute_state(state_program, valid, 0, attributes, constants));
        check("native state mask preserves untouched constants", constants[0][0] == 2 && constants[0][1] == 2 &&
              constants[0][2] == 7 && constants[0][3] == 6 && constants[1][0] == 0.5f);
    }
}

static void gpu_packed_program(uint32_t program[136][4], uint32_t valid[136], uint32_t scenario)
{
    uint32_t slot, count = 2;
    memset(program, 0, 136 * 4 * sizeof(uint32_t)); memset(valid, 0, 136 * sizeof(uint32_t));
    if (scenario == 2) {
        program[0][1] = (1u << 21) | 0x1B; program[0][2] = 2u << 26; program[0][3] = 0xF800;
        program[1][1] = (1u << 21) | (1u << 9) | 0x1B;
        program[1][2] = 2u << 26; program[1][3] = 15u << 24;
        for (slot = 2; slot <= 15; slot++) {
            program[slot][1] = (3u << 21) | (slot << 9) | 0x1B;
            program[slot][2] = (2u << 26) | (0x1Bu << 2);
            program[slot][3] = (1u << 28) | (15u << 24);
        }
        program[16][1] = (1u << 21) | 0x1B; program[16][2] = 1u << 26;
        program[16][3] = 0xF800 | (3u << 3) | 1u;
        count = 17;
    } else for (slot = 0; slot < 2; slot++) {
        uint32_t attribute = slot ? (scenario == 1 ? 7 : 3) : (scenario == 1 ? 15 : 0);
        program[slot][1] = (1u << 21) | (scenario == 3 ? slot << 13 : attribute << 9) | 0x1B;
        program[slot][2] = (scenario == 3 ? 3u : 2u) << 26;
        program[slot][3] = 0xF800 | (slot ? (3u << 3) | 1u : 0);
    }
    for (slot = 0; slot < count; slot++) valid[slot] = 15;
}

static void gpu_packed_setup(Nv2aGpuDraw *state, uint32_t *color, uint32_t program[136][4],
                             uint32_t valid[136], float constants[192][4])
{
    state->color = (uint8_t *)color; state->width = state->height = state->clip_width = state->clip_height = 8;
    state->pitch = 32; state->bytes_per_pixel = 4; state->color_mask = 0x01010101;
    state->vertex_program = program; state->vertex_valid = valid; state->vertex_constants = constants;
}

static void test_gpu_packed_vertices(void)
{
    uint32_t program[136][4], valid[136], color[64], depth[64], scenario, vertex, pixel, attribute, component;
    uint32_t indices[6] = {0,1,2,2,1,3};
    float constants[192][4] = {{3,3,100,1},{0.25f,0.5f,1,1}};
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[4] = {0};
    gpu_packed_setup(&state, color, program, valid, constants);
    state.depth = (uint8_t *)depth; state.depth_pitch = 32; state.depth_format = 2;
    state.depth_enable = state.depth_mask = 1; state.depth_function = 0x207;
    for (scenario = 0; scenario < 4; scenario++) {
        uint32_t position = scenario == 1 ? 15 : 0, diffuse = scenario == 1 ? 7 : scenario == 2 ? 1 : 3;
        gpu_packed_program(program, valid, scenario);
        memset(vertices, 0, sizeof vertices);
        for (pixel = 0; pixel < 64; pixel++) { color[pixel] = 0xFF112233; depth[pixel] = 0xFFFFFFA5; }
        for (vertex = 0; vertex < 3; vertex++) {
            for (attribute = 0; attribute < 16; attribute++)
                if (scenario != 2 && (scenario == 3 || (attribute != position && attribute != diffuse)))
                    for (component = 0; component < 4; component++) {
                        uint32_t poison = 0x7FC01234;
                        memcpy(&vertices[vertex].attributes[attribute][component], &poison, sizeof poison);
                    }
            if (scenario != 3) {
                vertices[vertex].attributes[position][0] = vertex == 1 ? 7.0f : 1.0f;
                vertices[vertex].attributes[position][1] = vertex == 2 ? 7.0f : 1.0f;
                vertices[vertex].attributes[position][2] = 100; vertices[vertex].attributes[position][3] = 1;
                vertices[vertex].attributes[diffuse][0] = 0.25f; vertices[vertex].attributes[diffuse][1] = 0.5f;
                vertices[vertex].attributes[diffuse][2] = vertices[vertex].attributes[diffuse][3] = 1;
            }
        }
        check("packed vertex mask draw", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
        if (scenario == 3) {
            for (pixel = 0; pixel < 64; pixel++) {
                check("zero-input vertex program leaves degenerate color unchanged", color[pixel] == 0xFF112233);
                check("zero-input vertex program leaves degenerate depth unchanged", depth[pixel] == 0xFFFFFFA5);
            }
        } else {
            check("packed vertex mask exact color", color[27] == 0xFF4080FF);
            check("packed vertex mask exact depth and stencil", depth[27] == 0x000064A5);
            for (vertex = 0; vertex < 3; vertex++) {
                memset(vertices[vertex].attributes[diffuse], 0, 16);
                vertices[vertex].attributes[diffuse][0] = vertices[vertex].attributes[diffuse][3] = 1;
            }
            check("packed vertex mutation draw", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
            check("packed vertex mutation reaches exact output", color[27] == 0xFFFF0000);
            vertices[3] = vertices[1]; vertices[3].attributes[position][1] = 7;
            state.indices = indices; state.index_count = 6;
            check("packed indexed quad draw", nv2a_gpu_draw(&state, vertices, 4)); nv2a_gpu_flush();
            check("packed indexed offsets and stride", color[45] == 0xFFFF0000 && depth[45] == 0x000064A5);
            state.indices = NULL; state.index_count = 0;
        }
        nv2a_gpu_invalidate();
    }
    state.vertex_program = NULL; state.depth_enable = 0;
    for (vertex = 0; vertex < 3; vertex++) {
        memset(&vertices[vertex], 0, sizeof vertices[vertex]);
        vertices[vertex].position[0] = vertex == 1 ? 7.0f : 1.0f;
        vertices[vertex].position[1] = vertex == 2 ? 7.0f : 1.0f;
        vertices[vertex].position[3] = 1;
        vertices[vertex].diffuse[1] = vertices[vertex].diffuse[3] = 1;
    }
    check("canonical fixed-function stream after packed programs", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
    check("fixed-function input layout remains unchanged", color[27] == 0xFF00FF00);
    nv2a_gpu_invalidate();
}

static void test_gpu_packed_vertex_benchmark(void)
{
    enum { count = 6144, iterations = 128 };
    uint32_t program[136][4], valid[136], color[64] = {0}, vertex, pass;
    float constants[192][4] = {0};
    Nv2aGpuVertex *vertices = (Nv2aGpuVertex *)calloc(count, sizeof *vertices);
    Nv2aGpuDraw state = {0};
    LARGE_INTEGER frequency, start, end;
    check("vertex benchmark allocation", vertices != NULL);
    if (!vertices) return;
    gpu_packed_program(program, valid, 0);
    gpu_packed_setup(&state, color, program, valid, constants);
    state.color_mask = 0;
    for (vertex = 0; vertex < count; vertex++) {
        vertices[vertex].attributes[0][0] = vertex % 3 == 1 ? 7.0f : 1.0f;
        vertices[vertex].attributes[0][1] = vertex % 3 == 2 ? 7.0f : 1.0f;
        vertices[vertex].attributes[0][3] = 1;
        vertices[vertex].attributes[3][0] = 0.25f; vertices[vertex].attributes[3][1] = 0.5f;
        vertices[vertex].attributes[3][2] = vertices[vertex].attributes[3][3] = 1;
    }
    check("vertex benchmark warm draw", nv2a_gpu_draw(&state, vertices, count)); nv2a_gpu_flush();
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start);
    for (pass = 0; pass < iterations; pass++)
        check("vertex benchmark submission", nv2a_gpu_draw(&state, vertices, count));
    nv2a_gpu_flush();
    QueryPerformanceCounter(&end);
    printf("vertex upload benchmark: %.3f ms, %u draws, %u vertices per draw\n",
           (double)(end.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart, iterations, count);
    state.color_mask = 0x01010101;
    check("vertex benchmark visible output", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
    check("vertex benchmark exact final color", color[27] == 0xFF4080FF);
    nv2a_gpu_invalidate();
    free(vertices);
    nv2a_gpu_report();
}

static void test_gpu_indexed_stream_benchmark(void)
{
    enum { capacity = 6144 };
    uint32_t program[136][4], valid[136], color[64], expected[64], indices[capacity];
    float constants[192][4] = {0};
    Nv2aGpuVertex *vertices = (Nv2aGpuVertex *)calloc(capacity, sizeof *vertices);
    uint32_t pipeline, workload, vertex, pass;
    check("indexed stream benchmark allocation", vertices != NULL);
    if (!vertices) return;
    gpu_packed_program(program, valid, 0);
    for (vertex = 0; vertex < capacity; vertex++) {
        vertices[vertex].position[0] = vertex % 3 == 1 ? 7.0f : 1.0f;
        vertices[vertex].position[1] = vertex % 3 == 2 ? 7.0f : 1.0f;
        vertices[vertex].position[3] = 1;
        vertices[vertex].diffuse[0] = 0.25f; vertices[vertex].diffuse[1] = 0.5f;
        vertices[vertex].diffuse[2] = vertices[vertex].diffuse[3] = 1;
        memcpy(vertices[vertex].attributes[0], vertices[vertex].position, 16);
        memcpy(vertices[vertex].attributes[3], vertices[vertex].diffuse, 16);
        indices[vertex] = (vertex / 3) * 3 + (vertex % 3 ? 3 - vertex % 3 : 0);
    }
    for (pipeline = 0; pipeline < 2; pipeline++)
        for (workload = 0; workload < 2; workload++) {
            uint32_t count = workload ? capacity : 96, iterations = workload ? 128 : 4096;
            Nv2aGpuDraw state = {0};
            LARGE_INTEGER frequency, start, end;
            gpu_packed_setup(&state, color, program, valid, constants);
            if (!pipeline) state.vertex_program = 0;
            memset(color, 0xA5, sizeof color);
            check("indexed stream nonindexed reference", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_flush();
            memcpy(expected, color, sizeof expected);
            state.indices = indices; state.index_count = count; state.color_mask = 0;
            check("indexed stream benchmark warm draw", nv2a_gpu_draw(&state, vertices, count)); nv2a_gpu_flush();
            QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start);
            for (pass = 0; pass < iterations; pass++)
                check("indexed stream benchmark draw", nv2a_gpu_draw(&state, vertices, count));
            nv2a_gpu_flush();
            QueryPerformanceCounter(&end);
            printf("indexed stream benchmark %s %s: %.3f ms, %u draws, %u vertices\n",
                   pipeline ? "program" : "fixed", workload ? "large" : "small",
                   (double)(end.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart, iterations, count);
            memset(color, 0xA5, sizeof color); state.color_mask = 0x01010101;
            check("indexed stream visible draw", nv2a_gpu_draw(&state, vertices, count)); nv2a_gpu_flush();
            check("indexed stream complete output matches reference", !memcmp(color, expected, sizeof color));
            nv2a_gpu_invalidate();
        }
    free(vertices);
    nv2a_gpu_report();
}

static void test_gpu_dot_textures(void)
{
    const uint32_t expected_texels[4] = {13,9,10,6};
    const uint32_t boundary_bytes[4] = {0,127,128,255};
    const uint32_t boundary_texels[4][4] = {{0,5,10,0},{15,15,0,0},{0,0,0,15},{0,0,15,15}};
    uint32_t color[64] = {0}, texels[16];
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    state.color = (uint8_t *)color; state.width = state.height = state.clip_width = state.clip_height = 8;
    state.pitch = 32; state.bytes_per_pixel = 4; state.color_mask = 0x01010101;
    state.combiners.control = 1; state.combiners.rgb_input[0] = 0x0A200000; state.combiners.alpha_input[0] = 0x0000201A;
    state.combiners.rgb_output[0] = state.combiners.alpha_output[0] = 0xC00;
    state.combiners.final_input[0] = 0xC; state.combiners.final_input[1] = 0x1C80;
    for (uint32_t texel = 0; texel < 16; texel++) texels[texel] = 0xFF000000u | ((texel + 1) * 0x000B0703u);
    for (uint32_t stage = 2; stage < 4; stage++) {
        state.textures[stage].source = (const uint8_t *)texels; state.textures[stage].source_bytes = sizeof texels;
        state.textures[stage].width = state.textures[stage].height = 4; state.textures[stage].pitch = 16;
        state.textures[stage].format = 6; state.textures[stage].decode = gpu_decode_grid; state.textures[stage].decode_context = texels;
        state.textures[stage].address_u = state.textures[stage].address_v = 1;
    }
    for (uint32_t vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].position[3] = 1;
        vertices[vertex].texture[0][0] = 64.0f / 255; vertices[vertex].texture[0][1] = 192.0f / 255;
        vertices[vertex].texture[0][2] = 32.0f / 255;
        vertices[vertex].texture[1][0] = 1; vertices[vertex].texture[2][1] = 1;
    }
    vertices[1].position[0] = 8; vertices[2].position[1] = 8;
    state.stage_program = 4u | (17u << 5) | (9u << 10);
    nv2a_gpu_invalidate();
    for (uint32_t mapping = 0; mapping < 4; mapping++) {
        state.shader_dot_mapping = mapping | (mapping << 4);
        check("native dot texture mapping accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("native dot texture mapping selects distinct texel", color[9] == texels[expected_texels[mapping]]);
        for (uint32_t sample = 0; sample < 4; sample++) {
            for (uint32_t vertex = 0; vertex < 3; vertex++) {
                vertices[vertex].texture[0][0] = boundary_bytes[sample] / 255.0f;
                vertices[vertex].texture[0][1] = boundary_bytes[sample] / 255.0f;
            }
            check("native dot texture byte boundary accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
            check("native dot mapping handles sign endpoints", color[9] == texels[boundary_texels[mapping][sample]]);
        }
        for (uint32_t vertex = 0; vertex < 3; vertex++) {
            vertices[vertex].texture[0][0] = 64.0f / 255; vertices[vertex].texture[0][1] = 192.0f / 255;
        }
    }
    state.shader_dot_mapping = 0;
    for (uint32_t vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].texture[1][1] = 0.25f; vertices[vertex].texture[1][2] = 0.5f;
        vertices[vertex].texture[2][0] = -0.25f; vertices[vertex].texture[2][2] = 0.25f;
    }
    check("multicomponent dot coordinates accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("dot coordinates include weighted signed XYZ contributions", color[9] == texels[10]);
    for (uint32_t vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].texture[1][1] = vertices[vertex].texture[1][2] = 0;
        vertices[vertex].texture[2][0] = vertices[vertex].texture[2][2] = 0;
    }
    state.combiners.rgb_input[0] = 0x0B200000; state.combiners.alpha_input[0] = 0x0000201B;
    for (uint32_t vertex = 0; vertex < 3; vertex++) vertices[vertex].texture[3][2] = 1;
    state.stage_program = 4u | (17u << 5) | (17u << 10) | (9u << 15);
    check("dot ST at stage three accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("stage three consumes stage two dot intermediate", color[9] == texels[3]);
    state.shader_dot_mapping = 2u << 8;
    check("cached stage three dot mapping accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("stage three mapping uses independent high field", color[9] == texels[7]);
    state.shader_dot_mapping = 0; state.stage_program = 4u | (17u << 5) | (9u << 10);
    for (uint32_t vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].texture[3][0] = 1; vertices[vertex].texture[3][2] = 0;
    }
    state.stage_program |= 9u << 15;
    check("chained dot texture stages accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("dot texture chain retains preceding dot value", color[9] == texels[7]);
    state.shader_other_stage_input = 2u << 20;
    check("cached dot texture selector change accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("dot texture selector uses sampled stage output", color[9] == texels[11]);
    state.shader_other_stage_input = 0; state.stage_program = 4u | (17u << 5) | (17u << 10);
    state.combiners.rgb_input[0] = 0x0A200000; state.combiners.alpha_input[0] = 0x0000201A;
    check("dot product zero output accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("dot product texture register is zero including alpha", color[9] == 0);
    state.stage_program = 4u | (17u << 5) | (9u << 10); state.shader_dot_mapping = 4;
    check("unresolved HILO dot mapping rejected", !nv2a_gpu_draw(&state, vertices, 3));
    state.shader_dot_mapping = 0; state.stage_program = 4u | (9u << 10);
    check("dot texture missing predecessor rejected", !nv2a_gpu_draw(&state, vertices, 3));
    state.stage_program = 4u | (9u << 5);
    check("dot texture invalid stage rejected", !nv2a_gpu_draw(&state, vertices, 3));
    state.stage_program = 17;
    check("dot product stage zero rejected", !nv2a_gpu_draw(&state, vertices, 3));
    state.stage_program = 4u | (17u << 15);
    check("dot product stage three rejected", !nv2a_gpu_draw(&state, vertices, 3));
    state.stage_program = 4u | (17u << 5) | (9u << 10); state.shader_other_stage_input = 2u << 16;
    check("dot texture self input rejected", !nv2a_gpu_draw(&state, vertices, 3));
    state.shader_other_stage_input = 0; state.textures[2].cube = 1;
    check("dot ST cube resource rejected", !nv2a_gpu_draw(&state, vertices, 3));
    state.textures[2].cube = 0; state.textures[2].linear = 1;
    check("dot ST linear resource rejected", !nv2a_gpu_draw(&state, vertices, 3));
    nv2a_gpu_invalidate();
}

static void test_gpu_dependent_textures(void)
{
    const float producer_coordinates[3][4] = {{0.125f,0.375f,0.625f,0.875f},
                                              {0.375f,0.625f,0.875f,0.125f},
                                              {0.625f,0.875f,0.125f,0.375f}};
    const uint32_t expected_texels[2][3] = {{3,4,9},{9,14,3}};
    uint32_t color[64] = {0}, texels[16], producer_texel = 0xDF205F9F;
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    state.color = (uint8_t *)color; state.width = state.height = state.clip_width = state.clip_height = 8;
    state.pitch = 32; state.bytes_per_pixel = 4; state.color_mask = 0x01010101;
    state.combiners.control = 1; state.combiners.rgb_output[0] = state.combiners.alpha_output[0] = 0xC00;
    state.combiners.final_input[0] = 0xC; state.combiners.final_input[1] = 0x1C80;
    for (uint32_t texel = 0; texel < 16; texel++) texels[texel] = 0xFF000000u | ((texel + 1) * 0x00090703u);
    for (uint32_t stage = 0; stage < 4; stage++) {
        state.textures[stage].source = (const uint8_t *)texels; state.textures[stage].source_bytes = sizeof texels;
        state.textures[stage].width = state.textures[stage].height = 4; state.textures[stage].pitch = 16;
        state.textures[stage].format = 6; state.textures[stage].decode = gpu_decode_grid; state.textures[stage].decode_context = texels;
    }
    for (uint32_t vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].position[3] = 1;
        for (uint32_t stage = 0; stage < 3; stage++) memcpy(vertices[vertex].texture[stage], producer_coordinates[stage], sizeof producer_coordinates[stage]);
    }
    vertices[1].position[0] = 8; vertices[2].position[1] = 8;
    nv2a_gpu_invalidate();
    for (uint32_t mode = 0; mode < 2; mode++) {
        for (uint32_t stage = 1; stage < 4; stage++) {
            state.stage_program = (15u + mode) << (stage * 5);
            for (uint32_t producer = 0; producer < stage; producer++) state.stage_program |= 4u << (producer * 5);
            state.combiners.rgb_input[0] = ((8u + stage) << 24) | 0x00200000;
            state.combiners.alpha_input[0] = 0x00002010u | (8u + stage);
            for (uint32_t vertex = 0; vertex < 3; vertex++) memset(vertices[vertex].texture[stage], 0, sizeof vertices[vertex].texture[stage]);
            for (uint32_t source_stage = 0; source_stage < stage; source_stage++) {
                state.shader_other_stage_input = stage == 1 ? 0xF000u : source_stage << (stage * 4 + 8);
                check("native dependent texture read accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
                check("dependent texture uses selected AR or GB channels", color[9] == texels[expected_texels[mode][source_stage]]);
            }
            for (uint32_t vertex = 0; vertex < 3; vertex++) {
                if (stage < 3) memcpy(vertices[vertex].texture[stage], producer_coordinates[stage], sizeof producer_coordinates[stage]);
            }
        }
    }
    state.textures[0].source = (const uint8_t *)&producer_texel; state.textures[0].source_bytes = 4;
    state.textures[0].width = state.textures[0].height = 1; state.textures[0].pitch = 4;
    state.textures[0].decode = gpu_decode; state.textures[0].decode_context = &producer_texel;
    for (uint32_t vertex = 0; vertex < 3; vertex++) vertices[vertex].texture[0][3] = 1;
    state.stage_program = 1u | (16u << 5) | (15u << 10); state.shader_other_stage_input = 1u << 16;
    state.combiners.rgb_input[0] = 0x0A200000; state.combiners.alpha_input[0] = 0x0000201A;
    check("chained dependent texture reads accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("dependent reads consume sampled producer output", color[9] == texels[7]);
    state.shader_other_stage_input = 0;
    check("cached dependent producer mutation accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("cached dependent shader observes producer mutation", color[9] == texels[3]);
    state.shader_other_stage_input = 2u << 16;
    check("same-stage dependent input rejected", !nv2a_gpu_draw(&state, vertices, 3));
    state.shader_other_stage_input = 15u << 16;
    check("out-of-range dependent input rejected", !nv2a_gpu_draw(&state, vertices, 3));
    state.shader_other_stage_input = 0; state.textures[2].linear = 1;
    check("dependent linear texture rejected", !nv2a_gpu_draw(&state, vertices, 3));
    state.textures[2].linear = 0; state.textures[2].cube = 1;
    check("dependent cube texture rejected", !nv2a_gpu_draw(&state, vertices, 3));
    state.textures[2].cube = 0; state.stage_program = 15;
    check("dependent read in stage zero rejected", !nv2a_gpu_draw(&state, vertices, 3));
    nv2a_gpu_invalidate();
}

static void test_gpu_clip_planes(void)
{
    const float values[4] = {-1, -0.0f, 0, 1};
    uint32_t color[64], depth[64];
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    state.color = (uint8_t *)color; state.depth = (uint8_t *)depth;
    state.width = state.height = state.clip_width = state.clip_height = 8;
    state.pitch = state.depth_pitch = 32; state.bytes_per_pixel = 4; state.color_mask = 0x01010101;
    state.depth_enable = state.depth_mask = 1; state.depth_format = 2; state.depth_function = 0x207;
    for (uint32_t vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].position[3] = 1; vertices[vertex].position[2] = 100;
        vertices[vertex].diffuse[0] = vertices[vertex].diffuse[3] = 1;
        for (uint32_t stage = 0; stage < 4; stage++)
            for (uint32_t component = 0; component < 4; component++) vertices[vertex].texture[stage][component] = 1;
    }
    vertices[1].position[0] = 8; vertices[2].position[1] = 8;
    nv2a_gpu_invalidate();
    for (uint32_t stage = 0; stage < 4; stage++) {
        state.stage_program = 5u << (stage * 5);
        for (uint32_t component = 0; component < 4; component++) {
            for (uint32_t polarity = 0; polarity < 2; polarity++) {
                state.shader_clip_mode = polarity << (stage * 4 + component);
                for (uint32_t sample = 0; sample < 4; sample++) {
                    int discarded = polarity ? values[sample] >= 0 : values[sample] < 0;
                    for (uint32_t pixel_index = 0; pixel_index < 64; pixel_index++) {
                        color[pixel_index] = 0xFF123456; depth[pixel_index] = 0xFFFFFF5A;
                    }
                    for (uint32_t vertex = 0; vertex < 3; vertex++) vertices[vertex].texture[stage][component] = values[sample];
                    check("native clip-plane comparison accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
                    check("clip-plane comparison preserves or writes color", color[9] == (discarded ? 0xFF123456 : 0xFFFF0000));
                    check("clip-plane discard preserves depth and stencil", depth[9] == (discarded ? 0xFFFFFF5A : 0x0000645A));
                    nv2a_gpu_invalidate();
                }
            }
            for (uint32_t vertex = 0; vertex < 3; vertex++) vertices[vertex].texture[stage][component] = 1;
        }
    }
    state.stage_program = 5; state.shader_clip_mode = 0;
    for (uint32_t pixel_index = 0; pixel_index < 64; pixel_index++) {
        color[pixel_index] = 0xFF123456; depth[pixel_index] = 0xFFFFFF5A;
    }
    vertices[0].texture[0][0] = vertices[2].texture[0][0] = -1;
    check("interpolated clip plane accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("clip plane discards per fragment rather than per triangle", color[9] == 0xFF123456 && color[13] == 0xFFFF0000);
    check("interpolated clip plane preserves discarded depth", depth[9] == 0xFFFFFF5A && depth[13] == 0x0000645A);
    nv2a_gpu_invalidate();
    for (uint32_t vertex = 0; vertex < 3; vertex++) vertices[vertex].texture[0][0] = 1;
    state.combiners.control = 1; state.combiners.rgb_input[0] = 0x08200000;
    state.combiners.alpha_input[0] = 0x00002018;
    state.combiners.rgb_output[0] = state.combiners.alpha_output[0] = 0xC00;
    state.combiners.final_input[0] = 0xC; state.combiners.final_input[1] = 0x1C80;
    check("clip-plane output register accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("clip-plane output is zero including alpha", color[9] == 0);
    nv2a_gpu_invalidate();
}

static void test_gpu(void)
{
    uint32_t framebuffer[64] = {0}, texels[18] = {0xFF804020,0xFF202040};
    uint32_t depthbuffer[64];
    uint32_t resolved[64] = {0};
    uint16_t depth16[64];
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    uint32_t index;
    state.color = (uint8_t *)framebuffer;
    state.width = state.height = state.clip_width = state.clip_height = 8;
    state.pitch = 32; state.bytes_per_pixel = 4; state.color_mask = 0x01010101;
    state.stage_program = 1; state.combiners.control = 1;
    state.combiners.rgb_input[0] = 0x08040000; state.combiners.rgb_output[0] = 0xC00;
    state.combiners.alpha_input[0] = 0x00002014; state.combiners.alpha_output[0] = 0xC00;
    state.combiners.final_input[0] = 0x0000000C; state.combiners.final_input[1] = 0x00001C80;
    for (index = 0; index < 2; index++) {
        state.textures[index].source = (const uint8_t *)&texels[index]; state.textures[index].source_bytes = 4;
        state.textures[index].width = state.textures[index].height = 1; state.textures[index].pitch = 4;
        state.textures[index].linear = 1; state.textures[index].decode = gpu_decode; state.textures[index].decode_context = &texels[index];
    }
    for (index = 0; index < 3; index++) {
        vertices[index].position[3] = 1; vertices[index].diffuse[0] = 0.5f; vertices[index].diffuse[1] = 0.25f;
        vertices[index].diffuse[2] = vertices[index].diffuse[3] = 1;
        vertices[index].texture[0][3] = vertices[index].texture[1][3] = 1;
    }
    vertices[1].position[0] = 8; vertices[2].position[1] = 8;
    check("hardware textured triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware texture/diffuse pixel", framebuffer[9] == 0xFF401020);
    state.stage_program = 0x21; state.combiners.control = 2;
    state.combiners.rgb_input[1] = 0x0C090000; state.combiners.rgb_output[1] = 0xC00;
    state.combiners.alpha_input[1] = 0x0000201C; state.combiners.alpha_output[1] = 0xC00;
    check("hardware second combiner stage accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware two-stage pixel", framebuffer[9] == 0xFF080208);
    texels[0] = 0xFF202040;
    check("hardware mutated source accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware observes source mutation", framebuffer[9] == 0xFF020110);
    {
        uint32_t decode_before = gpu_decode_calls;
        state.textures[0].source_bytes = sizeof texels;
        check("expanded texture validation extent accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("texture extent change invalidates cached resource", gpu_decode_calls == decode_before + 1);
        decode_before = gpu_decode_calls;
        check("unchanged full texture snapshot accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("unchanged texture snapshot avoids redecoding", gpu_decode_calls == decode_before);
        texels[17] ^= 0x01000000u;
        check("texture tail mutation accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("texture snapshot checks final source byte", gpu_decode_calls == decode_before + 1);
        decode_before = gpu_decode_calls; state.textures[0].pitch = 8;
        check("texture pitch mutation accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
        check("texture layout participates in cache identity", gpu_decode_calls == decode_before + 1);
        state.textures[0].pitch = 4; state.textures[0].source_bytes = 4;
    }
    state.alpha_enable = 1; state.alpha_function = 0x204; state.alpha_reference = 128;
    for (index = 0; index < 3; index++) vertices[index].diffuse[3] = 0.25f;
    check("hardware alpha-tested triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware alpha discard preserves color", framebuffer[9] == 0xFF020110);
    nv2a_gpu_invalidate();
    for (index = 0; index < 64; index++) framebuffer[index] = 0xFF000000;
    texels[0] = 0xFFFF0000;
    state.alpha_enable = 0; state.stage_program = 1; state.combiners.control = 1;
    state.blend_enable = 1; state.blend_source = 0x302; state.blend_destination = 0x303; state.blend_equation = 0x8006;
    for (index = 0; index < 3; index++) { vertices[index].diffuse[0] = 1; vertices[index].diffuse[3] = 0.5f; }
    check("hardware blended triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware source-alpha blend pixel", framebuffer[9] == 0xBF800000);
    state.blend_enable = 0; state.color_mask = 0x01000001; texels[0] = 0xFF00FF00;
    check("hardware masked triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware channel mask preserves red and green", (framebuffer[9] & 0x00FFFF00) == 0x00800000);
    nv2a_gpu_invalidate();
    for (index = 0; index < 64; index++) { framebuffer[index] = 0xFF000000; depthbuffer[index] = 0xFFFFFF5A; }
    state.color_mask = 0x01010101; texels[0] = 0xFF804020;
    state.depth = (uint8_t *)depthbuffer; state.depth_pitch = 32; state.depth_format = 2;
    state.depth_enable = state.depth_mask = 1; state.depth_function = 0x201;
    for (index = 0; index < 3; index++) { vertices[index].position[2] = 100; vertices[index].diffuse[0] = 0.5f; vertices[index].diffuse[3] = 1; }
    check("hardware depth-tested triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    if (depthbuffer[9] != 0x0000645A) printf("hardware depth readback: %08X (expected 0000645A)\n", depthbuffer[9]);
    check("hardware Z24 write preserves stencil", depthbuffer[9] == 0x0000645A);
    check("hardware depth-tested color", framebuffer[9] == 0xFF401020);
    for (index = 0; index < 3; index++) { vertices[index].position[2] = 200; vertices[index].diffuse[0] = 1; }
    check("hardware occluded triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware occlusion preserves color and depth", framebuffer[9] == 0xFF401020 && depthbuffer[9] == 0x0000645A);
    state.alpha_enable = 1; state.alpha_function = 0x204; state.alpha_reference = 128;
    for (index = 0; index < 3; index++) { vertices[index].position[2] = 50; vertices[index].diffuse[3] = 0.25f; }
    check("hardware alpha-tested depth triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware alpha discard preserves depth", depthbuffer[9] == 0x0000645A && framebuffer[9] == 0xFF401020);
    state.alpha_enable = 0;
    check("hardware nearer triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware nearer depth updates color", (framebuffer[9] & 0x00FFFFFF) == 0x00801020);
    if ((framebuffer[9] & 0x00FFFFFF) != 0x00801020) printf("hardware nearer triangle color: %08X depth %08X\n", framebuffer[9], depthbuffer[9]);
    state.alpha_enable = state.depth_enable = 0;
    state.combiners.rgb_input[0] = 0x08200000; state.combiners.alpha_input[0] = 0x00002018;
    check("hardware resolve source draw accepted", nv2a_gpu_draw(&state, vertices, 3));
    state.color = (uint8_t *)resolved;
    state.textures[0].source = (const uint8_t *)framebuffer; state.textures[0].source_bytes = sizeof framebuffer;
    state.textures[0].width = state.textures[0].height = 8; state.textures[0].pitch = 32; state.textures[0].format = 0x12;
    state.textures[0].decode_context = framebuffer;
    check("hardware resident render-target resolve accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware resolve reads GPU result rather than stale host pixels", resolved[9] == 0xFF804020);
    nv2a_gpu_flush();
    for (index = 0; index < 64; index++) framebuffer[index] = 0xFF204080;
    resolved[63] = 0xFFABCDEF;
    check("retained resolve observes CPU source pixels", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("retained resolve refreshes source SRV and target pixels", resolved[9] == 0xFF204080 && resolved[63] == 0xFFABCDEF);
    nv2a_gpu_flush();
    for (index = 0; index < 3; index++) {
        vertices[index].texture[0][0] = 3.5f; vertices[index].texture[0][1] = 2.5f;
    }
    framebuffer[19] = 0xFF112233;
    check("single-texel resident source mutation accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("boxed color refresh samples changed nonzero texel", resolved[9] == 0xFF112233 && resolved[63] == 0xFFABCDEF);
    nv2a_gpu_flush(); framebuffer[19] = 0xFF204080;
    check("resident source restoration accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("updated upload snapshot observes restoration", resolved[9] == 0xFF204080 && resolved[63] == 0xFFABCDEF);
    nv2a_gpu_flush();
    framebuffer[10] = 0xFF556677; framebuffer[35] = 0xFF99AABB;
    for (index = 0; index < 3; index++) {
        vertices[index].texture[0][0] = 2.5f; vertices[index].texture[0][1] = 1.5f;
    }
    check("multirow resident source mutation accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("boxed color refresh samples first changed row", resolved[9] == 0xFF556677);
    nv2a_gpu_flush();
    for (index = 0; index < 3; index++) {
        vertices[index].texture[0][0] = 3.5f; vertices[index].texture[0][1] = 4.5f;
    }
    check("unchanged resident source reuse accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("boxed upload preserves source pitch and final changed row", resolved[9] == 0xFF99AABB && resolved[63] == 0xFFABCDEF);
    nv2a_gpu_flush(); framebuffer[10] = framebuffer[35] = 0xFF204080;
    check("multirow resident source restoration accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("multirow upload snapshot observes restoration", resolved[9] == 0xFF204080);
    for (index = 0; index < 3; index++) vertices[index].texture[0][0] = vertices[index].texture[0][1] = 0;
    state.color = (uint8_t *)framebuffer;
    check("hardware feedback alias safely falls back", !nv2a_gpu_draw(&state, vertices, 3));
    nv2a_gpu_invalidate();
    state.textures[0].source = (const uint8_t *)texels; state.textures[0].source_bytes = 4;
    state.textures[0].width = state.textures[0].height = 1; state.textures[0].pitch = 4; state.textures[0].decode_context = texels;
    state.depth_enable = 1; state.depth_format = 1; state.depth_pitch = 16; state.depth = (uint8_t *)depth16;
    for (index = 0; index < 64; index++) depth16[index] = 65535;
    for (index = 0; index < 3; index++) vertices[index].position[2] = 100;
    check("hardware Z16 triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware Z16 guest depth write", depth16[9] == 100);
    nv2a_gpu_invalidate();
    for (index = 0; index < 64; index++) depth16[index] = 65535;
    for (index = 0; index < 3; index++) vertices[index].position[2] = 100.75f;
    check("hardware fractional Z16 triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware Z16 truncates guest fractional depth", depth16[9] == 100);
    nv2a_gpu_invalidate();
    for (index = 0; index < 64; index++) depthbuffer[index] = 0xFFFFFF5A;
    state.depth_format = 2; state.depth_pitch = 32; state.depth = (uint8_t *)depthbuffer; state.control0 = 0x10000;
    for (index = 0; index < 3; index++) { vertices[index].position[3] = 100; vertices[index].position[2] = 500; }
    check("hardware W-depth triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    if (depthbuffer[9] != 0x0000645A) printf("hardware W-depth readback: %08X (expected 0000645A)\n", depthbuffer[9]);
    check("hardware W-depth uses reciprocal clip W rather than Z", depthbuffer[9] == 0x0000645A);
    for (index = 0; index < 3; index++) vertices[index].position[3] = 200;
    check("hardware W-depth occluded triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware W-depth rejection preserves guest depth", depthbuffer[9] == 0x0000645A);
    nv2a_gpu_invalidate();
    for (index = 0; index < 64; index++) depthbuffer[index] = 0xFFFFFF5A;
    vertices[0].position[3] = 100; vertices[1].position[3] = 200; vertices[2].position[3] = 400;
    check("hardware varying W-depth triangle accepted", nv2a_gpu_draw(&state, vertices, 3)); nv2a_gpu_sync();
    check("hardware W-depth matches integer harmonic interpolation", depthbuffer[9] == 0x0000825A);
    nv2a_gpu_invalidate();
}

static void test_gpu_depth_transition(void)
{
    uint32_t color[64] = {0}, depth[64];
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    state.color = (uint8_t *)color;
    state.width = state.height = state.clip_width = state.clip_height = 8;
    state.pitch = 32; state.bytes_per_pixel = 4; state.color_mask = 0x01010101;
    state.depth = (uint8_t *)depth; state.depth_pitch = 32; state.depth_format = 2;
    state.depth_mask = 1; state.depth_function = 0x207;
    for (uint32_t pixel = 0; pixel < 64; pixel++) depth[pixel] = 0xFFFFFFA5;
    vertices[1].position[0] = vertices[2].position[1] = 16;
    for (uint32_t vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].position[2] = 50; vertices[vertex].position[3] = 1;
        vertices[vertex].diffuse[0] = vertices[vertex].diffuse[3] = 1;
    }
    check("cold color-only draw accepted", nv2a_gpu_draw(&state, vertices, 3));
    nv2a_gpu_flush_reason(NV2A_GPU_SYNC_SEMAPHORE);
    check("cold color-only output preserves unused depth",
          color[63] == 0xFFFF0000 && depth[63] == 0xFFFFFFA5);
    state.depth_enable = 1;
    check("cold transition to depth draw accepted", nv2a_gpu_draw(&state, vertices, 3));
    nv2a_gpu_flush_reason(NV2A_GPU_SYNC_SEMAPHORE);
    if (depth[63] != 0x000032A5)
        fprintf(stderr, "cold transition depth: %08X, expected 000032A5\n", depth[63]);
    check("cold transition publishes exact depth and stencil", depth[63] == 0x000032A5);
    state.depth_enable = 0;
    check("transition back to color-only accepted", nv2a_gpu_draw(&state, vertices, 3));
    nv2a_gpu_flush_reason(NV2A_GPU_SYNC_SEMAPHORE);
    depth[63] = 0xFFFFFF33;
    state.depth_enable = 1;
    check("depth re-enable after CPU mutation accepted", nv2a_gpu_draw(&state, vertices, 3));
    nv2a_gpu_flush_reason(NV2A_GPU_SYNC_SEMAPHORE);
    check("depth re-enable preserves CPU stencil mutation", depth[63] == 0x00003233);
    nv2a_gpu_invalidate();
}

static void test_gpu_homogeneous_clipping(void)
{
    uint32_t color[2][64], depth[2][64];
    const uint32_t indices[] = {0, 1, 2, 0, 2, 3};
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[4] = {0};
    state.width = state.height = state.clip_width = state.clip_height = 8;
    state.pitch = state.depth_pitch = 32; state.bytes_per_pixel = 4;
    state.color_mask = 0x01010101; state.depth_format = 2;
    state.depth_mask = 1; state.depth_function = 0x207;
    state.blend_enable = 1; state.blend_source = 0x302;
    state.blend_destination = 0x303; state.blend_equation = 0x8006;
    state.indices = indices;
    vertices[1].position[0] = vertices[2].position[0] = 8;
    vertices[2].position[1] = vertices[3].position[1] = 8;
    for (uint32_t vertex = 0; vertex < 4; vertex++) {
        vertices[vertex].position[2] = 50;
        vertices[vertex].diffuse[0] = 1;
        vertices[vertex].diffuse[3] = 0.5f;
    }
    for (uint32_t enabled = 0; enabled < 2; enabled++)
        for (uint32_t batch = 0; batch < 2; batch++)
            for (uint32_t scenario = 0; scenario < 5; scenario++) {
                state.depth_enable = enabled;
                state.index_count = batch ? 6 : 3;
                for (uint32_t vertex = 0; vertex < 4; vertex++)
                    vertices[vertex].position[3] = scenario == 4 ? -1.0f : 1.0f;
                if (scenario >= 1 && scenario <= 3)
                    vertices[1].position[3] = scenario == 3 ? -0.25f : -1.0f;
                if (scenario == 2) vertices[3].position[3] = -1;
                for (uint32_t pipeline = 0; pipeline < 2; pipeline++) {
                    for (uint32_t pixel = 0; pixel < 64; pixel++) {
                        color[pipeline][pixel] = 0xFF000000;
                        depth[pipeline][pixel] = 0xFFFFFFA5;
                    }
                    state.color = (uint8_t *)color[pipeline];
                    state.depth = (uint8_t *)depth[pipeline];
                    /* Uniform colors make the flat-shading GS match the no-GS reference. */
                    state.flat_shading = pipeline;
                    check("homogeneous clipping draw accepted", nv2a_gpu_draw(&state, vertices, 4));
                    nv2a_gpu_flush_reason(NV2A_GPU_SYNC_SEMAPHORE);
                    nv2a_gpu_invalidate();
                }
                check("GS preserves native homogeneous-clipped color",
                      !memcmp(color[0], color[1], sizeof color[0]));
                check("GS preserves native homogeneous-clipped depth/stencil",
                      !memcmp(depth[0], depth[1], sizeof depth[0]));
                unsigned visible = 0;
                for (uint32_t pixel = 0; pixel < 64; pixel++)
                    visible += color[0][pixel] != 0xFF000000;
                if (scenario == 0 || scenario == 1 || scenario == 3)
                    check("camera-plane crossing retains visible reference fragments", visible != 0);
                if (scenario == 4)
                    check("fully behind-camera primitives remain clipped", visible == 0);
            }
}

static void test_gpu_simd_validation(void)
{
    uint32_t color[64] = {0}, depth[64], texel = 0xFFFF0000, indices[12];
    Nv2aGpuDraw state = {0};
    Nv2aGpuVertex vertices[3] = {0};
    state.color = (uint8_t *)color;
    state.width = state.height = state.clip_width = state.clip_height = 8;
    state.pitch = 32; state.bytes_per_pixel = 4; state.color_mask = 0x01010101;
    state.depth = (uint8_t *)depth; state.depth_pitch = 32; state.depth_format = 2;
    state.depth_enable = state.depth_mask = 1; state.depth_function = 0x207;
    for (uint32_t pixel = 0; pixel < 64; pixel++) depth[pixel] = 0xFFFFFFA5;
    state.indices = indices; state.index_count = 12;
    vertices[1].position[0] = vertices[2].position[1] = 16;
    for (uint32_t vertex = 0; vertex < 3; vertex++) {
        vertices[vertex].position[2] = 50;
        vertices[vertex].position[3] = 1;
        vertices[vertex].diffuse[0] = vertices[vertex].diffuse[3] = 1;
        vertices[vertex].texture[0][3] = 1;
    }
    for (uint32_t index = 0; index < 12; index++) indices[index] = index % 3;
    check("SIMD validation accepts complete indexed draw", nv2a_gpu_draw(&state, vertices, 3));
    for (uint32_t index = 0; index < 12; index++) {
        uint32_t saved = indices[index];
        for (uint32_t invalid = 0; invalid < 3; invalid++) {
            indices[index] = invalid == 0 ? 3 : invalid == 1 ? 0x80000000u : UINT32_MAX;
            check("SIMD validation rejects invalid index in every lane", !nv2a_gpu_draw(&state, vertices, 3));
        }
        indices[index] = saved;
    }
    for (uint32_t vertex = 0; vertex < 3; vertex++)
        for (uint32_t lane = 0; lane < 4; lane++) {
            float saved = vertices[vertex].position[lane];
            uint32_t invalid = lane & 1 ? 0x7F800001u : 0xFF800000u;
            memcpy(&vertices[vertex].position[lane], &invalid, 4);
            check("SIMD validation rejects nonfinite position in every lane", !nv2a_gpu_draw(&state, vertices, 3));
            vertices[vertex].position[lane] = saved;
        }
    nv2a_gpu_flush_reason(NV2A_GPU_SYNC_SEMAPHORE);
    check("SIMD rejection preserves prior draw completion and exact output", color[63] == 0xFFFF0000);
    check("SIMD rejection preserves exact pending depth and stencil", depth[63] == 0x000032A5);
    state.stage_program = 1;
    state.textures[0].source = (const uint8_t *)&texel; state.textures[0].source_bytes = 4;
    state.textures[0].width = state.textures[0].height = 1; state.textures[0].pitch = 4;
    state.textures[0].linear = 1; state.textures[0].decode = gpu_decode; state.textures[0].decode_context = &texel;
    check("SIMD validation accepts finite texture coordinates", nv2a_gpu_draw(&state, vertices, 3));
    for (uint32_t vertex = 0; vertex < 3; vertex++)
        for (uint32_t lane = 0; lane < 4; lane++) {
            float saved = vertices[vertex].texture[0][lane];
            uint32_t invalid = lane & 1 ? 0x7F800000u : 0x7FC00001u;
            memcpy(&vertices[vertex].texture[0][lane], &invalid, 4);
            check("SIMD validation rejects nonfinite texture coordinate in every lane", !nv2a_gpu_draw(&state, vertices, 3));
            vertices[vertex].texture[0][lane] = saved;
        }
    nv2a_gpu_flush_reason(NV2A_GPU_SYNC_SEMAPHORE);
    check("SIMD texture rejection preserves exact completed output", color[63] == 0xFFFF0000);
    check("SIMD texture rejection preserves exact completed depth and stencil", depth[63] == 0x000032A5);
    nv2a_gpu_invalidate();
}

int main(int argument_count, char **arguments)
{
    TestVertex *vertices = (TestVertex *)((uint8_t *)memory + 0x4000);
    const uint32_t attributes[] = {0, 3, 9, 10};
    uint32_t index, attribute;
    check("DMA resolves harness memory",
          xbox_DmaPhysicalPointer(0x1000, 4) == (uint8_t *)memory + 0x1000);
    check("DMA rejects out-of-bounds span",
          xbox_DmaPhysicalPointer(sizeof memory - 1, 4) == NULL);
    check("DMA rejects overflowing address",
          xbox_DmaPhysicalPointer(UINT64_MAX, 4) == NULL);
    check("register aperture resolves DMA registers",
          xbox_Nv2aRegisterPointer(0x800040, 8) == nv2a_registers + 0x800040 / 4);
    check("register aperture rejects unaligned address",
          xbox_Nv2aRegisterPointer(1, 4) == NULL);
    check("register aperture rejects out-of-bounds span",
          xbox_Nv2aRegisterPointer(sizeof nv2a_registers - 4, 8) == NULL);
    if (argument_count == 3 && !strcmp(arguments[1], "--ghost-effects"))
        return test_ghost_pixel_shaders(arguments[2]);
    if (argument_count == 2 && !strncmp(arguments[1], "--effects", 9))
        return test_effect_commands(strcmp(arguments[1], "--effects") ? arguments[1] : NULL);
    if (argument_count == 2 && !strncmp(arguments[1], "--pb-", 5)) return invalid_pushbuffer_case(arguments[1]);
    if (argument_count == 2 && !strcmp(arguments[1], "--shadow-map")) {
        _putenv_s("RECOMP_GPU_EXPERIMENTAL_DEPTH", "1");
        check("shadow-map D3D11 device", nv2a_gpu_available());
        check("shadow-map shader compilation", nv2a_gpu_compile());
        test_gpu_shadow_far_plane();
        test_gpu_shadow_source();
        printf("nv2a_shadow_map: %s\n", failures ? "FAILED" : "ALL PASS");
        return failures ? 1 : 0;
    }
    if (argument_count == 2 && !strcmp(arguments[1], "--depth-textures")) {
        check("depth texture D3D11 device", nv2a_gpu_available());
        check("depth texture shader compilation", nv2a_gpu_compile());
        test_gpu_depth_textures();
        test_gpu_depth_mips_and_alias();
        test_gpu_structured_shadow_mask();
        test_depth_texture_commands();
        printf("nv2a_depth_textures: %s\n", failures ? "FAILED" : "ALL PASS");
        return failures ? 1 : 0;
    }
    if (getenv("NV2A_GPU_FOUR_TARGETS_ONLY")) {
        _putenv_s("RECOMP_GPU_EXPERIMENTAL_DEPTH", "1");
        check("four-target D3D11 device", nv2a_gpu_available());
        check("four-target shader compilation", nv2a_gpu_compile());
        test_gpu_four_targets();
        printf("nv2a_shader_four_targets: %s\n", failures ? "FAILED" : "ALL PASS");
        return failures ? 1 : 0;
    }
    if (getenv("NV2A_GPU_SIMD_ONLY")) {
        _putenv_s("RECOMP_GPU_EXPERIMENTAL_DEPTH", "1");
        check("SIMD validation D3D11 hardware device", nv2a_gpu_available());
        check("SIMD validation hardware shader compilation", nv2a_gpu_compile());
        test_gpu_depth_transition();
        test_gpu_homogeneous_clipping();
        test_gpu_simd_validation();
        test_gpu_scissor_coverage();
        test_gpu_rejected_outputs();
        printf("nv2a_shader_simd: %s\n", failures ? "FAILED" : "ALL PASS");
        return failures ? 1 : 0;
    }
    if (getenv("NV2A_GPU_RESIDENT_ONLY")) {
        _putenv_s("RECOMP_GPU_EXPERIMENTAL_DEPTH", "1");
        check("resident D3D11 hardware device", nv2a_gpu_available());
        check("resident hardware shader compilation", nv2a_gpu_compile());
        test_gpu_resident();
        printf("nv2a_shader_resident: %s\n", failures ? "FAILED" : "ALL PASS");
        return failures ? 1 : 0;
    }
    if (getenv("NV2A_GPU_VERTEX_PACKED_ONLY")) {
        _putenv_s("RECOMP_GPU_EXPERIMENTAL_DEPTH", "1");
        check("packed vertex D3D11 device", nv2a_gpu_available());
        check("packed vertex shader compilation", nv2a_gpu_compile());
        if (!getenv("NV2A_GPU_VERTEX_PACKED_BENCH_ONLY")) {
            test_gpu_vertex();
            test_gpu_packed_vertices();
        }
        test_gpu_packed_vertex_benchmark();
        test_gpu_indexed_stream_benchmark();
        printf("nv2a_shader_packed_vertex: %s\n", failures ? "FAILED" : "ALL PASS");
        return failures ? 1 : 0;
    }
    if (getenv("NV2A_GPU_TEXTURE_CACHE_ONLY")) {
        check("texture cache D3D11 device", nv2a_gpu_available());
        check("texture cache hardware shader compilation", nv2a_gpu_compile());
        if (!getenv("NV2A_GPU_TEXTURE_CACHE_BENCH_ONLY")) {
            test_gpu_texture_cache();
            test_gpu_texture_cache_dimensions();
            test_gpu_cube();
            test_gpu_dxt();
        }
        test_gpu_texture_cache_benchmark();
        printf("nv2a_shader_texture_cache: %s\n", failures ? "FAILED" : "ALL PASS");
        return failures ? 1 : 0;
    }
    if (getenv("NV2A_GPU_DEPTH_PRECISION_ONLY")) {
        _putenv_s("RECOMP_GPU_EXPERIMENTAL_DEPTH", "1");
        check("depth precision D3D11 device", nv2a_gpu_available());
        check("depth precision hardware shader compilation", nv2a_gpu_compile());
        test_gpu_depth_download_precision();
        nv2a_gpu_report();
        printf("nv2a_shader_depth_precision: %s\n", failures ? "FAILED" : "ALL PASS");
        return failures ? 1 : 0;
    }
    if (getenv("NV2A_GPU_DEPTH_DOWNLOAD_ONLY")) {
        _putenv_s("RECOMP_GPU_EXPERIMENTAL_DEPTH", "1");
        check("depth download D3D11 device", nv2a_gpu_available());
        check("depth download hardware shader compilation", nv2a_gpu_compile());
        if (!getenv("NV2A_GPU_DEPTH_DOWNLOAD_BENCH_ONLY")) {
            test_gpu_depth_minimal();
            test_gpu_scissor_coverage();
            test_gpu_rejected_outputs();
            test_gpu_depth_download_precision();
        }
        test_gpu_occlusion_benchmark();
        test_gpu_publication_benchmark();
        printf("nv2a_shader_depth_download: %s\n", failures ? "FAILED" : "ALL PASS");
        return failures ? 1 : 0;
    }
    if (getenv("NV2A_GPU_PUBLICATION_ONLY")) {
        _putenv_s("RECOMP_GPU_EXPERIMENTAL_DEPTH", "1");
        check("publication D3D11 device", nv2a_gpu_available());
        check("publication hardware shader compilation", nv2a_gpu_compile());
        test_gpu_depth_minimal();
        test_gpu_scissor_coverage();
        test_gpu_rejected_outputs();
        test_gpu_publication_benchmark();
        printf("nv2a_shader_publication: %s\n", failures ? "FAILED" : "ALL PASS");
        return failures ? 1 : 0;
    }
    if (getenv("NV2A_GPU_DEPTH_ONLY")) {
        _putenv_s("RECOMP_GPU_EXPERIMENTAL_DEPTH", "1");
        check("minimal D3D11 device", nv2a_gpu_available());
        test_gpu_depth_minimal();
        printf("nv2a_shader_depth: %s\n", failures ? "FAILED" : "ALL PASS");
        return failures ? 1 : 0;
    }
    if (getenv("NV2A_GPU_PROBE")) {
        _putenv_s("RECOMP_GPU_EXPERIMENTAL_DEPTH", "1");
        check("D3D11 device initialization", nv2a_gpu_available());
        check("hardware shader compilation", nv2a_gpu_compile());
        test_gpu_vertex();
        test_gpu_cube();
        test_gpu_dxt();
        test_gpu_clip_planes();
        test_gpu_dependent_textures();
        test_gpu_dot_textures();
        test_gpu();
        test_gpu_depth_minimal();
        test_gpu_scissor_coverage();
        test_gpu_rejected_outputs();
    }
    _putenv_s("RECOMP_FB_DUMP", "");
    _putenv_s("RECOMP_TEX_DUMP", "");
    _putenv_s("RECOMP_RASTER_TEST", "");
    test_invalid_pushbuffer_packets();
    test_effect_command_processes();
    test_gpu_four_targets();
    test_split_pushbuffer_packet();
    method(0x0200, 8u << 16);
    method(0x0204, 8u << 16);
    method(0x0208, 0x28);
    method(0x020C, 32u | (32u << 16));
    method(0x0210, 0x1000);
    method(0x0214, 0x2000);
    method(0x1D90, 0xFF000000);
    method(0x1D8C, 0xFFFFFF00);
    method(0x1D94, 0xF3);
    method(0x037C, 0x1D01);
    for (attribute = 0; attribute < 4; attribute++) {
        method(0x1720 + attributes[attribute] * 4, 0x4000 + attribute * 16);
        method(0x1760 + attributes[attribute] * 4, (sizeof(TestVertex) << 8) | 0x42);
    }
    for (index = 0; index < 4; index++) {
        vertices[index].position[3] = 1;
        vertices[index].diffuse[0] = 0.5f;
        vertices[index].diffuse[1] = 0.25f;
        vertices[index].diffuse[2] = vertices[index].diffuse[3] = 1;
        vertices[index].texture0[3] = vertices[index].texture1[3] = 1;
    }
    vertices[1].position[0] = 8;
    vertices[2].position[1] = 8;
    upload_program();
    bind_texture(0, 0x3000, 0xFF804020);
    method(0x1E60, 1);
    method(0x1E70, 1);
    method(0x0AC0, 0x08040000);
    method(0x0260, 0x00002014);
    method(0x1E40, 0xC00);
    method(0x0AA0, 0xC00);
    method(0x0288, 0x0000000C);
    method(0x028C, 0x00001C80);
    draw(5, 3);
    check("uploaded vertex program translates geometry", pixel(1, 1) == 0xFF000000);
    check("guest combiner computes lit pixel", pixel(3, 3) == 0xFF401020);
    {
        const uint32_t expected[4] = {0xFF400000, 0xFF401000, 0xFF401020, 0xFF401020};
        for (uint32_t components = 1; components <= 4; components++) {
            method(0x176C, (sizeof(TestVertex) << 8) | (components << 4) | 2);
            method(0x1D94, 0xF0);
            draw(5, 3);
            check("float attribute component defaults match prepared fetch", pixel(3, 3) == expected[components - 1]);
        }
        method(0x1760, (sizeof(TestVertex) << 8) | 0x32);
        method(0x1D94, 0xF0);
        draw(5, 3);
        check("float3 position retains default homogeneous W", pixel(3, 3) == 0xFF401020);
        method(0x1760, (sizeof(TestVertex) << 8) | 0x42);
        method(0x1EA0, 1);
        method(0x1EA0, 0);
        method(0x1D94, 0xF0);
        draw(5, 3);
        check("program start change refreshes attribute dependency mask", pixel(3, 3) == 0xFF401020);
        for (index = 0; index < 3; index++)
            memcpy((uint8_t *)memory + 0x7001 + index * 17, vertices[index].diffuse, 16);
        method(0x172C, 0x7001); method(0x176C, (17u << 8) | 0x42);
        method(0x1D94, 0xF0); draw(5, 3);
        check("unaligned float arrays retain exact output", pixel(3, 3) == 0xFF401020);
        for (index = 0; index < 3; index++) {
            float red = 0.25f;
            memcpy((uint8_t *)memory + 0x7001 + index * 17, &red, 4);
        }
        method(0x1D94, 0xF0); draw(5, 3);
        check("prepared source observes changed vertex bytes", pixel(3, 3) == 0xFF201020);
        method(0x172C, 0x4000 + 16); method(0x176C, (sizeof(TestVertex) << 8) | 0x42);
        method(0x1D94, 0xF0); draw(5, 3);
        method(0x176C, 0);
        float_method(0x1A30, 0.5f); float_method(0x1A34, 0.25f);
        float_method(0x1A38, 1); float_method(0x1A3C, 1);
        method(0x1D94, 0xF0); draw(5, 3);
        check("prepared uniform attributes retain exact output", pixel(3, 3) == 0xFF401020);
        float_method(0x1A30, 0.25f);
        method(0x1D94, 0xF0); draw(5, 3);
        check("prepared uniforms refresh between batches", pixel(3, 3) == 0xFF201020);
        method(0x176C, (sizeof(TestVertex) << 8) | 0x42);
        method(0x1D94, 0xF0); draw(5, 3);
    }
    bind_texture(1, 0x3010, 0xFF202040);
    method(0x1E70, 0x21);
    method(0x0AC0, 0x08090000);
    draw(5, 3);
    check("second guest texture sampled", pixel(3, 3) == 0xFF100808);
    method(0x1E70, 1);
    method(0x0AC0, 0x08040000);
    method(0x030C, 1);
    method(0x0354, 0x0201);
    method(0x035C, 1);
    for (index = 0; index < 3; index++) vertices[index].position[2] = 100;
    draw(5, 3);
    check("depth written to guest surface", (memory[0x2000 / 4 + 3 * 8 + 3] >> 8) == 100);
    for (index = 0; index < 3; index++) {
        vertices[index].position[2] = 200;
        vertices[index].diffuse[0] = 1;
    }
    draw(5, 3);
    check("occluded geometry does not overwrite color", pixel(3, 3) == 0xFF401020);
    method(0x0300, 1);
    method(0x033C, 0x0204);
    method(0x0340, 128);
    for (index = 0; index < 3; index++) {
        vertices[index].position[2] = 50;
        vertices[index].diffuse[3] = 0.25f;
    }
    draw(5, 3);
    check("alpha discard preserves depth", (memory[0x2000 / 4 + 3 * 8 + 3] >> 8) == 100);
    check("alpha discard preserves color", pixel(3, 3) == 0xFF401020);
    method(0x0300, 0);
    method(0x030C, 0);
    method(0x1D94, 0xF0);
    method(0x0304, 1);
    method(0x0344, 0x0302);
    method(0x0348, 0x0303);
    method(0x0350, 0x8006);
    method(0x0AC0, 0x08200000);
    method(0x1EA4, 0);
    float_method(0x0B80, 0); float_method(0x0B84, 0);
    memory[0x3000 / 4] = 0xFFFF0000;
    for (index = 0; index < 4; index++) vertices[index].diffuse[3] = 0.5f;
    vertices[2].position[0] = 8;
    vertices[3].position[1] = 8;
    draw(8, 4);
    check("shared edge blended only once", pixel(3, 3) == 0xBF800000);
    method(0x1D94, 0xF0);
    vertices[1].position[3] = -1;
    draw(8, 4);
    check("mixed-W triangle retains its clipped visible contribution", pixel(3, 5) == 0x9FBF0000);
    check("homogeneous clipping excludes outside fragments", pixel(5, 3) == 0xFF000000);
    vertices[1].position[3] = 1;
    method(0x0304, 0);
    method(0x1D94, 0xF0);
    for (index = 0; index < 3; index++) vertices[index].diffuse[3] = 1;
    vertices[2].position[0] = 0;
    method(0x1E9C, 0);
    method(0x0B00, 0);
    method(0x0B04, (3u << 21) | (59u << 13) | 0x1Bu);
    method(0x0B08, (2u << 26) | (0x1Bu << 2));
    method(0x0B0C, (3u << 28) | 0xF800u);
    float_method(0x0A20, 2); float_method(0x0A24, 2);
    float_method(0x0A28, 0); float_method(0x0A2C, 0);
    draw(5, 3);
    check("viewport methods alias shader constants", pixel(1, 1) == 0xFF000000 && pixel(3, 3) == 0xFFFF0000);
    method(0x1D94, 0xF0);
    method(0x1E9C, 0);
    method(0x0B00, 0);
    method(0x0B04, (3u << 21) | 0x1Bu);
    method(0x0B08, (2u << 26) | (0x1Bu << 2));
    method(0x0B0C, (3u << 28) | 0xF800u);
    float_method(0x0680, 4); float_method(0x0684, 4);
    float_method(0x0688, 0); float_method(0x068C, 0);
    draw(5, 3);
    check("matrix methods alias shader constants", pixel(3, 3) == 0xFF000000 && pixel(5, 5) == 0xFFFF0000);
    method(0x1D94, 0xF0);
    method(0x1E9C, 130);
    method(0x0B00, 0);
    method(0x0B04, (1u << 21) | 0x1Bu);
    method(0x0B08, 2u << 26);
    method(0x0B0C, 0xF001u);
    float_method(0x1E80, 2); float_method(0x1E84, 2);
    float_method(0x1E88, 0); float_method(0x1E8C, 0);
    method(0x1E90, 130);
    draw(5, 3);
    check("state shader writes shared constants", pixel(1, 1) == 0xFF000000 && pixel(3, 3) == 0xFFFF0000);
    method(0x1D94, 0xF0);
    method(0x1E94, 4);
    for (index = 0; index < 16; index++)
        float_method(0x0680 + index * 4, index == 0 || index == 5 ? 80.0f : index == 10 || index == 15 ? 1.0f : 0.0f);
    vertices[1].position[0] = 0.1f;
    vertices[2].position[1] = 0.1f;
    draw(5, 3);
    check("fixed-function object vertices use composite transform", pixel(1, 1) == 0xFF000000 && pixel(3, 3) == 0xFFFF0000);
    method(0x176C, 2);
    method(0x194C, 0xFF804020);
    method(0x0AC0, 0x08040000);
    memory[0x3000 / 4] = 0xFFFFFFFF;
    draw(5, 3);
    check("immediate byte color uses hardware channel order", pixel(3, 3) == 0xFF204080);
    memory[0x1000 / 4 + 48] = 0x12345678;
    method(0x020C, 24u | (32u << 16));
    method(0x1D94, 0xF0);
    draw(5, 3);
    check("surface format is independent of clip-to-pitch ratio", memory[0x1000 / 4 + 3 * 6 + 3] == 0xFF204080);
    check("narrow render target preserves following memory", memory[0x1000 / 4 + 48] == 0x12345678);
    method(0x020C, 32u | (32u << 16));
    if (getenv("NV2A_GPU_EXEC")) {
        for (index = 0; index < 6; index++) memory[0x6000 / 4 + index * 32] = 0xFF804020;
        for (index = 0; index < 3; index++) {
            vertices[index].texture0[0] = 1; vertices[index].texture0[1] = vertices[index].texture0[2] = 0;
        }
        method(0x1B00, 0x6000); method(0x1B04, (6u << 8) | (1u << 16) | 4u);
        method(0x1E70, 3); method(0x0AC0, 0x08200000); method(0x1D94, 0xF0);
        draw(5, 3);
        check("guest cube metadata and aligned face decoding", pixel(3, 3) == 0xFF804020);
        method(0x1E70, 1); method(0x1D94, 0xF0);
        draw(5, 3);
        check("guest projective coordinates select cube storage", pixel(3, 3) == 0xFF804020);
        method(0x1E70, 3);
        method(0x1B04, (6u << 8) | (1u << 16)); method(0x1D94, 0xF0);
        draw(5, 3);
        check("guest cube coordinates accept non-cube storage", pixel(3, 3) == 0xFF804020);
        bind_texture(0, 0x3000, 0xFFFFFFFF); method(0x1E70, 1); method(0x0AC0, 0x08040000);
        method(0x1E60, 0); method(0x0288, 0); method(0x028C, 0);
        method(0x1E70, 5); method(0x17F8, 8); method(0x1D94, 0xF0);
        draw(5, 3);
        check("guest clip register discards nonnegative Q", pixel(3, 3) == 0xFF000000);
        method(0x17F8, 0); method(0x1D94, 0xF0);
        draw(5, 3);
        check("guest clip register mutation restores diffuse draw", pixel(3, 3) == 0xFF204080);
        method(0x1E60, 1); method(0x0288, 0xC); method(0x028C, 0x1C80); method(0x1E70, 1);
        {
            uint32_t texels[16];
            float saved_texture1[3][4];
            const float producer_coordinates[4] = {0.375f,0.625f,0.875f,0.125f};
            for (index = 0; index < 16; index++) texels[index] = 0xFF000000u | ((index + 1) * 0x00090703u);
            xbox_swizzle_rect((uint8_t *)memory + 0x7000, texels, 4, 4, 4);
            for (index = 0; index < 3; index++) {
                memcpy(saved_texture1[index], vertices[index].texture1, sizeof saved_texture1[index]);
                memcpy(vertices[index].texture1, producer_coordinates, sizeof producer_coordinates);
            }
            bind_texture(0, 0x7200, 0xDF205F9F); method(0x1B04, (6u << 8) | (1u << 16) | 0x20u);
            method(0x1B80, 0x7000); method(0x1B84, (6u << 8) | (1u << 16) | (2u << 20) | (2u << 24) | 0x20u);
            method(0x1B88, 0x303); method(0x1B90, 16u << 16);
            method(0x0AC0, 0x0A200000); method(0x0260, 0x0000201A);
            method(0x1E70, 1u | (4u << 5) | (15u << 10)); method(0x1E78, 0); method(0x1D94, 0xF0);
            draw(5, 3);
            check("guest dependent AR consumes sampled stage zero", pixel(3, 3) == texels[3]);
            method(0x1E78, 1u << 16); method(0x1D94, 0xF0);
            draw(5, 3);
            check("guest dependent input register selects stage one", pixel(3, 3) == texels[4]);
            method(0x1E70, 1u | (4u << 5) | (16u << 10)); method(0x1D94, 0xF0);
            draw(5, 3);
            check("guest dependent GB reads green and blue", pixel(3, 3) == texels[14]);
            {
                const uint32_t expected_dot_texels[4] = {13,9,10,6};
                uint32_t mapping;
                float_method(0x1AB0, 0); float_method(0x1AB4, 1); float_method(0x1AB8, 0); float_method(0x1ABC, 0);
                for (index = 0; index < 3; index++) {
                    vertices[index].texture1[0] = 1; vertices[index].texture1[1] = vertices[index].texture1[2] = vertices[index].texture1[3] = 0;
                }
                bind_texture(0, 0x7200, 0xFF40C020); method(0x1B04, (6u << 8) | (1u << 16) | 0x20u);
                method(0x1B88, 0x101); method(0x1E78, 0); method(0x1E70, 1u | (17u << 5) | (9u << 10));
                for (mapping = 0; mapping < 4; mapping++) {
                    method(0x1E74, mapping | (mapping << 4)); method(0x1D94, 0xF0);
                    draw(5, 3);
                    check("guest dot mapping register samples swizzled texture", pixel(3, 3) == texels[expected_dot_texels[mapping]]);
                }
                method(0x1E74, 0); method(0x1E78, 1u << 16); method(0x1D94, 0xF0);
                draw(5, 3);
                check("guest dot source register reads zero DOTPRODUCT output", pixel(3, 3) == texels[1]);
                method(0x1E78, 0); method(0x1E74, 1u << 4); method(0x1D94, 0xF0);
                draw(5, 3);
                check("guest dot mappings retain independent stage fields", pixel(3, 3) == texels[9]);
                method(0x1E74, 0); float_method(0x1AB4, 0); float_method(0x1ABC, 1);
            }
            for (index = 0; index < 3; index++) memcpy(vertices[index].texture1, saved_texture1[index], sizeof saved_texture1[index]);
            bind_texture(0, 0x3000, 0xFFFFFFFF); method(0x1E70, 1); method(0x1E78, 0);
            method(0x0AC0, 0x08040000); method(0x0260, 0x00002014);
        }
    }
    if (getenv("NV2A_SHADER_BENCH")) {
        clock_t started;
        uint32_t checksum = 0;
        method(0x0200, 128u << 16);
        method(0x0204, 128u << 16);
        method(0x020C, 512u | (512u << 16));
        method(0x0210, 0x8000);
        method(0x176C, (sizeof(TestVertex) << 8) | 0x42);
        upload_program();
        method(0x0AC0, 0x08040000);
        method(0x0260, 0x18200000);
        if (getenv("NV2A_SHADER_BENCH_GENERAL")) {
            method(0x1E60, 2);
            method(0x0AC4, 0x0C200000);
            method(0x0264, 0x1C200000);
            method(0x1E44, 0xC00);
            method(0x0AA4, 0xC00);
        }
        method(0x0304, 1);
        method(0x0344, 0x0302);
        method(0x0348, 0x0303);
        memory[0x3000 / 4] = 0x80804020;
        vertices[1].position[0] = 128;
        vertices[2].position[1] = 128;
        for (index = 0; index < 3; index++) {
            vertices[index].diffuse[0] = 0.5f;
            vertices[index].diffuse[1] = 0.25f;
            vertices[index].diffuse[2] = vertices[index].diffuse[3] = 1;
        }
        started = clock();
        for (index = 0; index < 256; index++) draw(5, 3);
        for (index = 0; index < 128 * 128; index++)
            checksum = checksum * 33u + memory[0x8000 / 4 + index];
        check("benchmark preserves menu shader color", memory[0x8000 / 4 + 3 * 128 + 3] == 0x80401020);
        printf("menu shader benchmark: %.3f ms checksum %08X\n",
                1000.0 * (double)(clock() - started) / CLOCKS_PER_SEC, checksum);
        method(0x0304, 0);
        method(0x1E60, 1);
        method(0x0260, 0x18140000);
        draw(5, 3);
        check("affine modulation preserves texture times diffuse alpha", memory[0x8000 / 4 + 3 * 128 + 3] == 0x80401020);
        method(0x0260, 0x14200000);
        draw(5, 3);
        check("affine modulation preserves diffuse-only alpha", memory[0x8000 / 4 + 3 * 128 + 3] == 0xFF401020);
    }
    nv2a_pb_exec_report();
    printf("nv2a_shader_smoke: %s\n", failures ? "FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}
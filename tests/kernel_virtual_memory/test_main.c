#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <string.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }
extern recomp_func_t recomp_lookup_kernel(uint32_t va);
extern RECOMP_TLS uint32_t g_eax, g_esp;
extern ptrdiff_t g_xbox_mem_offset;

static int failures;
static uint8_t *memory;
static uint32_t output_base, output_size;
static uint32_t allocate, release;

static void check(bool passed, const char *name)
{
    if (!passed) {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

static uint32_t invoke(uint32_t target, const uint32_t *args, unsigned count)
{
    uint32_t *stack = (uint32_t *)(memory + 0x20000);
    stack[0] = 0xBEEF0001;
    memcpy(stack + 1, args, count * sizeof(*args));
    stack[count + 1] = 0xABCDEF01;
    g_esp = 0x20000;
    recomp_func_t fn = recomp_lookup_kernel(target);
    if (!fn) {
        check(false, "kernel thunk resolves immediately before dispatch");
        return 0xC000000Du;
    }
    fn();
    check(g_esp == 0x20000 + (count + 1) * 4, "stdcall pops exactly the guest arguments");
    check(stack[count + 1] == 0xABCDEF01, "stdcall preserves the caller's next stack word");
    return g_eax;
}

static uint32_t vm_call(uint32_t target, uint32_t base, uint32_t size, uint32_t type)
{
    uint32_t *fields = (uint32_t *)(memory + 0x21000);
    fields[0] = base;
    fields[1] = 0x00F7D410;
    fields[2] = size;
    fields[3] = 0x00F7E218;
    const uint32_t alloc_args[] = {0x21000, 0, 0x21008, type, PAGE_READWRITE};
    const uint32_t free_args[] = {0x21000, 0x21008, type};
    uint32_t status = invoke(target, target == allocate ? alloc_args : free_args,
                             target == allocate ? 5 : 3);
    check(fields[1] == 0x00F7D410 && fields[3] == 0x00F7E218,
          "guest IN/OUT fields stay 32-bit and preserve adjacent words");
    output_base = fields[0];
    output_size = fields[2];
    return status;
}

int main(void)
{
    uint32_t header[0x1000 / 4] = {0};
    header[0] = 0x48454258;
    header[0x104 / 4] = 0x10000;
    header[0x108 / 4] = sizeof(header);
    check(xbox_MemoryLayoutInit(header, sizeof(header)), "synthetic guest memory initializes");
    if (failures) return 1;
    g_xbox_mem_offset = xbox_GetMemoryOffset();
    memory = (uint8_t *)g_xbox_mem_offset;
    uint32_t *thunks = (uint32_t *)(memory + 0x30000);
    thunks[0] = 0x80000000u | 184u;
    thunks[1] = 0x80000000u | 199u;
    xbox_kernel_set_thunk_address(0x30000, 2);
    xbox_kernel_bridge_init();
    allocate = thunks[0];
    release = thunks[1];
    bool resolved = recomp_lookup_kernel(allocate) != NULL && recomp_lookup_kernel(release) != NULL;
    check(resolved, "both memory ordinals dispatch through the bridge");
    if (!resolved) {
        xbox_MemoryLayoutShutdown();
        return 1;
    }

    check(vm_call(allocate, 0, 0x10000, MEM_RESERVE | MEM_COMMIT) == 0,
          "reserve/commit returns a guest heap allocation");
    uint32_t base = output_base;
    if (!base) {
        xbox_MemoryLayoutShutdown();
        return 1;
    }
    memset(memory + base, 0x5A, 0x10000);
    check(vm_call(release, base + 0x1003, 1, MEM_DECOMMIT) == 0,
          "decommit handles guest storage rather than native VirtualFree");
    check(output_base == base + 0x1000 && output_size == 0x1000,
          "decommit rounds and reports the affected guest page");
    bool zero = true;
    for (unsigned i = 0; i < 0x1000; i++) zero &= memory[base + 0x1000 + i] == 0;
    check(zero && memory[base + 0xFFF] == 0x5A && memory[base + 0x2000] == 0x5A,
          "decommit clears only its rounded range");
    check(xbox_HeapBlockSize(base) >= 0x10000, "decommit retains the reservation");
    check(vm_call(release, base + 0xF000, 0x2000, MEM_DECOMMIT) != 0 &&
          output_base == base + 0xF000 && output_size == 0x2000,
          "out-of-range decommit fails without modifying IN/OUT fields");
    check(vm_call(release, base, 0, MEM_DECOMMIT | MEM_RELEASE) != 0 &&
          xbox_HeapBlockSize(base) >= 0x10000,
          "combined free types fail without releasing the live reservation");
    check(vm_call(allocate, 0, 0x1000, MEM_RESERVE | MEM_COMMIT) == 0,
          "another allocation succeeds after decommit");
    uint32_t other = output_base;
    check(other < base || other >= base + 0x10000, "decommitted storage is not reused by another owner");
    check(vm_call(release, other, 0, MEM_RELEASE) == 0, "whole release returns storage to the guest heap");

    check(vm_call(release, base + 0x8000, 0x2000, MEM_RELEASE) == 0,
          "Xbox partial release splits the allocation");
    check(xbox_HeapBlockSize(base + 0x8000) == 0 &&
          xbox_HeapBlockSize(base) >= 0x8000 &&
          xbox_HeapBlockSize(base + 0xA000) >= 0x6000,
          "partial release preserves both live sides without retaining the hole");
    check(vm_call(release, base + 0xA000, 0, MEM_RELEASE) == 0, "release frees the right-hand fragment");
    check(vm_call(release, base, 0, MEM_RELEASE) == 0, "release frees the left-hand fragment");
    check(vm_call(release, base, 0, MEM_RELEASE) != 0 && output_base == base && output_size == 0,
          "double release fails without changing guest fields");
    check(vm_call(allocate, 0, 123, MEM_RESERVE | MEM_COMMIT) == 0 && output_size == 0x1000,
          "small virtual allocations report page-rounded sizes");
    uint32_t small = output_base;
    check(xbox_HeapBlockSize(small) == 0x1000,
          "reused larger blocks return their unused tail to the allocator");
    check(vm_call(release, small, 0, MEM_DECOMMIT) == 0 && xbox_HeapBlockSize(small) != 0,
          "zero-size decommit keeps the whole allocation reserved");
    check(vm_call(allocate, small, 0x1000, MEM_COMMIT) == 0 && output_base == small,
          "decommitted storage can be recommitted at the same guest address");
    check(vm_call(release, small, 0, MEM_RELEASE) == 0,
          "recommitted storage can subsequently be released");
    check(vm_call(allocate, 0, UINT32_MAX, MEM_RESERVE | MEM_COMMIT) != 0 &&
          output_base == 0 && output_size == UINT32_MAX,
          "overflowing page rounding fails without corrupting IN/OUT fields");
    check(vm_call(allocate, 0, 0xFFFF0000, MEM_RESERVE | MEM_COMMIT) != 0 &&
          output_base == 0 && output_size == 0xFFFF0000,
          "oversized heap requests cannot wrap the allocation bounds check");

    uint32_t reused = 0;
    for (unsigned iteration = 0; iteration < 16; iteration++) {
        uint32_t status = vm_call(allocate, 0, 8 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT);
        check(status == 0, "repeated map-sized allocation succeeds");
        if (status) break;
        uint32_t current = output_base;
        check(output_size == 8 * 1024 * 1024 &&
              xbox_HeapBlockSize(current) == output_size,
              "map-sized reservations own exactly their reported extent");
        if (!iteration) reused = current;
        check(current <= reused, "map-sized allocations reuse storage without advancing the address");
        reused = current;
        status = vm_call(release, current, 0, MEM_RELEASE);
        check(status == 0 && xbox_HeapBlockSize(current) == 0, "map-sized release actually reclaims storage");
        if (status) break;
    }
    xbox_MemoryLayoutShutdown();
    printf("kernel_virtual_memory: %s\n", failures ? "FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}

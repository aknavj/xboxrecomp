#include "nv2a_gpu_memory.h"
#include "nv2a_gpu.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
constexpr size_t page_bytes = 4096;
struct View {
    uint8_t *base;
    size_t bytes;
    std::vector<DWORD> protection;
};
struct Mapping {
    uint8_t *publication;
    size_t bytes;
    std::vector<View> views;
    std::vector<uint8_t> protected_pages;
    size_t protected_count = 0;
    uint64_t protection_epoch = 0;
};
std::vector<Mapping> mappings;
uint64_t protection_epoch;
SRWLOCK memory_lock = SRWLOCK_INIT;
CONDITION_VARIABLE published = CONDITION_VARIABLE_INIT;
volatile LONG active, pending, owner, stopping;
LONG handlers;
void *exception_handler;

[[noreturn]] void memory_failed(const char *operation)
{
    DWORD error = GetLastError();
    std::fprintf(stderr, "[GPU-D3D11] guest-memory coherence %s failed: %lu\n", operation, error);
    std::fflush(stderr);
    std::_Exit(EXIT_FAILURE);
}

Mapping *find_mapping(const void *memory, size_t bytes, size_t &offset, View **matched = nullptr)
{
    uintptr_t address = reinterpret_cast<uintptr_t>(memory);
    static thread_local size_t recent_mapping, recent_view;
    auto contains = [&](View &view) {
        uintptr_t base = reinterpret_cast<uintptr_t>(view.base);
        if (address >= base && address - base < view.bytes && bytes <= view.bytes - (address - base)) {
            offset = address - base;
            if (matched) *matched = &view;
            return true;
        }
        return false;
    };
    if (recent_mapping < mappings.size()) {
        auto &mapping = mappings[recent_mapping];
        if (recent_view < mapping.views.size() && contains(mapping.views[recent_view]))
            return &mapping;
    }
    for (size_t index = 0; index < mappings.size(); index++) {
        auto &mapping = mappings[index];
        for (size_t view = 0; view < mapping.views.size(); view++) {
            if (contains(mapping.views[view])) {
                recent_mapping = index;
                recent_view = view;
                return &mapping;
            }
        }
    }
    return nullptr;
}

size_t query_view_region(const View &view, size_t page, MEMORY_BASIC_INFORMATION &information, const char *operation)
{
    const uint8_t *address = view.base + page * page_bytes;
    if (!VirtualQuery(address, &information, sizeof information)) memory_failed(operation);
    uintptr_t current = reinterpret_cast<uintptr_t>(address);
    uintptr_t base = reinterpret_cast<uintptr_t>(information.BaseAddress);
    if (base > current || current - base >= information.RegionSize) {
        SetLastError(ERROR_INVALID_DATA);
        memory_failed(operation);
    }
    size_t remaining = std::min(view.bytes - page * page_bytes, information.RegionSize - (current - base));
    size_t end = page + remaining / page_bytes;
    if (end <= page) {
        SetLastError(ERROR_INVALID_DATA);
        memory_failed(operation);
    }
    return end;
}

bool protected_address(const void *address)
{
    size_t offset;
    auto *mapping = find_mapping(address, 1, offset);
    return mapping && mapping->protected_pages[offset / page_bytes] != 0;
}

bool any_protected()
{
    return std::any_of(mappings.begin(), mappings.end(), [](const Mapping &mapping) {
        return mapping.protected_count != 0;
    });
}

void publish_cpu_memory()
{
    if (GetCurrentThreadId() == static_cast<DWORD>(InterlockedCompareExchange(&owner, 0, 0))) {
        nv2a_gpu_flush_reason(NV2A_GPU_SYNC_CPU_ACCESS);
        return;
    }
    AcquireSRWLockExclusive(&memory_lock);
    InterlockedExchange(&pending, 1);
    while (nv2a_gpu_memory_pending())
        if (!SleepConditionVariableSRW(&published, &memory_lock, INFINITE, 0))
            memory_failed("wait for CPU access");
    ReleaseSRWLockExclusive(&memory_lock);
}

LONG CALLBACK memory_exception(EXCEPTION_POINTERS *exception)
{
    if (!InterlockedCompareExchange(&active, 0, 0) ||
        exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        exception->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    const void *address = reinterpret_cast<const void *>(exception->ExceptionRecord->ExceptionInformation[1]);
    AcquireSRWLockExclusive(&memory_lock);
    if (!protected_address(address)) {
        ReleaseSRWLockExclusive(&memory_lock);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    handlers++;
    ReleaseSRWLockExclusive(&memory_lock);
    if (GetCurrentThreadId() == static_cast<DWORD>(InterlockedCompareExchange(&owner, 0, 0)))
        nv2a_gpu_flush_reason(NV2A_GPU_SYNC_CPU_ACCESS);
    else {
        AcquireSRWLockExclusive(&memory_lock);
        while (protected_address(address)) {
            InterlockedExchange(&pending, 1);
            if (!SleepConditionVariableSRW(&published, &memory_lock, INFINITE, 0))
                memory_failed("wait for publication");
        }
        ReleaseSRWLockExclusive(&memory_lock);
    }
    AcquireSRWLockExclusive(&memory_lock);
    handlers--;
    WakeAllConditionVariable(&published);
    ReleaseSRWLockExclusive(&memory_lock);
    return EXCEPTION_CONTINUE_EXECUTION;
}
}

extern "C" int nv2a_gpu_memory_initialize(void)
{
    const char *value = std::getenv("RECOMP_NV2A_GPU_RESIDENT");
    if (!value || !*value || std::strcmp(value, "0") == 0) return 0;
    if (nv2a_gpu_memory_active()) {
        SetLastError(ERROR_ALREADY_INITIALIZED);
        memory_failed("initialize twice");
    }
    exception_handler = AddVectoredExceptionHandler(1, memory_exception);
    if (!exception_handler) memory_failed("install access handler");
    InterlockedExchange(&stopping, 0);
    InterlockedExchange(&active, 1);
    return 1;
}

extern "C" int nv2a_gpu_memory_add_mapping(void *section, size_t bytes,
                                         const Nv2aGpuMemoryView *views, uint32_t count)
{
    if (!nv2a_gpu_memory_active()) return 0;
    if (!section || !bytes || bytes % page_bytes || !views || !count) {
        SetLastError(ERROR_INVALID_PARAMETER);
        memory_failed("register mapping");
    }
    Mapping mapping = {};
    mapping.publication = static_cast<uint8_t *>(MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, bytes));
    if (!mapping.publication) memory_failed("map private publication view");
    mapping.bytes = bytes;
    mapping.protected_pages.resize(bytes / page_bytes);
    for (uint32_t index = 0; index < count; index++) {
        if (!views[index].base || !views[index].bytes || views[index].bytes > bytes ||
            (!index && views[index].bytes != bytes) ||
            views[index].bytes % page_bytes) {
            SetLastError(ERROR_INVALID_PARAMETER);
            memory_failed("register CPU alias");
        }
        View view = {};
        view.base = static_cast<uint8_t *>(views[index].base);
        view.bytes = views[index].bytes;
        view.protection.resize(view.bytes / page_bytes);
        mapping.views.push_back(std::move(view));
    }
    AcquireSRWLockExclusive(&memory_lock);
    if (any_protected()) {
        SetLastError(ERROR_BUSY);
        memory_failed("register mapping during residency");
    }
    mapping.protection_epoch = ++protection_epoch;
    mappings.push_back(std::move(mapping));
    ReleaseSRWLockExclusive(&memory_lock);
    return 1;
}

extern "C" void nv2a_gpu_memory_set_thread(void)
{
    InterlockedExchange(&owner, static_cast<LONG>(GetCurrentThreadId()));
}

extern "C" int nv2a_gpu_memory_active(void)
{
    return InterlockedCompareExchange(&active, 0, 0) != 0;
}

extern "C" int nv2a_gpu_memory_pending(void)
{
    return InterlockedCompareExchange(&pending, 0, 0) != 0;
}

extern "C" void nv2a_gpu_memory_service(void)
{
    if (nv2a_gpu_memory_pending() &&
        GetCurrentThreadId() == static_cast<DWORD>(InterlockedCompareExchange(&owner, 0, 0)))
        nv2a_gpu_flush_reason(NV2A_GPU_SYNC_CPU_ACCESS);
}

extern "C" int nv2a_gpu_memory_virtual_protect(void *memory, size_t bytes, uint32_t protection, void *previous)
{
    if (!nv2a_gpu_memory_active()) return VirtualProtect(memory, bytes, protection, static_cast<DWORD *>(previous));
    for (;;) {
        AcquireSRWLockExclusive(&memory_lock);
        if (!any_protected()) {
            BOOL result = VirtualProtect(memory, bytes, protection, static_cast<DWORD *>(previous));
            ReleaseSRWLockExclusive(&memory_lock);
            return result;
        }
        ReleaseSRWLockExclusive(&memory_lock);
        publish_cpu_memory();
    }
}

extern "C" size_t nv2a_gpu_memory_virtual_query(const void *memory, void *information, size_t bytes)
{
    if (!nv2a_gpu_memory_active()) return VirtualQuery(memory, static_cast<MEMORY_BASIC_INFORMATION *>(information), bytes);
    for (;;) {
        AcquireSRWLockExclusive(&memory_lock);
        if (!any_protected()) {
            SIZE_T result = VirtualQuery(memory, static_cast<MEMORY_BASIC_INFORMATION *>(information), bytes);
            ReleaseSRWLockExclusive(&memory_lock);
            return result;
        }
        ReleaseSRWLockExclusive(&memory_lock);
        publish_cpu_memory();
    }
}

extern "C" int nv2a_gpu_memory_can_reside(const void *memory, size_t bytes)
{
    if (!nv2a_gpu_memory_active() || InterlockedCompareExchange(&stopping, 0, 0) || !bytes ||
        GetCurrentThreadId() != static_cast<DWORD>(InterlockedCompareExchange(&owner, 0, 0))) return 0;
    AcquireSRWLockShared(&memory_lock);
    size_t offset;
    View *view = nullptr;
    Mapping *mapping = find_mapping(memory, bytes, offset, &view);
    bool supported = mapping != nullptr;
    if (mapping) {
        struct Eligibility {
            const void *memory;
            size_t bytes;
            uint64_t epoch;
        };
        static thread_local std::array<Eligibility, 8> recent = {};
        static thread_local size_t next;
        for (const auto &entry : recent)
            if (entry.epoch == mapping->protection_epoch && entry.memory == memory && entry.bytes == bytes) {
                ReleaseSRWLockShared(&memory_lock);
                return 1;
            }
        bool all_protected = true;
        size_t first = offset / page_bytes, end = (offset + bytes + page_bytes - 1) / page_bytes;
        for (size_t page = first; page < end;) {
            DWORD protection = view->protection[page];
            size_t stop = page + 1;
            if (!mapping->protected_pages[page]) {
                all_protected = false;
                MEMORY_BASIC_INFORMATION information = {};
                stop = std::min(end, query_view_region(*view, page, information, "query target protection"));
                protection = information.Protect;
            }
            if (protection != PAGE_READWRITE && protection != PAGE_EXECUTE_READWRITE &&
                protection != PAGE_WRITECOPY && protection != PAGE_EXECUTE_WRITECOPY) {
                supported = false;
                break;
            }
            page = stop;
        }
        // Saved permissions cannot change until publication removes the guards.
        if (supported && all_protected) {
            recent[next] = {memory, bytes, mapping->protection_epoch};
            next = (next + 1) % recent.size();
        }
    }
    ReleaseSRWLockShared(&memory_lock);
    return supported;
}

extern "C" int nv2a_gpu_memory_protect(void *memory, size_t bytes)
{
    AcquireSRWLockExclusive(&memory_lock);
    size_t offset;
    Mapping *mapping = find_mapping(memory, bytes, offset);
    if (!mapping) {
        ReleaseSRWLockExclusive(&memory_lock);
        return 0;
    }
    size_t first = offset / page_bytes, end = (offset + bytes + page_bytes - 1) / page_bytes;
    if (std::all_of(mapping->protected_pages.begin() + first, mapping->protected_pages.begin() + end,
                    [](uint8_t protected_page) { return protected_page != 0; })) {
        ReleaseSRWLockExclusive(&memory_lock);
        return 1;
    }
    for (auto &view : mapping->views) {
        size_t limit = std::min(end, view.bytes / page_bytes), page = first;
        while (page < limit) {
            if (mapping->protected_pages[page]) { page++; continue; }
            MEMORY_BASIC_INFORMATION information = {};
            size_t region_end = query_view_region(view, page, information, "query alias protection");
            size_t stop = std::min(limit, region_end);
            size_t run = page + 1;
            while (run < stop && !mapping->protected_pages[run]) run++;
            DWORD previous;
            if (!VirtualProtect(view.base + page * page_bytes, (run - page) * page_bytes, PAGE_NOACCESS, &previous))
                memory_failed("protect CPU aliases");
            std::fill(view.protection.begin() + page, view.protection.begin() + run, previous);
            page = run;
        }
    }
    for (size_t page = first; page < end; page++)
        if (!mapping->protected_pages[page]) {
            mapping->protected_pages[page] = 1;
            mapping->protected_count++;
        }
    ReleaseSRWLockExclusive(&memory_lock);
    return 1;
}

extern "C" int nv2a_gpu_memory_is_protected(const void *memory)
{
    AcquireSRWLockShared(&memory_lock);
    bool result = protected_address(memory);
    ReleaseSRWLockShared(&memory_lock);
    return result;
}

extern "C" uint8_t *nv2a_gpu_memory_canonical(const void *memory, size_t bytes)
{
    AcquireSRWLockShared(&memory_lock);
    size_t offset;
    Mapping *mapping = find_mapping(memory, bytes, offset);
    uint8_t *result = mapping ? mapping->views.front().base + offset : nullptr;
    ReleaseSRWLockShared(&memory_lock);
    return result;
}

extern "C" uint8_t *nv2a_gpu_memory_write_pointer(void *memory, size_t bytes)
{
    AcquireSRWLockShared(&memory_lock);
    size_t offset;
    Mapping *mapping = find_mapping(memory, bytes, offset);
    uint8_t *result = static_cast<uint8_t *>(memory);
    if (mapping) {
        size_t first = offset / page_bytes, end = (offset + bytes + page_bytes - 1) / page_bytes;
        for (size_t page = first; page < end; page++)
            if (mapping->protected_pages[page]) { result = mapping->publication + offset; break; }
    }
    ReleaseSRWLockShared(&memory_lock);
    return result;
}

extern "C" void nv2a_gpu_memory_release(void)
{
    AcquireSRWLockExclusive(&memory_lock);
    for (auto &mapping : mappings) {
        for (auto &view : mapping.views) {
            for (size_t page = 0; page < view.protection.size();) {
                DWORD protection = view.protection[page];
                if (!protection) { page++; continue; }
                size_t end = page + 1;
                while (end < view.protection.size() && view.protection[end] == protection) end++;
                DWORD previous;
                if (!VirtualProtect(view.base + page * page_bytes, (end - page) * page_bytes, protection, &previous))
                    memory_failed("restore CPU aliases");
                std::fill(view.protection.begin() + page, view.protection.begin() + end, 0);
                page = end;
            }
        }
        std::fill(mapping.protected_pages.begin(), mapping.protected_pages.end(), 0);
        mapping.protected_count = 0;
        mapping.protection_epoch = ++protection_epoch;
    }
    InterlockedExchange(&pending, 0);
    WakeAllConditionVariable(&published);
    ReleaseSRWLockExclusive(&memory_lock);
}

extern "C" void nv2a_gpu_memory_shutdown(void)
{
    if (!nv2a_gpu_memory_active()) return;
    // Keep access handling alive while draining, but never create new residency.
    InterlockedExchange(&stopping, 1);
    if (GetCurrentThreadId() == static_cast<DWORD>(InterlockedCompareExchange(&owner, 0, 0)))
        nv2a_gpu_flush_reason(NV2A_GPU_SYNC_CPU_ACCESS);
    else {
        AcquireSRWLockExclusive(&memory_lock);
        InterlockedExchange(&pending, 1);
        while (nv2a_gpu_memory_pending())
            if (!SleepConditionVariableSRW(&published, &memory_lock, INFINITE, 0))
                memory_failed("wait for shutdown publication");
        ReleaseSRWLockExclusive(&memory_lock);
    }
    AcquireSRWLockExclusive(&memory_lock);
    InterlockedExchange(&active, 0);
    while (handlers)
        if (!SleepConditionVariableSRW(&published, &memory_lock, INFINITE, 0))
            memory_failed("wait for access handlers");
    if (!RemoveVectoredExceptionHandler(exception_handler)) memory_failed("remove access handler");
    exception_handler = nullptr;
    for (auto &mapping : mappings)
        if (!UnmapViewOfFile(mapping.publication)) memory_failed("unmap publication view");
    mappings.clear();
    ReleaseSRWLockExclusive(&memory_lock);
}

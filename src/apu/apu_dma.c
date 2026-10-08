#include "apu_state.h"

static void bind_storage(MCPXAPUState *d, MCPXAPUDmaBinding *binding, uint64_t physical)
{
    uint32_t offset = (uint32_t)(physical & 0x03FFFFFF);
    if (offset >= d->ram_size) {
        fprintf(stderr, "[APU] DMA binding outside RAM: 0x%llX\n",
                (unsigned long long)physical);
        abort();
    }
    if (!d->physical_mapper && !d->ram_ptr) {
        fprintf(stderr, "[APU] DMA binding has no RAM backing\n");
        abort();
    }
    uint8_t *storage = d->physical_mapper
        ? d->physical_mapper(physical, 1) : d->ram_ptr + offset;
    if (!storage) {
        fprintf(stderr, "[APU] cannot bind DMA storage: 0x%llX\n",
                (unsigned long long)physical);
        abort();
    }
    binding->physical = physical;
    binding->storage = storage;
    binding->available = d->ram_size - offset;
}

uint8_t *mcpx_apu_dma_pointer(const MCPXAPUDmaBinding *binding,
                            uint64_t offset, uint32_t bytes)
{
    if (!binding->storage || offset > binding->available ||
        bytes > binding->available - offset) {
        fprintf(stderr, "[APU] bound DMA extent outside RAM: 0x%llX + %llu + %u\n",
                (unsigned long long)binding->physical,
                (unsigned long long)offset, bytes);
        abort();
    }
    return binding->storage + offset;
}

uint8_t *mcpx_apu_dma_table_pointer(MCPXAPUState *d, MCPXAPUDmaBinding *binding,
                                  uint64_t physical, uint64_t offset, uint32_t bytes)
{
    if (!binding->storage || binding->physical != physical)
        bind_storage(d, binding, physical);
    return mcpx_apu_dma_pointer(binding, offset, bytes);
}

void mcpx_apu_dma_reset_table(MCPXAPUDmaTable *table)
{
    memset(&table->base, 0, sizeof(table->base));
    if (table->entries)
        memset(table->entries, 0, (size_t)table->capacity * sizeof(*table->entries));
}

void mcpx_apu_dma_free_table(MCPXAPUDmaTable *table)
{
    free(table->entries);
    memset(table, 0, sizeof(*table));
}

static MCPXAPUDmaBinding *entry_slot(MCPXAPUState *d, MCPXAPUDmaTable *table,
                                    uint64_t physical, uint32_t entry)
{
    if (!table->base.storage || table->base.physical != physical) {
        mcpx_apu_dma_reset_table(table);
        bind_storage(d, &table->base, physical);
    }
    mcpx_apu_dma_pointer(&table->base, (uint64_t)entry * NV_PSGE_SIZE, NV_PSGE_SIZE);
    if (entry >= table->capacity) {
        uint32_t maximum = table->base.available / NV_PSGE_SIZE;
        uint32_t capacity = table->capacity ? table->capacity : 16;
        if (capacity > maximum) capacity = maximum;
        while (capacity <= entry) {
            if (capacity > maximum / 2) {
                capacity = maximum;
                break;
            }
            capacity *= 2;
        }
        MCPXAPUDmaBinding *entries =
            realloc(table->entries, (size_t)capacity * sizeof(*entries));
        if (!entries) {
            fprintf(stderr, "[APU] cannot allocate %u DMA descriptor bindings\n", capacity);
            abort();
        }
        memset(entries + table->capacity, 0,
               (size_t)(capacity - table->capacity) * sizeof(*entries));
        table->entries = entries;
        table->capacity = capacity;
    }
    return &table->entries[entry];
}

uint8_t *mcpx_apu_dma_descriptor(MCPXAPUState *d, MCPXAPUDmaTable *table,
                                uint64_t physical, uint32_t entry)
{
    entry_slot(d, table, physical, entry);
    return mcpx_apu_dma_pointer(&table->base, (uint64_t)entry * NV_PSGE_SIZE, NV_PSGE_SIZE);
}

MCPXAPUDmaBinding *mcpx_apu_dma_entry(MCPXAPUState *d, MCPXAPUDmaTable *table,
                                    uint64_t physical, uint32_t entry)
{
    MCPXAPUDmaBinding *binding = entry_slot(d, table, physical, entry);
    uint32_t payload = ldl_le_p(mcpx_apu_dma_pointer(
        &table->base, (uint64_t)entry * NV_PSGE_SIZE, 4));
    if (table->page_aligned) payload &= 0xFFFFF000u;
    if (!binding->storage || binding->physical != payload)
        bind_storage(d, binding, payload);
    return binding;
}

void mcpx_apu_dma_program_entry(MCPXAPUState *d, MCPXAPUDmaTable *table,
                               uint64_t physical, uint32_t entry, uint32_t payload)
{
    MCPXAPUDmaBinding *binding = entry_slot(d, table, physical, entry);
    stl_le_p(mcpx_apu_dma_pointer(&table->base, (uint64_t)entry * NV_PSGE_SIZE, 4), payload);
    /* Rewriting the same physical value can intentionally select a new backing bank. */
    bind_storage(d, binding, payload);
}

MCPXAPUDmaBinding mcpx_apu_dma_sge_address(MCPXAPUState *d, MCPXAPUDmaTable *table,
                                        uint64_t physical, uint64_t linear)
{
    uint64_t entry = linear / TARGET_PAGE_SIZE;
    if (entry > UINT32_MAX) {
        fprintf(stderr, "[APU] SGE index overflow: %llu\n", (unsigned long long)entry);
        abort();
    }
    uint32_t offset = (uint32_t)(linear % TARGET_PAGE_SIZE);
    MCPXAPUDmaBinding *binding = mcpx_apu_dma_entry(d, table, physical, (uint32_t)entry);
    MCPXAPUDmaBinding result = *binding;
    result.storage = mcpx_apu_dma_pointer(binding, offset, 1);
    result.physical += offset;
    result.available = MIN(binding->available - offset, TARGET_PAGE_SIZE - offset);
    return result;
}

void mcpx_apu_dma_sge_read(MCPXAPUState *d, MCPXAPUDmaTable *table,
                         uint64_t physical, uint64_t linear, void *output, uint32_t bytes)
{
    uint8_t *destination = output;
    while (bytes) {
        MCPXAPUDmaBinding source = mcpx_apu_dma_sge_address(d, table, physical, linear);
        uint32_t count = MIN(bytes, source.available);
        memcpy(destination, source.storage, count);
        destination += count;
        linear += count;
        bytes -= count;
    }
}

void mcpx_apu_dma_register_write(MCPXAPUState *d, hwaddr reg)
{
    MCPXAPUDmaTable *table = NULL;
    MCPXAPUDmaBinding *binding = NULL;
    switch (reg) {
    case NV_PAPU_VPVADDR: binding = &d->vp.voice_table; break;
    case NV_PAPU_FENADDR: binding = &d->vp.notify_table; break;
    case NV_PAPU_FEMEMADDR: binding = &d->frontend_memory; break;
    case NV_PAPU_VPSGEADDR: table = &d->vp.sge_table; break;
    case NV_PAPU_VPSSLADDR: table = &d->vp.ssl_table; break;
    case NV_PAPU_GPSADDR: table = &d->gp.scratch_table; break;
    case NV_PAPU_EPSADDR: table = &d->ep.scratch_table; break;
    default:
        fprintf(stderr, "[APU] unsupported DMA binding register: 0x%llX\n",
                (unsigned long long)reg);
        abort();
    }
    if (table) {
        mcpx_apu_dma_reset_table(table);
        binding = &table->base;
    }
    bind_storage(d, binding, qatomic_read(&d->regs[reg]));
}

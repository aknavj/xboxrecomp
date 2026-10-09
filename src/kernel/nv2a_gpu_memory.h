#ifndef NV2A_GPU_MEMORY_H
#define NV2A_GPU_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Nv2aGpuMemoryView {
    void *base;
    size_t bytes;
} Nv2aGpuMemoryView;

int nv2a_gpu_memory_initialize(void);
int nv2a_gpu_memory_add_mapping(void *section, size_t bytes, const Nv2aGpuMemoryView *views, uint32_t count);
void nv2a_gpu_memory_set_thread(void);
int nv2a_gpu_memory_active(void);
int nv2a_gpu_memory_pending(void);
void nv2a_gpu_memory_service(void);
int nv2a_gpu_memory_virtual_protect(void *memory, size_t bytes, uint32_t protection, void *previous);
size_t nv2a_gpu_memory_virtual_query(const void *memory, void *information, size_t bytes);
int nv2a_gpu_memory_can_reside(const void *memory, size_t bytes);
int nv2a_gpu_memory_protect(void *memory, size_t bytes);
int nv2a_gpu_memory_is_protected(const void *memory);
uint8_t *nv2a_gpu_memory_canonical(const void *memory, size_t bytes);
uint8_t *nv2a_gpu_memory_write_pointer(void *memory, size_t bytes);
void nv2a_gpu_memory_release(void);
void nv2a_gpu_memory_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

#ifndef XBOX_FB_PRESENT_D3D11_H
#define XBOX_FB_PRESENT_D3D11_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FbGpuPresenter FbGpuPresenter;
FbGpuPresenter *fb_gpu_create(void *window, uint32_t source_width, uint32_t source_height,
                              uint32_t client_width, uint32_t client_height);
int fb_gpu_draw(FbGpuPresenter *presenter, const uint32_t *pixels,
                uint32_t client_width, uint32_t client_height, int upload);
void fb_gpu_destroy(FbGpuPresenter *presenter);

#ifdef __cplusplus
}
#endif

#endif

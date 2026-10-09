#ifndef XBOX_FB_PRESENT_H
#define XBOX_FB_PRESENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void xbox_FramebufferStatsReport(void);
void xbox_FramebufferPresentationCounts(uint64_t *source_updates, uint64_t *presents);

#ifdef __cplusplus
}
#endif

#endif

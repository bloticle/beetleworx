#pragma once
#include "ppc_context.h"
namespace em2 {
enum class ShadowResizeStatus {Idle,Queued,Applied,MemoryLimit,GpuWaitFailed,Unsupported,CreationFailed,NativeSlotLimit};
void QueueShadowResize();
ShadowResizeStatus LiveShadowResizeStatus();
void ApplyLiveShadowResize(PPCContext&,uint8_t* base,uint32_t renderer);
bool ShadowOwnerNeedsRefresh(uint8_t* base,uint32_t owner);
// During rebinding, a generator can retain the previous target for one frame.
// Resolve only targets still tracked by the same live owner; -1 is unknown.
int TrackedShadowTargetSlot(uint32_t owner,uint32_t target);
bool TestLiveShadowResize();
}

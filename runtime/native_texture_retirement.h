#pragma once
#include "ppc_context.h"
namespace em2 {
// Called only at the two native texture-data destructor header-free sites.
// An exact queue match transfers the header to the existing fenced cleanup.
bool DeferQueuedTextureHeaderFree(PPCContext&,uint8_t* base);
void TrackNativeTextureData(uint32_t owner,uint8_t* base);
// Native queue consumer owns its +520 lock at the audited header-free site.
void RetireNativeTextureHeader(PPCContext&,uint8_t* base);
// Called after original resource Release and before wrapper backing/header free.
void TransferQueuedTextureOwnership(PPCContext&,uint8_t* base);
bool TestNativeTextureRetirement();
}

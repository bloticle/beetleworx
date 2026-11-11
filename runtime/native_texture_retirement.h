#pragma once
#include "ppc_context.h"
namespace em2 {
// Called only at the two native texture-data destructor header-free sites.
// An exact queue match transfers the header to the existing fenced cleanup.
bool DeferQueuedTextureHeaderFree(PPCContext&,uint8_t* base);
bool TestNativeTextureRetirement();
}

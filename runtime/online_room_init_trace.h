#pragma once
#include "ppc_context.h"
namespace em2::net {
// Passive private Room-only diagnostic. Existing native registry hook may call
// before/after its original; never allocates or repairs a guest registry.
void TraceNativeRoomComponentRegistry(PPCContext&,uint8_t* base,const char* phase) noexcept;
}

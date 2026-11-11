#pragma once
#include "ppc_context.h"
#include <rex/cvar.h>
REXCVAR_DECLARE(bool,em2_native_memory_copy);
namespace em2 {
inline bool NativeMemoryCopyEnabled() {return REXCVAR_GET(em2_native_memory_copy);}
void SkipNativeCopyPrefix(PPCContext& ctx,uint8_t* base,unsigned step);
bool TestNativeMemoryCopy();
}

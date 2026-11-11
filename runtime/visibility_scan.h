#pragma once
#include "ppc_context.h"
namespace em2 {
bool NativeVisibilityScanEnabled();
// 1: processed sixteen entries; 0: retry through the normal loop;
// -1: dense flags, use the original scalar loop for the remainder of this call.
int SkipNativeInvisibleBatch(PPCContext&,uint8_t*);
bool TestNativeVisibilityScan();
}

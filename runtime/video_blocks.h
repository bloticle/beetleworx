#pragma once
#include "ppc_context.h"
#include <rex/cvar.h>
REXCVAR_DECLARE(bool,em2_native_video_blocks);
REXCVAR_DECLARE(bool,em2_verify_video_blocks);
namespace em2 {
inline bool NativeVideoBlocksEnabled(){return REXCVAR_GET(em2_native_video_blocks);}
inline bool VideoBlockVerificationEnabled(){return REXCVAR_GET(em2_verify_video_blocks);}
// Diagnostic A/B runs the retained translation and localized implementation on
// the same real input, restoring their complete documented write footprint.
bool VerifyVideoBlock(PPCContext&,uint8_t*,PPCFunc* original,PPCFunc* localized,unsigned kernel);
bool TestNativeVideoBlocks();
}

#pragma once
#include <cstdint>
struct PPCContext;
namespace em2::em1 {
struct NativeScriptState;
void VerifyNativeGlobalDefinitions(PPCContext&,uint8_t*,const NativeScriptState&);
void TickNativeGlobalScripts(PPCContext&,uint8_t*,NativeScriptState&,double);
bool ReleaseNativeCutsceneInput(PPCContext&,uint8_t*,NativeScriptState&);
bool ReleaseNativeGlobalScripts(PPCContext&,uint8_t*,NativeScriptState&);
bool RetainNativeGlobalScripts(PPCContext&,uint8_t*,NativeScriptState&);
}

#pragma once
#include "ppc_context.h"
#include <rex/cvar.h>
REXCVAR_DECLARE(bool,em2_native_shader_upload);
namespace em2 {
inline bool NativeShaderUploadEnabled(){return REXCVAR_GET(em2_native_shader_upload);}
inline bool FetchUploadHasLongRun(uint64_t mask){auto runs=mask&(mask<<1);runs&=runs<<2;return (runs&(runs<<4))!=0;}
void SkipShaderUploadPrefix(PPCContext&,uint8_t*);
void SkipScalarShaderUploadPrefix(PPCContext&,uint8_t*);
void SkipFetchShaderUploadPrefix(PPCContext&,uint8_t*);
bool TestNativeFetchUpload();
bool TestNativeShaderUpload();
}

#pragma once
#include "ppc_context.h"
namespace em2 {
inline bool MaterialMaskEmpty(uint32_t address,uint32_t groups,uint8_t* base) {
    if(!groups || groups>16)return false;
    const auto end=uint64_t(address)+groups*8;
    if(end>0x100000000ull || PPC_PHYS_HOST_OFFSET(address)!=PPC_PHYS_HOST_OFFSET(uint32_t(end-1)) ||
       (uint64_t(address)<0x80000000ull && end>0x7F000000ull))return false;
    unsigned i=0;
    for(;i+2<=groups;i+=2) {
        const auto bits=simde_mm_loadu_si128((const simde__m128i*)PPC_RAW_ADDR(address+i*8));
        if(!simde_mm_testz_si128(bits,bits))return false;
    }
    return i==groups || PPC_LOAD_U64(address+i*8)==0;
}
inline bool SkipEmptyMaterialVertexStage(PPCContext& ctx,uint8_t* base) {
    const auto groups=ctx.r11.u32;
    if(!MaterialMaskEmpty(ctx.r21.u32,groups,base))return false;
    // Empty native passes leave these volatile loop counters live. Other
    // volatile scratch is replaced by the native pixel-stage setup below.
    ctx.r3.u64=uint64_t(groups)*1024;ctx.r5.u64=uint64_t(groups)*8;ctx.r6.u64=0;
    return true;
}
inline bool SkipEmptyMaterialPixelStage(PPCContext& ctx,uint8_t* base) {
    const auto groups=ctx.r11.u32;
    if(!MaterialMaskEmpty(ctx.r17.u32,groups,base))return false;
    ctx.r3.u64=uint64_t(groups)*1024;ctx.r5.u64=uint64_t(groups)*8;ctx.r6.u64=0;
    ctx.r9.u64=0;ctx.r10.u64=PPC_LOAD_U32(ctx.r25.u32+60);
    ctx.cr6.compare<uint32_t>(groups,groups,ctx.xer);
    return true;
}
bool NativeMaterialBitOpsSupported();
bool TestNativeMaterialBinding();
}

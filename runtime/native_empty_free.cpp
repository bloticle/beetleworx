#include "native_empty_free.h"
#include "ppc_context.h"
#include <windows.h>
#include <immintrin.h>
#include <atomic>
#include <cstring>
#include <rex/logging.h>
PPC_EXTERN_FUNC(__imp__sub_82E29700);
namespace {
bool Writable(uint8_t* base,uint32_t p){
    if(!base || p>UINT32_MAX-3)return false;
    const auto end=reinterpret_cast<uintptr_t>(base)+uint64_t(p)+4;
    for(auto at=reinterpret_cast<uintptr_t>(base)+p;at<end;){MEMORY_BASIC_INFORMATION m{};
        if(!VirtualQuery(reinterpret_cast<const void*>(at),&m,sizeof(m)) || m.State!=MEM_COMMIT || (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
           !(m.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return false;
        const auto next=reinterpret_cast<uintptr_t>(m.BaseAddress)+m.RegionSize;if(next<=at)return false;at=next;
    }return true;
}
template<class Original> bool Free(PPCContext& ctx,uint8_t* base,Original original,bool diagnose){
    bool stale=false;
    if(ctx.r4.u32 && !ctx.r5.u32){const auto error=GetLastError();const auto fp=_mm_getcsr();stale=!Writable(base,ctx.r4.u32);
        if(stale && diagnose){static std::atomic<unsigned> reports{0};if(reports.fetch_add(1)<8)
            REXLOG_WARN("Ignored unmapped zero-byte native free: allocator {:08X}, pointer {:08X}, caller {:08X}; no freelist write",ctx.r3.u32,ctx.r4.u32,uint32_t(ctx.lr));}
        _mm_setcsr(fp);SetLastError(error);
    }
    // Never insert an already unmapped, zero-byte cleanup pointer into a pool.
    // Valid zero-byte blocks and every nonzero free retain the original path.
    if(stale)return false;original(ctx,base);return true;
}
}
PPC_FUNC_IMPL(sub_82E29700){Free(ctx,base,__imp__sub_82E29700,true);}
bool em2::TestNativeEmptyFree(){
    const auto error=GetLastError();const auto fp=_mm_getcsr();
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,16*1024*1024,MEM_RESERVE,PAGE_NOACCESS));if(!base)return false;
    struct Release{uint8_t* base;DWORD error;unsigned fp;~Release(){VirtualFree(base,0,MEM_RELEASE);_mm_setcsr(fp);SetLastError(error);}}release{base,error,fp};
    PPCContext ctx{};ctx.r3.u64=0x833D3ED8;ctx.r4.u64=0x8000;ctx.lr=0x82E2972C;
    const auto saved=ctx;unsigned calls=0;auto original=[&](PPCContext& c,uint8_t*){++calls;c.r11.u64=0x12345678;};
    ctx.r4.u64=0x78BFA8;const auto reported=ctx;
    if(Free(ctx,base,original,false) || calls || std::memcmp(&ctx,&reported,sizeof(ctx)))return false;
    ctx=saved;
    if(Free(ctx,base,original,false) || calls || std::memcmp(&ctx,&saved,sizeof(ctx)))return false;
    if(!VirtualAlloc(base+0x8000,4096,MEM_COMMIT,PAGE_READWRITE))return false;
    if(!Free(ctx,base,original,false) || calls!=1 || ctx.r11.u32!=0x12345678)return false;
    DWORD ignored=0;VirtualProtect(base+0x8000,4096,PAGE_READONLY,&ignored);ctx=saved;
    if(Free(ctx,base,original,false) || calls!=1)return false;
    ctx.r5.u64=16;if(!Free(ctx,base,original,false) || calls!=2)return false;
    ctx=saved;ctx.r4.u64=0;if(!Free(ctx,base,original,false) || calls!=3)return false;
    REXLOG_INFO("Native empty-free checks passed: unmapped/read-only empty cleanup rejected, valid empty/nonempty/null frees forwarded unchanged");return true;
}

#include "online_movement_target.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <windows.h>
#include <immintrin.h>
#include <array>
#include <cstring>
PPC_EXTERN_FUNC(__imp__sub_82732A18);
namespace em2::net {
bool ValidateNativeMovementTarget(uint32_t registry,uint32_t handle,PPCContext& ctx,uint8_t* base){
    if(!handle)return true;
    struct Host {DWORD error=GetLastError();unsigned fp=_mm_getcsr();~Host(){SetLastError(error);_mm_setcsr(fp);}} host;
    GuestReadBatch reads;
    auto readable=[&](uint32_t p,size_t n){return base && p>=4096 && uint64_t(p)+n<=0x100000000ull && GuestReadBatch::Readable(PPC_RAW_ADDR(p),n);};
    const auto top=ctx.r1.u32&~15u;
    if(!readable(registry,24) || top<12288 || !readable(top-8192,8192))return false;
    struct Restore {
        PPCContext& ctx;PPCContext saved;uint8_t* memory;std::array<uint8_t,8192> bytes;
        Restore(PPCContext& c,uint8_t* p):ctx(c),saved(c),memory(p){std::memcpy(bytes.data(),p,bytes.size());}
        ~Restore(){std::memcpy(memory,bytes.data(),bytes.size());ctx=saved;}
    } restore(ctx,PPC_RAW_ADDR(top-8192));
    ctx.r1.u64=top-4096;
    const auto key=ctx.r1.u32+512;PPC_STORE_U32(key,handle);
    ctx.r3.u64=registry;ctx.r4.u64=key;__imp__sub_82732A18(ctx,base);
    const auto entity=ctx.r3.u32;
    if(!readable(entity,96) || PPC_LOAD_U32(entity)!=0x8208D908)return false;
    uint32_t guid=0;for(unsigned i=0;i<4;++i)guid|=PPC_LOAD_U32(entity+4+i*4);
    if(!guid)return false;
    const auto count=PPC_LOAD_U32(entity+44),components=PPC_LOAD_U32(entity+36);
    if(!count || count>256 || !readable(components,size_t(count)*4))return false;
    bool frame=false;
    for(unsigned i=0;i<count;++i){
        const auto component=PPC_LOAD_U32(components+i*4);
        if(!readable(component,4))return false;
        frame|=PPC_LOAD_U32(component)==0x82090340;
    }
    return frame;
}
}

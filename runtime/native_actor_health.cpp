#include "native_actor_health.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <windows.h>
#include <immintrin.h>
#include <array>
#include <cstring>
PPC_EXTERN_FUNC(__imp__sub_827C9CE0); // entity int-property read, vtable+204
PPC_EXTERN_FUNC(__imp__sub_827CA5F0); // entity int-property write, vtable+268
namespace {
bool Readable(uint8_t* base,uint32_t at,size_t size){return base && at>=4096 && uint64_t(at)+size<=0x100000000ull && em2::GuestReadBatch::Readable(base+at,size);}
struct Borrow {
    PPCContext& ctx;PPCContext saved;DWORD error=GetLastError();unsigned csr=_mm_getcsr();
    uint8_t* memory=nullptr;uint8_t* base=nullptr;uint32_t frame=0,scratch=0;std::array<uint8_t,8192> bytes;
    Borrow(PPCContext& c,uint8_t* guest_base):ctx(c),saved(c),base(guest_base){
        const auto top=c.r1.u32&~15u;if(top<12288 || !Readable(base,top-8192,bytes.size()))return;
        memory=base+top-8192;std::memcpy(bytes.data(),memory,bytes.size());frame=top-4096;scratch=frame+512;
    }
    void Reset(){ctx=saved;ctx.r1.u64=frame;PPC_STORE_U32(frame,saved.r1.u32);}
    ~Borrow(){if(memory)std::memcpy(memory,bytes.data(),bytes.size());ctx=saved;_mm_setcsr(csr);SetLastError(error);}
};
bool Route(uint8_t* base,uint32_t entity,uint32_t& property){
    if(!Readable(base,entity,96) || PPC_LOAD_U32(entity)!=0x8208D908 || !Readable(base,0x8208D908,272) ||
        PPC_LOAD_U32(0x8208D908+204)!=0x827C9CE0 || PPC_LOAD_U32(0x8208D908+268)!=0x827CA5F0 || !Readable(base,0x833DBB14,4))return false;
    const auto count=PPC_LOAD_U32(entity+44),components=PPC_LOAD_U32(entity+36);
    if(!count || count>256 || !Readable(base,components,size_t(count)*4))return false;
    for(unsigned i=0;i<count;++i){const auto component=PPC_LOAD_U32(components+i*4);
        if(!Readable(base,component,4) || !Readable(base,PPC_LOAD_U32(component),272))return false;}
    property=PPC_LOAD_U32(0x833DBB14);return Readable(base,property,8);
}
}
std::optional<int32_t> em2::ReadNativeActorHealth(PPCContext& ctx,uint8_t* base,uint32_t entity){
    GuestReadBatch reads;Borrow borrow(ctx,base);uint32_t property=0;
    if(!borrow.memory || !Route(base,entity,property))return {};
    borrow.Reset();PPC_STORE_U32(borrow.scratch,0xCDCDCDCD);
    ctx.r3.u64=entity;ctx.r4.u64=property+4;ctx.r5.u64=borrow.scratch;ctx.r6.u64=0;
    {GuestReadBatch::CallbackBoundary callback;__imp__sub_827C9CE0(ctx,base);}
    if(!ctx.r3.u32)return {};return int32_t(PPC_LOAD_U32(borrow.scratch));
}
bool em2::CorrectNativeActorHealth(PPCContext& ctx,uint8_t* base,uint32_t entity,int32_t health){
    GuestReadBatch reads;Borrow borrow(ctx,base);uint32_t property=0;
    if(!borrow.memory || !Route(base,entity,property))return false;
    const auto current=ReadNativeActorHealth(ctx,base,entity);if(!current)return false;if(*current==health)return true;
    borrow.Reset();ctx.r3.u64=entity;ctx.r4.u64=property+4;ctx.r5.s64=health;ctx.r6.u64=0;
    {GuestReadBatch::CallbackBoundary callback;__imp__sub_827CA5F0(ctx,base);}
    if(!ctx.r3.u32)return false;
    const auto corrected=ReadNativeActorHealth(ctx,base,entity);return corrected && *corrected==health;
}

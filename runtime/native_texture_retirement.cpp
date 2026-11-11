#include "native_texture_retirement.h"
#include "guest_read_batch.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <cstring>
#include <vector>

PPC_EXTERN_FUNC(EM2RetireTextureData);
PPC_EXTERN_FUNC(EM2RetireTextureDataBase);
PPC_EXTERN_FUNC(__imp__RtlEnterCriticalSection);
PPC_EXTERN_FUNC(__imp__RtlLeaveCriticalSection);
REXCVAR_DECLARE(bool,em2_trace_shadow_preparation);
namespace {
bool Readable(uint8_t* base,uint32_t address,size_t bytes){
    return base && address>=4096 && uint64_t(address)+bytes<=0x100000000ull &&
        em2::GuestReadBatch::Readable(PPC_RAW_ADDR(address),bytes);
}
// Caller holds the resource manager's native +520 critical section. This is
// the same bounded list consumed by 826811B8, which frees headers at 82681788.
bool QueuedHeader(uint8_t* base,uint32_t manager,uint32_t header){
    if(!Readable(base,manager,572) || !Readable(base,header,52) || (PPC_LOAD_U32(header)&15)!=3)return false;
    const auto type=PPC_LOAD_U32(manager);
    if(!Readable(base,type,60) || PPC_LOAD_U32(type+56)!=0x826811B8)return false;
    const auto count=PPC_LOAD_U16(manager+566),capacity=PPC_LOAD_U16(manager+564);
    if(!count || count>capacity || capacity>16384)return false;
    const auto items=PPC_LOAD_U32(manager+560);
    if(!Readable(base,items,size_t(count)*4))return false;
    for(unsigned i=0;i<count;++i)if(PPC_LOAD_U32(items+i*4)==header)return true;
    return false;
}
uint32_t Manager(uint8_t* base){
    if(!Readable(base,0x833CE200,4))return 0;
    const auto renderer=PPC_LOAD_U32(0x833CE200);
    if(!Readable(base,renderer,1892) || PPC_LOAD_U32(renderer)!=0x82053F28)return 0;
    const auto manager=PPC_LOAD_U32(renderer+1888);
    return Readable(base,manager,572)?manager:0;
}
}
bool em2::DeferQueuedTextureHeaderFree(PPCContext& ctx,uint8_t* base){
    if((ctx.lr!=0x8267CC74 && ctx.lr!=0x8267BE04) || !ctx.r3.u32)return false;
    struct Restore {PPCContext& ctx;PPCContext saved=ctx;DWORD error=GetLastError();unsigned csr=_mm_getcsr();~Restore(){ctx=saved;SetLastError(error);_mm_setcsr(csr);}} restore{ctx};
    const auto header=ctx.r3.u32,object=ctx.r31.u32;
    uint32_t manager=0;
    {
        GuestReadBatch inspection;
        if(!Readable(base,object,40) || PPC_LOAD_U32(object)!=0x82055A3C || PPC_LOAD_U32(object+28)!=header)return false;
        manager=Manager(base);if(!manager)return false;
        const auto type=PPC_LOAD_U32(manager);
        if(!Readable(base,type,60) || PPC_LOAD_U32(type+56)!=0x826811B8)return false;
    }
    ctx.r3.u64=manager+520;__imp__RtlEnterCriticalSection(ctx,base);
    struct Unlock {PPCContext& ctx;uint8_t* base;uint32_t manager;~Unlock(){ctx.r3.u64=manager+520;__imp__RtlLeaveCriticalSection(ctx,base);}} unlock{ctx,base,manager};
    GuestReadBatch inspection;
    const bool queued=Manager(base)==manager && QueuedHeader(base,manager,header);
    if(queued && REXCVAR_GET(em2_trace_shadow_preparation))
        REXLOG_INFO("Native texture header ownership deferred: object={:08X} header={:08X} resource={:08X} backing={:08X}; exact pending resource queue match, immediate free omitted, native fenced cleanup retained",object,header,PPC_LOAD_U32(object+36),PPC_LOAD_U32(object+32));
    return queued;
}
PPC_FUNC_IMPL(sub_8267CC20){EM2RetireTextureData(ctx,base);}
PPC_FUNC_IMPL(sub_8267BDB8){EM2RetireTextureDataBase(ctx,base);}

bool em2::TestNativeTextureRetirement(){
    std::vector<uint8_t> memory(0x20000);auto* base=memory.data();
    constexpr uint32_t manager=0x10000,type=0x11000,items=0x12000,header=0x13000;
    PPC_STORE_U32(manager,type);PPC_STORE_U32(type+56,0x826811B8);
    PPC_STORE_U32(manager+560,items);PPC_STORE_U16(manager+564,4);PPC_STORE_U16(manager+566,2);
    PPC_STORE_U32(header,3);PPC_STORE_U32(items,0x14000);PPC_STORE_U32(items+4,header);
    auto check=[](bool valid,const char* description){if(!valid)REXLOG_ERROR("Native texture retirement: {}",description);return valid;};
    if(!check(QueuedHeader(base,manager,header),"exact pending header admitted"))return false;
    PPC_STORE_U16(manager+566,1);
    if(!check(!QueuedHeader(base,manager,header),"unused capacity entries do not transfer ownership"))return false;
    PPC_STORE_U16(manager+566,5);
    if(!check(!QueuedHeader(base,manager,header),"count beyond capacity rejected"))return false;
    PPC_STORE_U16(manager+566,2);PPC_STORE_U32(header,4);
    if(!check(!QueuedHeader(base,manager,header),"foreign header kind rejected"))return false;
    PPC_STORE_U32(header,3);PPC_STORE_U32(type+56,0x12345678);
    if(!check(!QueuedHeader(base,manager,header),"foreign resource manager rejected"))return false;
    PPCContext ctx{};ctx.fpscr.InitHost();ctx.r3.u64=header;ctx.lr=0x82681788;
    const auto before=ctx;const auto saved=memory;const auto csr=_mm_getcsr();SetLastError(0x11223344);
    if(!check(!DeferQueuedTextureHeaderFree(ctx,base) && !std::memcmp(&before,&ctx,sizeof(ctx)) && memory==saved && _mm_getcsr()==csr && GetLastError()==0x11223344,
        "deferred cleanup itself remains untouched, with context/memory/FP/error unchanged"))return false;
    REXLOG_INFO("Native texture retirement checks passed: exact pending queue lookup, used extent, overflow/capacity/type rejection and ordinary/deferred free-site isolation; live original deferred allocator remains acceptance");return true;
}

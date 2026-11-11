#include "platform_allocations.h"
#include "physical_arena.h"
#include "movie_texture_memory.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <immintrin.h>

REXCVAR_DEFINE_BOOL(em2_trace_platform_allocations,false,"Debug","Trace native platform physical requests and Bink physical/virtual/shared buffer selection; diagnostic only");
PPC_EXTERN_FUNC(__imp__sub_826C7728);
PPC_EXTERN_FUNC(__imp__sub_826CB030);
PPC_EXTERN_FUNC(__imp__sub_826CB0B8);
PPC_EXTERN_FUNC(__imp__sub_826CB128);
namespace {
enum class Kind {BinkBuffer,Physical,PhysicalEx};
struct Record {uint32_t caller,size,a,b,c,d,result,buffer,mode;int64_t microseconds;};
template<class Allocate,class Sink> void Trace(PPCContext& ctx,uint8_t* base,Kind kind,Allocate allocate,Sink sink){
    Record record{uint32_t(ctx.lr),ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32};
    const auto start=std::chrono::steady_clock::now();allocate(ctx,base);
    // The diagnostic path must retain the original function's FP status too.
    const auto csr=_mm_getcsr();struct Restore{unsigned csr;~Restore(){_mm_setcsr(csr);}}restore{csr};
    record.microseconds=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();record.result=ctx.r3.u32;
    if(kind==Kind::BinkBuffer){record.buffer=PPC_LOAD_U32(record.b);record.mode=PPC_LOAD_U32(record.c);}
    sink(record);
}
void Dispatch(PPCContext& ctx,uint8_t* base,Kind kind,PPCFunc* original){
    if(!REXCVAR_GET(em2_trace_platform_allocations)){original(ctx,base);return;}
    Trace(ctx,base,kind,original,[&](const Record& r){
        static std::array<std::atomic<unsigned>,3> count{};
        const auto index=count[unsigned(kind)].fetch_add(1);
        // Small CRT/audio allocations must not hide later movie fallbacks.
        if(kind!=Kind::BinkBuffer && r.result && r.size<1024*1024 && index>=16)return;
        const char* label=kind==Kind::BinkBuffer?"bink-buffer":kind==Kind::Physical?"physical":"physical-ex";
        REXLOG_INFO("Platform allocation {} caller={:08X} size={} args={:08X},{:08X},{:08X},{:08X} result={:08X} buffer={:08X} mode={} elapsed_us={}",label,r.caller,r.size,r.a,r.b,r.c,r.d,r.result,r.buffer,r.mode,r.microseconds);
        if(!r.result){std::lock_guard lock(em2::NativePhysicalArenaMutex());em2::PhysicalArenaStats stats{};if(em2::ReadPhysicalArenaStats(base,0x833D2760+32128,4096,stats))REXLOG_INFO("Platform failure graphics arena: total={} used={} free={} largest={} blocks={}",stats.total,stats.used,stats.free,stats.largest_aligned,stats.blocks);}
    });
}
}
PPC_FUNC_IMPL(sub_826C7728){Dispatch(ctx,base,Kind::BinkBuffer,__imp__sub_826C7728);}
PPC_FUNC_IMPL(sub_826CB030){if(!em2::TryMovieTextureAllocation(ctx,base))Dispatch(ctx,base,Kind::Physical,__imp__sub_826CB030);}
PPC_FUNC_IMPL(sub_826CB0B8){Dispatch(ctx,base,Kind::PhysicalEx,__imp__sub_826CB0B8);}
PPC_FUNC_IMPL(sub_826CB128){if(!em2::ReleaseMovieTextureAllocation(ctx,base))__imp__sub_826CB128(ctx,base);}
bool em2::TestPlatformAllocations(){
    const auto old_csr=_mm_getcsr();struct Restore{unsigned csr;~Restore(){_mm_setcsr(csr);}}restore{old_csr};
    for(auto kind:{Kind::BinkBuffer,Kind::Physical,Kind::PhysicalEx}){
        alignas(16)std::array<uint8_t,512> a{},b{};PPCContext input{};input.r3.u64=131072;input.r4.u64=0xFFFFFFFF;input.r5.u64=128;input.r6.u64=132;input.r7.u64=0x404;input.lr=0x826C7F00;
        auto native=[](PPCContext& ctx,uint8_t* base){PPC_STORE_U32(128,0x12340000);PPC_STORE_U32(132,1);ctx.r3.u64=0x56780000;ctx.r12.u64=0xAABBCCDD;_mm_setcsr(_mm_getcsr()^0x8000);};
        auto expected=input;native(expected,a.data());const auto expected_csr=_mm_getcsr();_mm_setcsr(old_csr);
        auto actual=input;bool captured=false;Trace(actual,b.data(),kind,native,[&](const Record& r){captured=r.caller==input.lr&&r.size==131072&&r.result==0x56780000&&(kind!=Kind::BinkBuffer||(r.buffer==0x12340000&&r.mode==1));_mm_setcsr(_mm_getcsr()^0x8000);});
        if(!captured||std::memcmp(&actual,&expected,sizeof(actual))||a!=b||_mm_getcsr()!=expected_csr)return false;
        _mm_setcsr(old_csr);
    }
    REXLOG_INFO("Platform allocation trace parity passed: native context, memory and FP results retained for physical and Bink fallback diagnostics");return true;
}

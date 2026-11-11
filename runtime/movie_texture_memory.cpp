#include "movie_texture_memory.h"
#include "physical_arena.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/kernel/xmemory.h>
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <immintrin.h>

REXCVAR_DECLARE(bool,em2_trace_platform_allocations);
PPC_EXTERN_FUNC(__imp__sub_830FD0B0);
PPC_EXTERN_FUNC(__imp__sub_830FD158);
PPC_EXTERN_FUNC(__imp__sub_830FCBD8);
namespace {
constexpr uint32_t arena=0x833D2760+32128,physical_mask=0x1FFFFFFF;
struct Loan {uint8_t* base=nullptr;uint32_t backing=0,node=0,offset=0,bytes=0;};
std::array<Loan,64> loans;
struct Scope {PPCContext& ctx;PPCContext saved;unsigned csr;Scope(PPCContext& c):ctx(c),saved(c),csr(_mm_getcsr()){}~Scope(){ctx=saved;_mm_setcsr(csr);}};
bool Request(const PPCContext& c){
    // Exactly the movie plane texture creation call at82903470. Keep other
    // platform allocations, custom physical ranges and protection modes native.
    return c.lr==0x82903520 && c.r4.u32==UINT32_MAX && c.r5.u32==4096 && c.r6.u32==4 && c.r3.u32 && !(c.r3.u32&4095) && c.r3.u32<=16u*1024*1024;
}
unsigned Count(){return unsigned(std::count_if(loans.begin(),loans.end(),[](const auto& l){return l.base!=nullptr;}));}
uint32_t CachedAlias(uint32_t physical){return 0xE0000000|(physical-4096);}
// Keep the arena's registered A view: the cached E view has no SDK child
// allocation record for this borrowed region and its write watches won't arm.
uint32_t MovieAlias(uint32_t physical){return 0xA0000000|physical;}
uint32_t PhysicalOffset(uint32_t address){return (address&physical_mask)+(address>=0xE0000000?4096:0);}
bool TestSDKCachedAlias(){
    // Exercise the installed SDK, not a second copy of the alias formula.
    rex::memory::Memory memory;if(!memory.Initialize())return false;
    auto* heap=memory.LookupHeapByType(true,65536);uint32_t allocation=0;
    if(!heap||!heap->Alloc(131072,65536,rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite,true,&allocation))return false;
    struct Release{rex::memory::BaseHeap* heap;uint32_t allocation;~Release(){heap->Release(allocation);}}release{heap,allocation};
    const auto physical=memory.GetPhysicalAddress(allocation),cached=CachedAlias(physical);
    if(memory.GetPhysicalAddress(cached)!=physical){REXLOG_ERROR("Movie cached alias mismatch: A={:08X} cached={:08X}, physical={:08X}, cached physical={:08X}",allocation,cached,physical,memory.GetPhysicalAddress(cached));return false;}
    auto* a=memory.TranslateVirtual(allocation);auto* e=memory.TranslateVirtual(cached);
    std::memset(a,0x5A,131072);std::memset(e+4096,0xA5,65536);
    for(unsigned i=0;i<131072;++i)if(a[i]!=(i>=4096&&i<69632?0xA5:0x5A))return false;
    struct Watch{unsigned hits=0;uint32_t address=0;};Watch watch{0,physical+8192};
    const auto registration=memory.RegisterPhysicalMemoryInvalidationCallback(+[](void* context,uint32_t,uint32_t,bool){auto& w=*static_cast<Watch*>(context);++w.hits;return std::pair<uint32_t,uint32_t>{w.address,4096};},&watch);
    memory.EnablePhysicalMemoryAccessCallbacks(watch.address,4096,true,false);
    auto* movie=memory.TranslateVirtual(MovieAlias(physical));*reinterpret_cast<volatile uint8_t*>(movie+8192)=0x6D;
    const bool invalidated=watch.hits!=0;
    // Clear watches in all aliases before unregistering, even on rejection.
    rex::thread::global_critical_region region;memory.TriggerPhysicalMemoryCallbacks(region.Acquire(),allocation+8192,4096,true,true);
    memory.UnregisterPhysicalMemoryInvalidationCallback(registration);
    if(!invalidated){REXLOG_ERROR("Movie texture view did not trigger the SDK physical invalidation callback: address={:08X}",MovieAlias(physical));return false;}
    REXLOG_INFO("Movie texture address matches actual SDK physical addresses/shared A/E bytes and triggers physical invalidation; adjacent guards unchanged");return true;
}
}
bool em2::TryMovieTextureAllocation(PPCContext& ctx,uint8_t* base){
    if(!Request(ctx))return false;
    const auto bytes=ctx.r3.u32;uint32_t result=0;
    {
        Scope scope(ctx);std::lock_guard lock(NativePhysicalArenaMutex());PhysicalArenaStats stats{};
        if(!ReadPhysicalArenaStats(base,arena,4096,stats) || uint64_t(bytes)+8u*1024*1024>stats.free || stats.largest_aligned<bytes)return false;
        const auto backing=PPC_LOAD_U32(arena+32);
        if((backing&0xE0000000)!=0xA0000000 || (backing&4095) || uint64_t(backing&physical_mask)+stats.total>512u*1024*1024)return false;
        const auto slot=std::find_if(loans.begin(),loans.end(),[](const auto& loan){return !loan.base;});if(slot==loans.end())return false;
        ctx.r3.u64=arena;ctx.r4.u64=bytes;ctx.r5.u64=4096;__imp__sub_830FD0B0(ctx,base);
        const auto node=ctx.r3.u32;if(!node)return false;
        const auto offset=(backing&physical_mask)+PPC_LOAD_U32(node);
        // These planes are suballocations of the already-registered graphics
        // backing. Use its physical A view so CPU writes receive the same SDK
        // cache invalidation handling as other native graphics resources.
        result=MovieAlias(offset);*slot={base,backing,node,offset,bytes};
        if(REXCVAR_GET(em2_trace_platform_allocations))REXLOG_INFO("Movie texture arena allocate bytes={} address={:08X} node={:08X} live={} free_before={}",bytes,result,node,Count(),stats.free);
    }
    ctx.r3.u64=result;return true;
}
bool em2::ReleaseMovieTextureAllocation(PPCContext& ctx,uint8_t* base){
    const auto address=ctx.r3.u32;if(address<0xA0000000)return false;
    Scope scope(ctx);std::lock_guard lock(NativePhysicalArenaMutex());
    const auto slot=std::find_if(loans.begin(),loans.end(),[&](const auto& l){return l.base==base && l.offset==PhysicalOffset(address);});if(slot==loans.end())return false;
    PhysicalArenaStats stats{};
    if(!ReadPhysicalArenaStats(base,arena,4096,stats)||PPC_LOAD_U32(arena+32)!=slot->backing){
        REXLOG_ERROR("Movie texture backing changed before release; refusing to free a parent physical allocation");*slot={};return true;
    }
    // Confirm that the recorded metadata node is still an allocated member
    // of this arena. Never send a borrowed suballocation to MmFreePhysical.
    auto node=PPC_LOAD_U32(arena+8);bool found=false;
    for(unsigned i=0;i<stats.blocks;++i){if(node==slot->node){found=!PPC_LOAD_U8(node+11) && (slot->backing&physical_mask)+PPC_LOAD_U32(node)==slot->offset && PPC_LOAD_U32(node+4)==slot->bytes;break;}node=PPC_LOAD_U32(node+20);}
    if(!found){REXLOG_ERROR("Movie texture metadata changed before release; refusing a stale free");*slot={};return true;}
    ctx.r3.u64=arena;ctx.r4.u64=slot->node;__imp__sub_830FD158(ctx,base);*slot={};
    if(REXCVAR_GET(em2_trace_platform_allocations))REXLOG_INFO("Movie texture arena release address={:08X} live={}",address,Count());
    return true;
}
bool em2::TestMovieTextureMemory(){
    if(!TestSDKCachedAlias())return false;
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));if(!base)return false;
    if(!VirtualAlloc(base,65536,MEM_COMMIT,PAGE_READWRITE)||!VirtualAlloc(base+0x833D0000,65536,MEM_COMMIT,PAGE_READWRITE)){VirtualFree(base,0,MEM_RELEASE);return false;}
    std::lock_guard lock(NativePhysicalArenaMutex());const auto previous=loans;
    struct Cleanup{uint8_t* base;std::array<Loan,64> saved;unsigned csr=_mm_getcsr();~Cleanup(){loans=saved;_mm_setcsr(csr);VirtualFree(base,0,MEM_RELEASE);}}cleanup{base,previous};loans={};
    PPCContext ctx{};ctx.r1.u64=0xF000;ctx.r3.u64=arena;__imp__sub_830FCBD8(ctx,base);
    constexpr uint32_t a=0x5000,b=0x5040,c=0x5080,d=0x50C0,bytes=65536,reserve=8u*1024*1024;
    PPC_STORE_U32(arena+8,a);PPC_STORE_U32(arena+12,256);PPC_STORE_U32(arena+20,reserve+bytes+256);PPC_STORE_U32(arena+32,0xA0800000);
    auto block=[&](uint32_t node,uint32_t offset,uint32_t size,bool free,uint32_t previous,uint32_t next){PPC_STORE_U32(node,offset);PPC_STORE_U32(node+4,size);PPC_STORE_U8(node+11,free);PPC_STORE_U32(node+16,previous);PPC_STORE_U32(node+20,next);};
    block(a,0,bytes,true,d,b);block(b,bytes,128,false,a,c);block(c,bytes+128,reserve,true,b,d);block(d,bytes+128+reserve,128,false,c,a);
    ctx={};ctx.r1.u64=0xF000;ctx.r3.u64=bytes;ctx.r4.u64=UINT32_MAX;ctx.r5.u64=4096;ctx.r6.u64=4;ctx.lr=0x82903520;ctx.fpscr.InitHost();const auto input=ctx;const auto csr=_mm_getcsr();
    if(!TryMovieTextureAllocation(ctx,base)||ctx.r3.u32!=0xA0800000||Count()!=1)return false;
    auto expected=input;expected.r3.u64=0xA0800000;PhysicalArenaStats stats{};
    if(std::memcmp(&ctx,&expected,sizeof(ctx))||_mm_getcsr()!=csr||!ReadPhysicalArenaStats(base,arena,4096,stats)||stats.free!=reserve)return false;
    const auto before=ctx;if(!ReleaseMovieTextureAllocation(ctx,base)||Count()||std::memcmp(&ctx,&before,sizeof(ctx))||!ReadPhysicalArenaStats(base,arena,4096,stats)||stats.free!=reserve+bytes||_mm_getcsr()!=csr)return false;
    if(ReleaseMovieTextureAllocation(ctx,base))return false;
    for(unsigned kind=0;kind<4;++kind){ctx=input;if(kind==0)ctx.lr=0x82903524;if(kind==1)ctx.r6.u64=0x404;if(kind==2)ctx.r5.u64=65536;if(kind==3)ctx.r3.u64=2*bytes;const auto original=ctx;if(TryMovieTextureAllocation(ctx,base)||std::memcmp(&ctx,&original,sizeof(ctx))||Count())return false;}
    REXLOG_INFO("Movie texture memory passed: original arena allocate/free, registered physical view, exact capacity/headroom, caller/protection/alignment guards, duplicate lookup rejection, context/FP preservation");return true;
}

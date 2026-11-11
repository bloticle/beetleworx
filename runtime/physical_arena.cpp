#include "physical_arena.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <windows.h>
#include <array>
#include <cstring>
#include <immintrin.h>
PPC_EXTERN_FUNC(__imp__sub_830FD0B0);
PPC_EXTERN_FUNC(__imp__sub_830FD158);
PPC_EXTERN_FUNC(__imp__sub_830FCBD8);
std::recursive_mutex& em2::NativePhysicalArenaMutex(){static std::recursive_mutex mutex;return mutex;}
PPC_FUNC_IMPL(sub_830FD0B0){std::lock_guard lock(em2::NativePhysicalArenaMutex());__imp__sub_830FD0B0(ctx,base);}
PPC_FUNC_IMPL(sub_830FD158){std::lock_guard lock(em2::NativePhysicalArenaMutex());__imp__sub_830FD158(ctx,base);}
namespace {
constexpr uint32_t kArenaVtable=0x821E3A70;
bool Read(uint8_t* base,uint32_t descriptor,uint32_t alignment,em2::PhysicalArenaStats& result) {
    if(!alignment || alignment>0x10000000 || descriptor>0xFFFFFFD4 || PPC_LOAD_U32(descriptor)!=kArenaVtable)return false;
    const auto head=PPC_LOAD_U32(descriptor+8),total=PPC_LOAD_U32(descriptor+20),used=PPC_LOAD_U32(descriptor+12);
    if(!head || !total || used>total)return false;
    em2::PhysicalArenaStats stats{};stats.total=total;stats.used=used;
    uint64_t sum=0,free=0;auto node=head;
    do {
        if(node>0xFFFFFFE8 || (node&3) || ++stats.blocks>65536)return false;
        const auto offset=PPC_LOAD_U32(node),size=PPC_LOAD_U32(node+4),next=PPC_LOAD_U32(node+20);
        const auto end=uint64_t(offset)+size;
        if(!size || end>total || !next || next>0xFFFFFFE8 || (next&3) || PPC_LOAD_U32(next+16)!=node)return false;
        if(PPC_LOAD_U32(next)!=(end==total?0:uint32_t(end)))return false;
        sum+=size;if(sum>total)return false;
        if(PPC_LOAD_U8(node+11)) {
            free+=size;
            // 830FCEA8 aligns the arena-relative offset, then 830FD0B0
            // rounds requested sizes to 128 bytes before finding a block.
            const auto remainder=offset%alignment,padding=remainder?alignment-remainder:0;
            const auto usable=size>padding?(size-padding)&~127u:0;
            if(usable>stats.largest_aligned)stats.largest_aligned=usable;
        }
        node=next;
    }while(node!=head);
    if(sum!=total || free+used!=total)return false;
    stats.free=uint32_t(free);result=stats;return true;
}
}
bool em2::ReadPhysicalArenaStats(uint8_t* base,uint32_t descriptor,uint32_t alignment,PhysicalArenaStats& result) {
    result={};
    __try{return Read(base,descriptor,alignment,result);}
    __except(EXCEPTION_EXECUTE_HANDLER){result={};return false;}
}
bool em2::TestPhysicalArenaStats() {
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x20000,MEM_RESERVE,PAGE_READWRITE));
    if(!base)return false;
    if(!VirtualAlloc(base,0x10000,MEM_COMMIT,PAGE_READWRITE)){VirtualFree(base,0,MEM_RELEASE);return false;}
    struct Release {uint8_t* base;unsigned csr=_mm_getcsr();~Release(){_mm_setcsr(csr);VirtualFree(base,0,MEM_RELEASE);}}release{base};
    constexpr uint32_t arena=0x1000,a=0x2000,b=0x2040,c=0x2080;
    auto setup=[&](uint32_t total){std::memset(base,0,0x10000);PPCContext ctx{};ctx.r3.u64=arena;__imp__sub_830FCBD8(ctx,base);PPC_STORE_U32(arena+20,total);PPC_STORE_U32(arena+8,a);};
    auto block=[&](uint32_t address,uint32_t offset,uint32_t size,bool available,uint32_t previous,uint32_t next) {
        PPC_STORE_U32(address,offset);PPC_STORE_U32(address+4,size);PPC_STORE_U8(address+11,available);PPC_STORE_U32(address+16,previous);PPC_STORE_U32(address+20,next);
    };
    PhysicalArenaStats stats{};unsigned checks=0;
    auto check=[&](bool ok){++checks;if(!ok)REXLOG_ERROR("Physical arena snapshot check {} failed",checks);return ok;};
    setup(65536);block(a,0,65536,true,a,a);
    const auto csr=_mm_getcsr();std::array<uint8_t,0x10000> before{};std::memcpy(before.data(),base,before.size());
    if(!check(ReadPhysicalArenaStats(base,arena,4096,stats) && stats.total==65536 && stats.free==65536 && stats.used==0 && stats.largest_aligned==65536 && stats.blocks==1 &&
        _mm_getcsr()==csr && !std::memcmp(before.data(),base,before.size())))return false;
    // An exact-fit native allocation takes the real search/split path without
    // requiring a title's metadata allocator. It must match the snapshot cap.
    PPCContext ctx{};ctx.fpscr.InitHost();ctx.r1.u64=0xF000;ctx.r3.u64=arena;ctx.r4.u64=65536;ctx.r5.u64=4096;
    __imp__sub_830FD0B0(ctx,base);
    if(!check(ctx.r3.u32==a && ReadPhysicalArenaStats(base,arena,4096,stats) && stats.used==65536 && stats.free==0 && stats.largest_aligned==0))return false;
    // Fragmentation and offset alignment can reject a request despite ample
    // aggregate free bytes. Check the actual native failure path as well.
    setup(32768);PPC_STORE_U32(arena+12,8192);
    block(a,0,8192,true,c,b);block(b,8192,8192,false,a,c);block(c,16384,16384,true,b,a);
    if(!check(ReadPhysicalArenaStats(base,arena,4096,stats) && stats.free==24576 && stats.largest_aligned==16384 && stats.blocks==3))return false;
    ctx={};ctx.fpscr.InitHost();ctx.r1.u64=0xF000;ctx.r3.u64=arena;ctx.r4.u64=20480;ctx.r5.u64=4096;
    __imp__sub_830FD0B0(ctx,base);if(!check(ctx.r3.u32==0))return false;
    setup(8192);PPC_STORE_U32(arena+12,128);block(a,0,128,false,b,b);block(b,128,8064,true,a,a);
    if(!check(ReadPhysicalArenaStats(base,arena,4096,stats) && stats.free==8064 && stats.largest_aligned==4096))return false;
    ctx={};ctx.fpscr.InitHost();ctx.r1.u64=0xF000;ctx.r3.u64=arena;ctx.r4.u64=4097;ctx.r5.u64=4096;
    __imp__sub_830FD0B0(ctx,base);if(!check(ctx.r3.u32==0))return false;
    // Reject malformed/inconsistent snapshots rather than trust stale totals.
    for(unsigned variant=0;variant<7;++variant) {
        setup(8192);block(a,0,8192,true,a,a);
        if(variant==0)PPC_STORE_U32(arena,0);
        if(variant==1)PPC_STORE_U32(arena+12,8193);
        if(variant==2)PPC_STORE_U32(a+20,0);
        if(variant==3)PPC_STORE_U32(a+16,b);
        if(variant==4)PPC_STORE_U32(a+4,8193);
        if(variant==5)PPC_STORE_U32(a,1);
        if(variant==6)PPC_STORE_U32(arena+12,128);
        if(!check(!ReadPhysicalArenaStats(base,arena,4096,stats) && stats.total==0))return false;
    }
    if(!check(!ReadPhysicalArenaStats(base,arena,0,stats)))return false;
    // Faulting guest pointers are contained even from an exception report.
    setup(8192);PPC_STORE_U32(arena+8,0x10000);
    if(!check(!ReadPhysicalArenaStats(base,arena,4096,stats)))return false;
    REXLOG_INFO("Physical arena snapshot passed {} checks: real native constructor, exact-fit allocation, fragmentation/alignment failures, read-only/FP parity and malformed/inaccessible metadata; no resize admission without synchronization",checks);
    return true;
}

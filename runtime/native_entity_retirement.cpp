#include "native_entity_retirement.h"
#include "native_entity_probe.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <cstring>
#include <vector>
PPC_EXTERN_FUNC(sub_82744E90);
PPC_EXTERN_FUNC(sub_827437B0);
namespace {
bool Readable(uint8_t* base,uint32_t p,size_t n){return base && p>=4096 && uint64_t(p)+n<=0x100000000ull && em2::GuestReadBatch::Readable(base+p,n);}
struct Preserve {PPCContext& ctx;PPCContext saved;unsigned fp=_mm_getcsr();DWORD error=GetLastError();explicit Preserve(PPCContext& c):ctx(c),saved(c){}~Preserve(){ctx=saved;_mm_setcsr(fp);SetLastError(error);}};
}
bool em2::ReplayNativeEntityRetirement(PPCContext& ctx,uint8_t* base,uint32_t registry,
    const std::array<uint32_t,4>& id,std::span<const uint32_t> humans,bool& requested,const char*& error){
    Preserve preserve(ctx);GuestReadBatch reads;requested=false;
    const auto fail=[&](const char* why){error=why;return false;};
    NativeEntitySnapshot snapshot;uint32_t address=0;
    if(!ReadNativeEntitySnapshot(base,snapshot,error,address,false) || snapshot.registry!=registry)return fail("retirement registry unavailable or replaced");
    const auto found=std::find_if(snapshot.records.begin(),snapshot.records.end(),[&](const auto& row){return row.id==id;});
    if(found==snapshot.records.end()){error=nullptr;return true;}
    const auto entity=found->debug_address;
    if(PPC_LOAD_U32(entity)!=0x8208D908 || std::find(humans.begin(),humans.end(),entity)!=humans.end())return fail("retirement is not an admitted non-human entity");
    const auto queue=[&](bool& contains){
        if(!Readable(base,registry,204))return false;const auto data=PPC_LOAD_U32(registry+192),count=PPC_LOAD_U32(registry+196);
        if(count>65536 || (count && !Readable(base,data,size_t(count)*4)))return false;
        contains=false;
        for(unsigned i=0;i<count;++i){const auto value=PPC_LOAD_U32(data+i*4);
            if(std::any_of(humans.begin(),humans.end(),[&](auto human){return value==human || value==human-8;}))return false;
            contains|=value==entity || value==entity-8;
        }
        return true;
    };
    bool queued=false;if(!queue(queued))return fail("retirement queue is malformed or contains a human");
    const auto top=ctx.r1.u32&~15u;constexpr size_t bytes=65536;
    if(top<bytes+4096 || !Readable(base,top-uint32_t(bytes),bytes))return fail("retirement has no bounded borrowed stack");
    std::vector<uint8_t> backup(bytes);const auto start=top-uint32_t(bytes);std::memcpy(backup.data(),base+start,bytes);
    struct Restore {uint8_t* to;const std::vector<uint8_t>& bytes;~Restore(){std::memcpy(to,bytes.data(),bytes.size());}}restore{base+start,backup};
    const auto frame=[&]{ctx=preserve.saved;ctx.r1.u64=top-512;PPC_STORE_U32(ctx.r1.u32,top);ctx.r3.u64=registry;};
    if(!queued){frame();ctx.r4.u64=entity;GuestReadBatch::CallbackBoundary callback;sub_82744E90(ctx,base);requested=true;}
    if(!Readable(base,0x833DA7E4,4) || PPC_LOAD_U32(0x833DA7E4)!=registry || !queue(queued) || !queued)
        return fail("original retirement did not queue its entity in the same world");
    // This is an extra completed-boundary cleanup, not another simulation
    // update: honor native readiness (r4=false) and advance no timers (dt=0).
    {frame();ctx.r4.u64=0;ctx.f1.f64=0.0;GuestReadBatch::CallbackBoundary callback;sub_827437B0(ctx,base);}
    if(!ReadNativeEntitySnapshot(base,snapshot,error,address,false) || snapshot.registry!=registry)return fail("retirement flush replaced or invalidated the registry");
    if(std::any_of(snapshot.records.begin(),snapshot.records.end(),[&](const auto& row){return row.id==id;}))return fail("entity remains registered after original retirement flush");
    error=nullptr;return true;
}

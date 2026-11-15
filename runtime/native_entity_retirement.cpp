#include "native_entity_retirement.h"
#include "native_entity_probe.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <cstring>
#include <vector>
#include <array>
#include <set>
#include <rex/logging.h>
PPC_EXTERN_FUNC(sub_82744E90);
PPC_EXTERN_FUNC(sub_827437B0);
namespace {
bool Readable(uint8_t* base,uint32_t p,size_t n){return base && p>=4096 && uint64_t(p)+n<=0x100000000ull && em2::GuestReadBatch::Readable(base+p,n);}
struct Preserve {PPCContext& ctx;PPCContext saved;unsigned fp=_mm_getcsr();DWORD error=GetLastError();explicit Preserve(PPCContext& c):ctx(c),saved(c){}~Preserve(){ctx=saved;_mm_setcsr(fp);SetLastError(error);}};
}
bool em2::ReadNativeParticleRetirement(uint8_t* base,uint32_t registry,uint32_t entity,std::span<const uint32_t> humans,bool& ready){
    const auto fp=_mm_getcsr();const auto host_error=GetLastError();
    struct Host {unsigned fp;DWORD error;~Host(){_mm_setcsr(fp);SetLastError(error);}} host{fp,host_error};
    GuestReadBatch reads;ready=false;
    if(!Readable(base,0x833DA7E4,4) || PPC_LOAD_U32(0x833DA7E4)!=registry || !Readable(base,registry,204) ||
       !Readable(base,entity,96) || PPC_LOAD_U32(entity)!=0x8208D908 || std::find(humans.begin(),humans.end(),entity)!=humans.end())return false;
    const auto data=PPC_LOAD_U32(registry+192),queued=PPC_LOAD_U32(registry+196);
    if(!queued || queued>65536 || !Readable(base,data,size_t(queued)*4))return false;
    bool found=false;for(unsigned i=0;i<queued;++i){const auto p=PPC_LOAD_U32(data+i*4);found|=p==entity || p==entity-8;}if(!found)return false;
    const auto count=PPC_LOAD_U32(entity+44),items=PPC_LOAD_U32(entity+36);
    constexpr std::array<uint32_t,7> tables{0x82075E08,0x82082818,0x82099898,0x8209BA00,0x820B6298,0x820848D8,0x82090340};
    // InputOutput, Lifetime, LOD, ParticleSystemController, GameEvents,
    // JPSSceneGraph and JPSTransformation only: no physics, scripts, AI,
    // health, spawner, damage, pickup or other gameplay-bearing component.
    if(count!=tables.size() || !Readable(base,items,count*4) || !Readable(base,0x82F1BF10,8) || !Readable(base,0x828000A8,8) ||
       PPC_LOAD_U32(0x82F1BF10)!=0x38600001 || PPC_LOAD_U32(0x82F1BF14)!=0x4E800020 ||
       PPC_LOAD_U32(0x828000A8)!=0x88630050 || PPC_LOAD_U32(0x828000AC)!=0x4E800020)return false;
    std::set<uint32_t> seen;uint32_t particle=0;
    for(unsigned i=0;i<count;++i){const auto face=PPC_LOAD_U32(items+i*4);
        if(face<4104 || !Readable(base,face-8,96))return false;const auto table=PPC_LOAD_U32(face);
        if(std::find(tables.begin(),tables.end(),table)==tables.end() || !seen.insert(table).second ||
           !Readable(base,table,384) || PPC_LOAD_U32(face-8)!=table+0x190 || PPC_LOAD_U32(face+40)!=entity-8 ||
           PPC_LOAD_U32(table+380)!=(table==0x8209BA00?0x828000A8:0x82F1BF10))return false;
        if(table==0x8209BA00)particle=face;
    }
    const auto value=PPC_LOAD_U8(particle+80);if(value>1)return false;ready=value!=0;return true;
}
bool em2::ReplayNativeEntityRetirement(PPCContext& ctx,uint8_t* base,uint32_t registry,
    const std::array<uint32_t,4>& id,std::span<const uint32_t> humans,bool& requested,NativeVisualRetirement& pending,const char*& error){
    Preserve preserve(ctx);GuestReadBatch reads;requested=false;pending={};
    const auto fail=[&](const char* why){error=why;return false;};
    NativeEntitySnapshot snapshot;uint32_t address=0;
    if(!ReadNativeEntitySnapshot(base,snapshot,error,address,false) || snapshot.registry!=registry)return fail("retirement registry unavailable or replaced");
    const auto found=std::find_if(snapshot.records.begin(),snapshot.records.end(),[&](const auto& row){return row.id==id;});
    if(found==snapshot.records.end()){error=nullptr;return true;}
    const auto entity=found->debug_address;
    const auto handle=found->handle;
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
    if(const auto row=std::find_if(snapshot.records.begin(),snapshot.records.end(),[&](const auto& r){return r.id==id;});row!=snapshot.records.end()){
        bool ready=false;
        if(row->debug_address!=entity || row->handle!=handle || !ReadNativeParticleRetirement(base,registry,entity,humans,ready) || ready)
            return fail("entity remains registered after original retirement flush");
        pending={entity,handle};
    }
    error=nullptr;return true;
}
bool em2::TestNativeParticleRetirement(){
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));if(!base)return false;
    struct Release {void* base;~Release(){VirtualFree(base,0,MEM_RELEASE);}} release{base};
    for(const auto region:std::array<std::array<uint32_t,2>,5>{{{0,0x10000},{0x833D0000,0x10000},{0x82070000,0x50000},{0x82800000,0x1000},{0x82F1B000,0x1000}}})
        if(!VirtualAlloc(base+region[0],region[1],MEM_COMMIT,PAGE_READWRITE))return false;
    constexpr uint32_t registry=0x2000,entity=0x4000,items=0x6000;
    constexpr std::array<uint32_t,7> tables{0x82075E08,0x82082818,0x82099898,0x8209BA00,0x820B6298,0x820848D8,0x82090340};
    PPC_STORE_U32(0x833DA7E4,registry);PPC_STORE_U32(registry+192,0x3000);PPC_STORE_U32(registry+196,1);PPC_STORE_U32(0x3000,entity);
    PPC_STORE_U32(entity,0x8208D908);PPC_STORE_U32(entity+36,items);PPC_STORE_U32(entity+44,7);
    PPC_STORE_U32(0x82F1BF10,0x38600001);PPC_STORE_U32(0x82F1BF14,0x4E800020);PPC_STORE_U32(0x828000A8,0x88630050);PPC_STORE_U32(0x828000AC,0x4E800020);
    for(unsigned i=0;i<7;++i){const auto face=0x7008+i*0x100;PPC_STORE_U32(items+i*4,face);PPC_STORE_U32(face,tables[i]);PPC_STORE_U32(face-8,tables[i]+0x190);PPC_STORE_U32(face+40,entity-8);PPC_STORE_U32(tables[i]+380,i==3?0x828000A8:0x82F1BF10);}
    constexpr uint32_t particle=0x7308;unsigned checks=0;bool ready=false;
    auto check=[&](bool value,const char* why){++checks;if(!value)REXLOG_ERROR("Native particle retirement: {}",why);return value;};
    const auto memory=std::vector<uint8_t>(base,base+0x10000);const auto fp=_mm_getcsr();SetLastError(0x61723);
    if(!check(ReadNativeParticleRetirement(base,registry,entity,{},ready) && !ready && !std::memcmp(base,memory.data(),memory.size()) && GetLastError()==0x61723 && _mm_getcsr()==fp,"queued visual-only shape is read without readiness, memory or host-environment mutation"))return false;
    PPC_STORE_U8(particle+80,1);if(!check(ReadNativeParticleRetirement(base,registry,entity,{},ready) && ready,"native ready phase remains observable without executing callbacks"))return false;PPC_STORE_U8(particle+80,0);
    if(!check(!ReadNativeParticleRetirement(base,registry,entity,std::array<uint32_t,1>{entity},ready),"human target rejected"))return false;
    for(unsigned mutation=0;mutation<14;++mutation){
        uint32_t address=0,value=0;
        switch(mutation){
        case 0:address=entity;value=0x8208D90C;break;
        case 1:address=entity+44;value=6;break;
        case 2:address=entity+44;value=8;break;
        case 3:address=items;value=0x7108;break;
        case 4:address=0x7008;value=0x820A0000;break;
        case 5:address=0x7000;value=0x82075F9C;break;
        case 6:address=0x7008+40;value=entity;break;
        case 7:address=tables[0]+380;value=0x828000A8;break;
        case 8:address=0x833DA7E4;value=registry+4;break;
        case 9:address=0x3000;value=entity+8;break;
        case 10:address=registry+196;value=65537;break;
        case 11:address=registry+192;value=0;break;
        case 12:address=0x828000A8;value=0x88630051;break;
        case 13:address=entity+36;value=0;break;
        }
        const auto previous=PPC_LOAD_U32(address);PPC_STORE_U32(address,value);
        const auto rejected=!ReadNativeParticleRetirement(base,registry,entity,{},ready);PPC_STORE_U32(address,previous);
        if(!check(rejected,"unknown classes, component aliases/owners/readiness, queue/registry changes and unreadable layouts rejected"))return false;
    }
    PPC_STORE_U8(particle+80,2);if(!check(!ReadNativeParticleRetirement(base,registry,entity,{},ready),"non-boolean native readiness rejected"))return false;
    REXLOG_INFO("Native particle retirement passed {} visual shape, queue ownership, native leaf, human and mutation checks; native drain remains live acceptance",checks);return true;
}

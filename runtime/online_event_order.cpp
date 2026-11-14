#include "online_event_order.h"
#include "online_room_gameplay.h"
#include "simulation.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <set>
#include <vector>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <array>
PPC_EXTERN_FUNC(__imp__sub_827340E0);
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DEFINE_BOOL(em2_online_event_order,true,"Online","Traverse native broadcast subscribers in registration order during agreed Room updates");
namespace {
bool Readable(uint32_t p,size_t n,uint8_t* base){return base && p>=4096 && uint64_t(p)+n<=0x100000000ull && em2::GuestReadBatch::Readable(base+p,n);}
struct Environment {unsigned fp=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(fp);SetLastError(error);}};
struct Entry {uint32_t node,listener,record,epoch;};
struct Iteration {uint8_t* base=nullptr;uint32_t stack=0,manager=0,event=0,table=0,listener=0,record=0;std::vector<Entry> entries;size_t next=0;bool active=false;};
thread_local Iteration* iteration=nullptr;
bool Build(Iteration& out,const PPCContext& ctx,uint8_t* base){
    const auto manager=ctx.r3.u32,event=ctx.r4.u32;
    if(ctx.r5.u32 || ctx.r1.u32<160 || !Readable(manager,1016,base) || !Readable(event,12,base) || PPC_LOAD_U32(manager+1008)>=10)return false;
    const auto type=PPC_LOAD_U16(event+4);if(type>=214)return false;
    const auto table=PPC_LOAD_U32(manager+8+type*4);if(!Readable(table,12,base))return false;
    const auto count=PPC_LOAD_U32(table+4),buckets=PPC_LOAD_U32(table+8),ceiling=PPC_LOAD_U32(manager+4);
    if(count>65536 || (count && !Readable(buckets,size_t(count)*4,base)))return false;
    std::set<uint32_t> nodes,epochs;
    for(unsigned i=0;i<count;++i){auto node=PPC_LOAD_U32(buckets+i*4);
        while(node){
            if(nodes.size()>=65536 || !nodes.insert(node).second || !Readable(node,12,base))return false;
            const auto listener=PPC_LOAD_U32(node+4),record=PPC_LOAD_U32(node+8);
            if(!Readable(listener,6,base) || !Readable(record,20,base))return false;
            const auto epoch=PPC_LOAD_U32(record+16);
            // Inactive bindings cannot become eligible for this broadcast:
            // native reactivation assigns a newer epoch than its ceiling.
            if(PPC_LOAD_U8(record+12) && epoch<=ceiling){
                if(!epochs.insert(epoch).second)return false;
                out.entries.push_back({node,listener,record,epoch});
            }
            node=PPC_LOAD_U32(node);
        }
    }
    std::sort(out.entries.begin(),out.entries.end(),[](const auto& a,const auto& b){return a.epoch<b.epoch;});
    out.base=base;out.stack=ctx.r1.u32-160;out.manager=manager;out.event=event;out.table=table;out.active=true;return true;
}
void Run(PPCContext& ctx,uint8_t* base,bool eligible){
    struct Post {unsigned fp=0;DWORD error=0;bool armed=false;void Capture(){fp=_mm_getcsr();error=GetLastError();armed=true;}~Post(){if(armed){_mm_setcsr(fp);SetLastError(error);}}}post;
    Iteration current;
    {Environment preserve;if(eligible)Build(current,ctx,base);
        if(eligible && Readable(ctx.r4.u32,6,base) && PPC_LOAD_U16(ctx.r4.u32+4)==16 && REXCVAR_GET(em2_private_profile))
            REXLOG_INFO("Native Room stable event traversal: type 16 admitted {} subscribers {} processing {}; original broadcast gates and callbacks",current.active,current.entries.size(),Readable(ctx.r3.u32,1,base)?unsigned(PPC_LOAD_U8(ctx.r3.u32)):255);
    }
    struct Scope {Iteration* saved=iteration;explicit Scope(Iteration& value){iteration=&value;}~Scope(){iteration=saved;}}scope(current);
    try{__imp__sub_827340E0(ctx,base);}catch(...){post.Capture();throw;}
    post.Capture();
}
}
// Hooks change only the original loop's current/next node registers. Native
// eligibility, pending-removal checks, target filters, epoch ceiling, recursion
// guards and virtual callbacks run unchanged; hash tables are never rewritten.
void EM2StableEventFirst(PPCRegister& sp,PPCRegister& first,PPCRegister& manager,PPCRegister& event){
    Environment preserve;auto* s=iteration;
    if(!s || !s->active || sp.u32!=s->stack || manager.u32!=s->manager || event.u32!=s->event)return;
    first.u64=s->entries.empty()?0:1;
}
bool EM2StableEventEnter(PPCRegister& sp,PPCRegister& node){
    Environment preserve;auto* s=iteration;if(!s || !s->active || sp.u32!=s->stack)return node.u32!=0;
    auto* base=s->base;
    while(s->next<s->entries.size()){
        const auto entry=s->entries[s->next++];
        if(!Readable(s->table,12,base))throw std::runtime_error("Native event table was destroyed during dispatch");
        const auto count=PPC_LOAD_U32(s->table+4),buckets=PPC_LOAD_U32(s->table+8);
        if(!count)continue;
        if(count>65536 || !Readable(buckets,size_t(count)*4,base))throw std::runtime_error("Native event table changed layout during dispatch");
        auto current=PPC_LOAD_U32(buckets+(entry.listener%count)*4);unsigned seen=0;
        while(current){
            if(++seen>65536 || !Readable(current,12,base))throw std::runtime_error("Native event lookup lost its valid node chain");
            if(PPC_LOAD_U32(current+4)==entry.listener){
                const auto record=PPC_LOAD_U32(current+8);
                if(!Readable(record,20,base))throw std::runtime_error("Native event subscriber lost its record");
                // An explicitly reactivated/reused listener is a new binding.
                // Native's entry epoch ceiling also excludes that registration.
                if(PPC_LOAD_U32(record+16)==entry.epoch){s->listener=entry.listener;s->record=record;node.u64=current;return true;}
                break;
            }
            current=PPC_LOAD_U32(current);
        }
    }
    node.u64=0;return false;
}
void EM2StableEventNext(PPCRegister& sp,PPCRegister& listener,PPCRegister& record,PPCRegister& next){
    Environment preserve;auto* s=iteration;if(!s || !s->active || sp.u32!=s->stack)return;
    if(listener.u32!=s->listener || record.u32!=s->record)throw std::runtime_error("Native event iterator lost its admitted binding");
    // Only the native post-callback nonzero test reads this sentinel. Enter
    // resolves the next key through the CURRENT hash table before dereference,
    // so callbacks may remove nodes or rehash without retaining freed cursors.
    next.u64=s->next<s->entries.size()?1:0;
}

PPC_FUNC_IMPL(sub_827340E0){
    uint64_t tick=0;
    Run(ctx,base,REXCVAR_GET(em2_online_event_order) && em2::net::CurrentNativeRoomGameplay() &&
        !em2::net::NativeRoomGameplayHalted() && em2::CurrentAgreedSimulationTick(tick));
}

namespace {
struct Fixture {std::vector<uint32_t> calls;unsigned mutation=0;};
thread_local Fixture* fixture=nullptr;
void FixtureEvent(PPCContext& ctx,uint8_t* base){
    if(!fixture || (ctx.lr!=0x82734400 && ctx.lr!=0x8273424C) || PPC_LOAD_U32(0x2000+1008)!=1)throw std::runtime_error("event fixture ABI/depth");
    const auto serial=PPC_LOAD_U32(ctx.r3.u32+8);fixture->calls.push_back(serial);
    if(serial==1 && fixture->mutation==1)PPC_STORE_U8(0x6100+12,0);
    if(serial==1 && fixture->mutation==2){PPC_STORE_U32(0x2004,4);PPC_STORE_U32(0x6100+16,4);}
    if(serial==1 && fixture->mutation==4){
        const auto key=PPC_LOAD_U32(0x5104),bucket=0x4100+(key%7)*4;auto link=bucket;auto node=PPC_LOAD_U32(link);
        while(node && node!=0x5100){link=node;node=PPC_LOAD_U32(link);}
        if(node){PPC_STORE_U32(link,PPC_LOAD_U32(node));std::memset(base+node,0,12);}
    }
    if(serial==2 && fixture->mutation==3)throw std::runtime_error("event callback unwind");
    ctx.r3.u64=0;
}
}
bool em2::net::TestNativeEventOrder(){
    Environment preserve;
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    struct Release {uint8_t* base;Fixture* saved=fixture;~Release(){fixture=saved;if(base)VirtualFree(base,0,MEM_RELEASE);}}release{base};
    const auto path=std::filesystem::path(__FILE__).parent_path().parent_path()/"private/image.bin";
    std::error_code error;const auto size=std::filesystem::file_size(path,error);
    if(!base || error || !VirtualAlloc(base,65536,MEM_COMMIT,PAGE_READWRITE) || !VirtualAlloc(base+PPC_IMAGE_BASE,size,MEM_COMMIT,PAGE_READWRITE) ||
        !VirtualAlloc(base+PPC_IMAGE_BASE+PPC_IMAGE_SIZE,4096,MEM_COMMIT,PAGE_READWRITE))return false;
    std::ifstream image(path,std::ios::binary);if(!image.read(reinterpret_cast<char*>(base+PPC_IMAGE_BASE),size))return false;
    PPC_LOOKUP_FUNC(base,uint32_t(PPC_CODE_BASE))=FixtureEvent;
    unsigned checks=0;auto check=[&](bool valid,const char* why){++checks;if(!valid)REXLOG_ERROR("Native event order: {}",why);return valid;};
    auto setup=[&](const std::array<uint32_t,3>& listeners){
        std::memset(base,0,65536);PPC_STORE_U8(0x2000,1);PPC_STORE_U32(0x2004,3);PPC_STORE_U16(0x3004,16);
        PPC_STORE_U32(0x2000+8+16*4,0x4000);PPC_STORE_U32(0x4000,3);PPC_STORE_U32(0x4004,7);PPC_STORE_U32(0x4008,0x4100);PPC_STORE_U32(0x4200,uint32_t(PPC_CODE_BASE));
        for(unsigned i=0;i<3;++i){const auto listener=listeners[i],node=0x5000+i*256,record=0x6000+i*256,bucket=0x4100+(listener%7)*4;
            PPC_STORE_U32(node,PPC_LOAD_U32(bucket));PPC_STORE_U32(bucket,node);PPC_STORE_U32(node+4,listener);PPC_STORE_U32(node+8,record);
            PPC_STORE_U32(listener,0x4200);PPC_STORE_U32(listener+8,i+1);PPC_STORE_U8(record+12,1);PPC_STORE_U8(record+13,1);PPC_STORE_U32(record+16,i+1);
        }
    };
    const std::array<std::array<uint32_t,3>,3> layouts{{{0x7000,0x7100,0x7200},{0x7200,0x7000,0x7100},{0x7100,0x7200,0x7000}}};
    for(const auto& layout:layouts)for(unsigned mutation=0;mutation<5;++mutation){
        setup(layout);Fixture observed;observed.mutation=mutation;fixture=&observed;
        PPCContext ctx{};ctx.r1.u64=0xF000;ctx.r3.u64=0x2000;ctx.r4.u64=0x3000;ctx.r5.u64=0;ctx.fpscr.InitHost();
        bool threw=false;try{Run(ctx,base,true);}catch(const std::runtime_error&){threw=true;}
        const std::vector<uint32_t> expected=mutation==3?std::vector<uint32_t>{1,2}:mutation?std::vector<uint32_t>{1,3}:std::vector<uint32_t>{1,2,3};
        if(!check(observed.calls==expected && threw==(mutation==3) && !iteration,"original callbacks follow registration order across heap layouts, retain deactivation/epoch gates and unwind scope"))return false;
        if(!threw && !check(ctx.r1.u32==0xF000 && !PPC_LOAD_U32(0x2000+1008),"native stack and recursion depth restored"))return false;
    }
    for(bool targeted:{false,true}){
        setup(layouts[0]);Fixture observed;fixture=&observed;
        PPCContext ctx{};ctx.r1.u64=0xF000;ctx.r3.u64=0x2000;ctx.r4.u64=0x3000;ctx.r5.u64=targeted?layouts[0][1]:0;ctx.fpscr.InitHost();
        auto expected=ctx;Run(expected,base,false);const auto native=observed.calls;
        setup(layouts[0]);observed.calls.clear();if(!targeted)PPC_STORE_U32(0x6000+16,2);Run(ctx,base,true);
        if(!check(observed.calls==native && !iteration,"ambiguous epochs and explicit targets retain native dispatch"))return false;
    }
    fixture=nullptr;REXLOG_INFO("Native event order passed {} original broadcast/callback checks: distinct heap layouts, registration order, native deactivation/epoch filters, stack/depth and exception isolation; live Room acceptance separate",checks);return true;
}

#include "native_damage_event.h"
#include "native_entity_lifetime.h"
#include "native_entity_probe.h"
#include "native_actor_health.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <cstring>
PPC_EXTERN_FUNC(__imp__sub_8272F940);
PPC_EXTERN_FUNC(__imp__sub_82761F40);
PPC_EXTERN_FUNC(__imp__sub_827621C0);
PPC_EXTERN_FUNC(__imp__sub_82AAB408);
PPC_EXTERN_FUNC(__imp__sub_827FD850);
PPC_EXTERN_FUNC(__imp__sub_827985A8);
PPC_EXTERN_FUNC(__imp__sub_825430D8);
PPC_EXTERN_FUNC(__imp__sub_822F1C10);
namespace {
struct Preserve {unsigned fp=_mm_getcsr();DWORD error=GetLastError();~Preserve(){_mm_setcsr(fp);SetLastError(error);}};
bool Readable(uint8_t* base,uint32_t at,size_t size){return base && at>=4096 && uint64_t(at)+size<=0x100000000ull && em2::GuestReadBatch::Readable(base+at,size);}
// 827B58D0 hashes the four BIG-ENDIAN key bytes through 8272F940. Its
// character input is signed; treating high bytes as unsigned changes buckets.
uint32_t HandleHash(uint32_t handle){uint32_t hash=0;for(unsigned shift:{24u,16u,8u,0u})hash^=(hash<<5)+(hash>>2)+uint32_t(int32_t(int8_t(handle>>shift)))-0x14469E54u;return hash;}
bool Resolve(uint8_t* base,uint32_t registry,uint32_t handle,em2::net::DamageEntityId& id){
    const auto count=PPC_LOAD_U32(registry+20),buckets=PPC_LOAD_U32(registry+16);
    if(!handle || !count || count>1048576 || !Readable(base,buckets,size_t(count)*4))return false;
    auto node=PPC_LOAD_U32(buckets+(HandleHash(handle)%count)*4);
    for(unsigned visited=0;node && visited<65536;++visited){
        if(!Readable(base,node,12))return false;
        if(PPC_LOAD_U32(node)==handle){const auto entity=PPC_LOAD_U32(node+4);
            if((entity&3) || !Readable(base,entity,20) || PPC_LOAD_U32(entity)<0x82000000 || PPC_LOAD_U32(entity)>=0x83400000)return false;
            for(unsigned i=0;i<4;++i)id[i]=PPC_LOAD_U32(entity+4+i*4);
            return std::any_of(id.begin(),id.end(),[](auto word){return word!=0;});}
        node=PPC_LOAD_U32(node+8);
    }return false;
}
struct Borrow {
    PPCContext& ctx;PPCContext saved;Preserve host;
    uint8_t* base;uint8_t* memory=nullptr;uint32_t frame=0,event=0;std::array<uint8_t,8192> bytes;
    Borrow(PPCContext& c,uint8_t* b):ctx(c),saved(c),base(b){const auto top=c.r1.u32&~15u;
        if(top<12288 || !Readable(base,top-8192,bytes.size()))return;
        memory=base+top-8192;std::memcpy(bytes.data(),memory,bytes.size());frame=top-4096;event=frame+512;}
    void Reset(){ctx=saved;ctx.r1.u64=frame;PPC_STORE_U32(frame,saved.r1.u32);}
    ~Borrow(){if(memory)std::memcpy(memory,bytes.data(),bytes.size());ctx=saved;}
};
bool ReplayOne(Borrow& borrow,const em2::net::DamageEvent& source,uint32_t target,uint32_t instigator,const char*& error){
    auto& ctx=borrow.ctx;auto* base=borrow.base;const auto event=borrow.event;
    const auto target_ref=event+128,instigator_ref=event+132,word=event+136,tag=event+160;
    // Native string retain/release exempts the canonical empty reference at
    // 8203D278. Do not hand a null tag to listeners/destruction if this image
    // uses a different canonical reference.
    if(!source.tag_present && (!Readable(base,0x8203D278,4) || PPC_LOAD_U32(0x8203D278)!=0)){
        error="null Damage tag is not the native empty reference";return false;
    }
    if(!Readable(base,0x833DA6F4,4) || !Readable(base,PPC_LOAD_U32(0x833DA6F4),1016)){error="native Damage manager unavailable";return false;}
    const auto manager=PPC_LOAD_U32(0x833DA6F4);std::memset(base+event,0,416);
    PPC_STORE_U32(target_ref,target);PPC_STORE_U32(instigator_ref,instigator);
    borrow.Reset();ctx.r3.u64=event;ctx.r4.u64=target_ref;ctx.r5.u64=instigator_ref;
    {em2::GuestReadBatch::CallbackBoundary callback;__imp__sub_82761F40(ctx,base);}
    const auto destroy=[&]{borrow.Reset();ctx.r3.u64=event;em2::GuestReadBatch::CallbackBoundary callback;__imp__sub_827621C0(ctx,base);};
    bool success=false;
    try{
        for(unsigned i=0;i<2;++i)PPC_STORE_U8(event+16+i,source.flags[i]);
        for(unsigned i=0;i<8;++i)PPC_STORE_U32(event+32+i*4,source.vectors[i]);
        for(auto key:source.shape_keys){
            PPC_STORE_U32(word,key);borrow.Reset();ctx.r3.u64=event+64;
            ctx.r4.u64=PPC_LOAD_U32(event+68);ctx.r5.u64=word;
            em2::GuestReadBatch::CallbackBoundary callback;__imp__sub_827985A8(ctx,base);
        }
        // Append the host's complete arrays directly. AddStimulus would draw
        // another random amount for kind 1; CalculateDamage would apply local
        // resistance again. Both would change the authoritative payload.
        for(const auto& s:source.stimuli)for(unsigned lane=0;lane<3;++lane){
            const auto array=event+80+lane*12;PPC_STORE_U32(word,lane==0?s.type:lane==1?s.raw_bits:s.effective_bits);
            borrow.Reset();ctx.r3.u64=array;ctx.r4.u64=PPC_LOAD_U32(array+4);ctx.r5.u64=word;
            em2::GuestReadBatch::CallbackBoundary callback;
            if(lane==0)__imp__sub_82AAB408(ctx,base);else __imp__sub_827FD850(ctx,base);
        }
        if(!source.tag_present){
            borrow.Reset();ctx.r3.u64=event+76;
            {em2::GuestReadBatch::CallbackBoundary callback;__imp__sub_822F1C10(ctx,base);}
            PPC_STORE_U32(event+76,0);
        }else if(!source.tag.empty()){
            std::memcpy(base+tag,source.tag.data(),source.tag.size());PPC_STORE_U8(tag+uint32_t(source.tag.size()),0);
            borrow.Reset();ctx.r3.u64=event+76;
            {em2::GuestReadBatch::CallbackBoundary callback;__imp__sub_822F1C10(ctx,base);}
            PPC_STORE_U32(event+76,0);
            borrow.Reset();ctx.r3.u64=tag;
            {em2::GuestReadBatch::CallbackBoundary callback;__imp__sub_825430D8(ctx,base);}
            const auto reference=ctx.r3.u32;
            if(!reference){error="native Damage tag allocation failed";}
            else PPC_STORE_U32(event+76,reference);
        }
        if(!error){
            em2::net::DamageEvent captured;
            if(!em2::CaptureNativeDamageEvent(base,event,captured,error) || captured!=source){if(!error)error="native reconstructed Damage differs from host payload";}
            else{
                borrow.Reset();ctx.r3.u64=manager;ctx.r4.u64=event;ctx.r5.u64=0;
                {em2::GuestReadBatch::CallbackBoundary callback;em2::DispatchNativeAuthorityDamage(ctx,base);}
                success=true;
            }
        }
    }catch(...){destroy();throw;}
    destroy();return success;
}
}
bool em2::CaptureNativeDamageEvent(uint8_t* base,uint32_t event,net::DamageEvent& out,const char*& error){
    Preserve preserve;GuestReadBatch reads;error=nullptr;
    const auto fail=[&](const char* why){error=why;return false;};uint64_t before=0,after=0;
    if(!SampleNativeEntityLifetimeRevision(before))return fail("registry mutation in progress");
    if(!Readable(base,event,116) || PPC_LOAD_U32(event)!=0x82076C70 || PPC_LOAD_U16(event+4)!=14)return fail("unsupported native Damage class");
    if(!Readable(base,0x833DA7E4,4))return fail("registry global unavailable");const auto registry=PPC_LOAD_U32(0x833DA7E4);
    if(!Readable(base,registry,24) || PPC_LOAD_U32(registry+4)!=0x827B58D0 || PPC_LOAD_U32(registry+8)!=0x82A69948)return fail("registry layout changed");
    net::DamageEvent sample;
    // 8295C318 copies the array built by 8295C5xx from collision rows +36.
    // The row's +32 collidable pointer resolves the target separately through
    // 827D2B58; +36 is its child shape key. 828C07C8 similarly appends the
    // scalar key returned by 82946628, never the returned shape pointer.
    const auto shape_count=PPC_LOAD_U32(event+68),shape_data=PPC_LOAD_U32(event+64);
    if(shape_count>net::kMaxDamageShapeKeys || (shape_count && !Readable(base,shape_data,size_t(shape_count)*4)))return fail("Damage child shape array unavailable or exceeds bound");
    for(unsigned i=0;i<shape_count;++i)sample.shape_keys.push_back(PPC_LOAD_U32(shape_data+i*4));
    if(!Resolve(base,registry,PPC_LOAD_U32(event+8),sample.target))return fail("target handle has no live GUID");
    if(const auto handle=PPC_LOAD_U32(event+12)){net::DamageEntityId id;
        if(!Resolve(base,registry,handle,id))return fail("instigator handle has no live GUID");sample.instigator=id;}
    for(unsigned i=0;i<2;++i)sample.flags[i]=PPC_LOAD_U8(event+16+i);
    for(unsigned i=0;i<8;++i)sample.vectors[i]=PPC_LOAD_U32(event+32+i*4);
    const auto count=PPC_LOAD_U32(event+84);
    if(!count || count>net::kMaxDamageStimuli || PPC_LOAD_U32(event+96)!=count || PPC_LOAD_U32(event+108)!=count)return fail("Damage arrays differ or exceed bound");
    std::array<uint32_t,3> arrays{PPC_LOAD_U32(event+80),PPC_LOAD_U32(event+92),PPC_LOAD_U32(event+104)};
    for(const auto array:arrays)if(!Readable(base,array,size_t(count)*4))return fail("Damage array unavailable");
    for(unsigned i=0;i<count;++i)sample.stimuli.push_back({PPC_LOAD_U32(arrays[0]+i*4),PPC_LOAD_U32(arrays[1]+i*4),PPC_LOAD_U32(arrays[2]+i*4)});
    const auto tag=PPC_LOAD_U32(event+76);sample.tag_present=tag!=0;bool terminated=!tag;
    for(unsigned i=0;tag && i<=net::kMaxDamageTag;++i){if(uint64_t(tag)+i>UINT32_MAX || !Readable(base,tag+i,1))return fail("Damage tag unavailable");
        const auto byte=PPC_LOAD_U8(tag+i);if(!byte){terminated=true;break;}sample.tag.push_back(char(byte));}
    if(!terminated || !net::ValidDamageEvent(sample))return fail("unsupported Damage value");
    if(!SampleNativeEntityLifetimeRevision(after) || before!=after || PPC_LOAD_U32(0x833DA7E4)!=registry)return fail("registry changed during capture");
    out=std::move(sample);return true;
}
bool em2::ReplayNativeDamageEvents(PPCContext& ctx,uint8_t* base,std::span<const net::DamageEvent> events,const char*& error){
    error=nullptr;if(events.empty())return true;
    if(events.size()>128 || !std::all_of(events.begin(),events.end(),net::ValidDamageEvent)){error="invalid native Damage batch";return false;}
    GuestReadBatch reads;Borrow borrow(ctx,base);if(!borrow.memory){error="native Damage replay stack unavailable";return false;}
    NativeEntitySnapshot entities;uint64_t observed=0;bool cached=false;
    for(const auto& event:events){uint64_t revision=0;
        if(!SampleNativeEntityLifetimeRevision(revision)){error="registry mutation during native Damage replay";return false;}
        if(!cached || revision!=observed){uint32_t address=0;
            if(!ReadNativeEntitySnapshot(base,entities,error,address,false))return false;
            if(!SampleNativeEntityLifetimeRevision(observed) || observed!=revision){error="registry changed during Damage resolution";return false;}cached=true;
        }
        const auto handle=[&](const net::DamageEntityId& id){const auto found=std::lower_bound(entities.records.begin(),entities.records.end(),id,[](const auto& value,const auto& key){return value.id<key;});
            return found==entities.records.end() || found->id!=id?uint32_t(0):found->handle;};
        const auto target=handle(event.target),instigator=event.instigator?handle(*event.instigator):0;
        if(!target || (event.instigator && !instigator)){
            const auto& missing=!target?event.target:*event.instigator;
            REXLOG_WARN("Native Damage replay missing {} GUID {:08X}/{:08X}/{:08X}/{:08X}; {} live local entities, registry revision {}",
                !target?"target":"instigator",missing[0],missing[1],missing[2],missing[3],entities.records.size(),observed);
            error=!target?"host Damage target has no live local GUID":"host Damage instigator has no live local GUID";return false;
        }
        if(!ReplayOne(borrow,event,target,instigator,error))return false;
    }return true;
}
bool em2::TestNativeDamageEvent(){
    Preserve preserve;auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    struct Free {uint8_t* p;~Free(){if(p)VirtualFree(p,0,MEM_RELEASE);}} free{base};
    if(!base || !VirtualAlloc(base,0x10000,MEM_COMMIT,PAGE_READWRITE) || !VirtualAlloc(base+0x833D0000,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
    PPC_STORE_U32(0x833DA7E4,0x2000);PPC_STORE_U32(0x2004,0x827B58D0);PPC_STORE_U32(0x2008,0x82A69948);PPC_STORE_U32(0x2010,0x2800);PPC_STORE_U32(0x2014,17);
    unsigned checks=0;auto check=[&](bool good,const char* why){++checks;if(!good)REXLOG_ERROR("Native Damage capture: {}",why);return good;};
    const std::array<uint32_t,2> handles{0x7F80FF01,0x89ABCDEF};
    for(unsigned i=0;i<2;++i){PPC_STORE_U32(0x1800,handles[i]);PPCContext ctx{};ctx.r3.u64=0x1800;ctx.r4.u64=4;__imp__sub_8272F940(ctx,base);
        if(!check(ctx.r3.u32==HandleHash(handles[i]),"signed high-byte hashing matches the ORIGINAL native key hash"))return false;
        const auto slot=0x2800+(ctx.r3.u32%17)*4,node=0x3000+i*16,entity=0x4000+i*256;
        PPC_STORE_U32(node,handles[i]);PPC_STORE_U32(node+4,entity);PPC_STORE_U32(node+8,PPC_LOAD_U32(slot));PPC_STORE_U32(slot,node);
        PPC_STORE_U32(entity,0x8208D908);for(unsigned j=0;j<4;++j)PPC_STORE_U32(entity+4+j*4,i*4+j+1);
    }
    const uint32_t event=0x5000;PPC_STORE_U32(event,0x82076C70);PPC_STORE_U16(event+4,14);PPC_STORE_U32(event+8,handles[0]);PPC_STORE_U32(event+12,handles[1]);
    PPC_STORE_U32(event+76,0x6000);std::memcpy(base+0x6000,"PaintDamage",12);
    for(unsigned i=0;i<3;++i){PPC_STORE_U32(event+80+i*12,0x6100+i*256);PPC_STORE_U32(event+84+i*12,1);}
    PPC_STORE_U32(0x6100,2);PPC_STORE_U32(0x6200,0x447A0000);PPC_STORE_U32(0x6300,0x43FA0000);PPC_STORE_U32(event+32,0x80000000);
    const auto memory=std::vector<uint8_t>(base,base+0x10000);const auto fp=_mm_getcsr();SetLastError(0x1234);const char* error=nullptr;net::DamageEvent sample;
    if(!check(CaptureNativeDamageEvent(base,event,sample,error) && sample.target==net::DamageEntityId{1,2,3,4} && sample.instigator==net::DamageEntityId{5,6,7,8} &&
              sample.stimuli==std::vector<net::DamageStimulus>{{2,0x447A0000,0x43FA0000}} && sample.vectors[0]==0x80000000 && sample.tag=="PaintDamage","native dispatch payload resolves portable GUIDs and preserves exact raw/effective data"))return false;
    if(!check(!std::memcmp(base,memory.data(),memory.size()) && _mm_getcsr()==fp && GetLastError()==0x1234,"capture does not change guest bytes or host FP/error state"))return false;
    const auto good=sample;
    PPC_STORE_U32(event+76,0);
    if(!check(CaptureNativeDamageEvent(base,event,sample,error) && !sample.tag_present && sample.tag.empty(),"native nullable tag retained without reading address zero"))return false;
    PPC_STORE_U32(event+76,0x6000);
    PPC_STORE_U32(event+64,0x6400);PPC_STORE_U32(event+68,3);
    PPC_STORE_U32(0x6400,0x3152);PPC_STORE_U32(0x6404,UINT32_MAX);PPC_STORE_U32(0x6408,0);
    if(!check(CaptureNativeDamageEvent(base,event,sample,error) && sample.shape_keys==std::vector<uint32_t>{0x3152,UINT32_MAX,0},"native child shape keys retain order and sentinel bits"))return false;
    sample=good;
    for(unsigned mutation=0;mutation<10;++mutation){std::memcpy(base,memory.data(),memory.size());
        if(mutation==0)PPC_STORE_U32(event+8,0x55555555);if(mutation==1)PPC_STORE_U32(event+68,1);
        if(mutation==2)PPC_STORE_U32(event+108,2);if(mutation==3)PPC_STORE_U32(event+104,0x100000);
        if(mutation==4)PPC_STORE_U32(event+48,0x7fc00000);if(mutation==5)PPC_STORE_U32(event+76,0x100000);
        if(mutation==6)PPC_STORE_U32(0x4000,1);if(mutation==7)PPC_STORE_U32(0x3004,0x4001);
        if(mutation==8)PPC_STORE_U32(event+68,uint32_t(net::kMaxDamageShapeKeys+1));
        if(mutation==9){PPC_STORE_U32(event+68,1);PPC_STORE_U32(event+64,0x100000);}
        if(!check(!CaptureNativeDamageEvent(base,event,sample,error) && error && sample==good,"missing identities, unavailable child shape data, malformed arrays/floats/tags fail without changing output"))return false;
    }
    REXLOG_INFO("Native Damage capture passed {} original-hash, portable identity, exact payload and read-only failure checks; native replay and event transport are separate work",checks);return net::TestDamageEventCodec();
}

#include "native_damage_event.h"
#include "native_entity_lifetime.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <cstring>
PPC_EXTERN_FUNC(__imp__sub_8272F940);
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
}
bool em2::CaptureNativeDamageEvent(uint8_t* base,uint32_t event,net::DamageEvent& out,const char*& error){
    Preserve preserve;GuestReadBatch reads;error=nullptr;
    const auto fail=[&](const char* why){error=why;return false;};uint64_t before=0,after=0;
    if(!SampleNativeEntityLifetimeRevision(before))return fail("registry mutation in progress");
    if(!Readable(base,event,116) || PPC_LOAD_U32(event)!=0x82076C70 || PPC_LOAD_U16(event+4)!=14)return fail("unsupported native Damage class");
    if(PPC_LOAD_U32(event+68))return fail("auxiliary Damage semantics not mapped");
    if(!Readable(base,0x833DA7E4,4))return fail("registry global unavailable");const auto registry=PPC_LOAD_U32(0x833DA7E4);
    if(!Readable(base,registry,24) || PPC_LOAD_U32(registry+4)!=0x827B58D0 || PPC_LOAD_U32(registry+8)!=0x82A69948)return fail("registry layout changed");
    net::DamageEvent sample;
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
    const auto tag=PPC_LOAD_U32(event+76);bool terminated=false;
    for(unsigned i=0;i<=net::kMaxDamageTag;++i){if(uint64_t(tag)+i>UINT32_MAX || !Readable(base,tag+i,1))return fail("Damage tag unavailable");
        const auto byte=PPC_LOAD_U8(tag+i);if(!byte){terminated=true;break;}sample.tag.push_back(char(byte));}
    if(!terminated || !net::ValidDamageEvent(sample))return fail("unsupported Damage value");
    if(!SampleNativeEntityLifetimeRevision(after) || before!=after || PPC_LOAD_U32(0x833DA7E4)!=registry)return fail("registry changed during capture");
    out=std::move(sample);return true;
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
    for(unsigned mutation=0;mutation<8;++mutation){std::memcpy(base,memory.data(),memory.size());
        if(mutation==0)PPC_STORE_U32(event+8,0x55555555);if(mutation==1)PPC_STORE_U32(event+68,1);
        if(mutation==2)PPC_STORE_U32(event+108,2);if(mutation==3)PPC_STORE_U32(event+104,0x100000);
        if(mutation==4)PPC_STORE_U32(event+48,0x7fc00000);if(mutation==5)PPC_STORE_U32(event+76,0x100000);
        if(mutation==6)PPC_STORE_U32(0x4000,1);if(mutation==7)PPC_STORE_U32(0x3004,0x4001);
        if(!check(!CaptureNativeDamageEvent(base,event,sample,error) && error && sample==good,"missing identities, unmapped auxiliary data, malformed arrays/floats/tags fail without changing output"))return false;
    }
    REXLOG_INFO("Native Damage capture passed {} original-hash, portable identity, exact payload and read-only failure checks; native replay and event transport are separate work",checks);return net::TestDamageEventCodec();
}

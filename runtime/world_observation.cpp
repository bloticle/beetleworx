#include "guest_read_batch.h"
#include "world_observation.h"
#include <rex/logging.h>
#include <immintrin.h>
#include <windows.h>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

PPC_EXTERN_FUNC(__imp__sub_82A2EBD0);
PPC_EXTERN_FUNC(__imp__sub_827744A0);
PPC_EXTERN_FUNC(__imp__sub_827744B8);
PPC_EXTERN_FUNC(__imp__sub_82769EF8);
PPC_EXTERN_FUNC(__imp__sub_827B58D0);
PPC_EXTERN_FUNC(__imp__sub_82A69948);
PPC_EXTERN_FUNC(__imp__sub_82732A18);
PPC_EXTERN_FUNC(__imp__sub_82793B50);
PPC_EXTERN_FUNC(__imp__sub_82793B88);
PPC_EXTERN_FUNC(__imp__sub_82793BA8);
namespace {
thread_local unsigned resource_observation_depth=0;
struct ResourceObservationScope {
    ResourceObservationScope(){++resource_observation_depth;}
    ~ResourceObservationScope(){--resource_observation_depth;}
};
bool Readable(uint8_t* base,uint32_t address,size_t size){
    if(!base || address<4096 || !size || uint64_t(address)+size>0x100000000ull)return false;
    return em2::GuestReadBatch::Readable(PPC_RAW_ADDR(address),size);
}
bool Name(uint8_t* base,uint32_t pointer,std::array<char,128>& out,bool optional=false){
    if(!pointer)return optional;
    const bool contiguous=Readable(base,pointer,out.size());
    for(unsigned i=0;i<out.size();++i){
        if(uint64_t(pointer)+i>UINT32_MAX || (!contiguous && !Readable(base,pointer+i,1)))return false;
        const auto c=PPC_LOAD_U8(pointer+i);if(!c)return i!=0 || optional;if(c<32 || c>=127)return false;out[i]=char(c);
    }return false;
}
uint32_t ResolveChecked(PPCContext& ctx,uint8_t* base,uint32_t registry,uint32_t key){
    if(ctx.r1.u32<512 || !Readable(base,registry,24) || !Readable(base,key,4) || !Readable(base,ctx.r1.u32-512,512))return 0;
    const auto count=PPC_LOAD_U32(registry+20),buckets=PPC_LOAD_U32(registry+16),handle=PPC_LOAD_U32(key);
    if(!handle || !count || count>1048576 || PPC_LOAD_U32(registry+4)!=0x827B58D0 || PPC_LOAD_U32(registry+8)!=0x82A69948 || !Readable(base,buckets,size_t(count)*4))return 0;
    const auto saved=ctx;const auto csr=_mm_getcsr();ctx.r3.u64=key;__imp__sub_827B58D0(ctx,base);const auto index=ctx.r3.u32%count;ctx=saved;_mm_setcsr(csr);
    auto node=PPC_LOAD_U32(buckets+index*4);
    // Retail hash/equality and node layout, with a bounded walk instead of
    // blindly dispatching through a freed table or following a cycle forever.
    for(unsigned visited=0;node && visited<4096;++visited){
        if(!Readable(base,node,12))return 0;
        if(PPC_LOAD_U32(node)==handle){const auto entity=PPC_LOAD_U32(node+4);return !(entity&3) && Readable(base,entity,96)?entity:0;}
        const auto next=PPC_LOAD_U32(node+8);if(next==node)return 0;node=next;
    }return 0;
}
template<class Health>
bool SampleResources(PPCContext& ctx,uint8_t* base,const std::array<uint32_t,2>& entities,
                     std::array<uint32_t,4>& life,std::array<float,6>& ammo,Health health) {
    ResourceObservationScope observation_scope;
    em2::GuestReadBatch inspection;
    const auto saved=ctx;const auto csr=_mm_getcsr();
    struct Restore {PPCContext& ctx;PPCContext saved;unsigned csr;~Restore(){ctx=saved;_mm_setcsr(csr);}} restore{ctx,saved,csr};
    if(!Readable(base,0x833DAB60,4) || !Readable(base,0x833DADA0,4) || !Readable(base,0x833DBB13,5))return false;
    const auto manager=PPC_LOAD_U32(0x833DAB60),resources=PPC_LOAD_U32(0x833DADA0);
    if(!Readable(base,manager,640) || !Readable(base,resources,148))return false;
    std::array<uint32_t,4> sampled_life{};std::array<float,6> sampled_ammo{};
    const auto health_type=PPC_LOAD_U8(0x833DBB13);
    for(unsigned player=0;player<2;++player) {
        const auto entity=entities[player];if(!Readable(base,entity,96))return false;
        const auto types=PPC_LOAD_U32(entity+88),components=PPC_LOAD_U32(entity+36);
        if(!Readable(base,types,size_t(health_type)+1))return false;
        const auto slot=PPC_LOAD_U8(types+health_type);
        if(slot==255 || !Readable(base,components,unsigned(slot)*4+4))return false;
        const auto entry=PPC_LOAD_U32(components+unsigned(slot)*4);
        if(entry<8 || !Readable(base,entry,4))return false;
        const auto table=PPC_LOAD_U32(entry),property=PPC_LOAD_U32(0x833DBB14);
        if(!Readable(base,table,208) || !Readable(base,property,8))return false;
        const auto method=PPC_LOAD_U32(table+204);
        if((method&3) || method<PPC_CODE_BASE || method>=PPC_CODE_BASE+PPC_CODE_SIZE)return false;
        ctx=saved;ctx.r3.u64=player;ctx.r4.u64=0;
        {em2::GuestReadBatch::CallbackBoundary callback;health(ctx,base);}
        if(!Readable(base,manager+player*320+208,4))return false;
        const auto current=ctx.f1.f64;
        if(!std::isfinite(current) || current<0 || current>double(UINT32_MAX) || std::floor(current)!=current)return false;
        sampled_life[player*2]=uint32_t(current);
        sampled_life[player*2+1]=PPC_LOAD_U32(manager+player*320+208);
    }
    if(!Readable(base,resources,148))return false;
    for(unsigned channel=0;channel<3;++channel)for(unsigned field=0;field<2;++field) {
        const auto value=std::bit_cast<float>(PPC_LOAD_U32(resources+channel*40+60+field*4));
        if(!std::isfinite(value))return false;
        sampled_ammo[channel*2+field]=value;
    }
    life=sampled_life;ammo=sampled_ammo;return true;
}
template<class Health>
bool Capture(PPCContext& ctx,uint8_t* base,em2::net::GameplayObservation& out,Health health){
    em2::GuestReadBatch inspection;
    const auto saved=ctx;const auto csr=_mm_getcsr();
    struct Restore {PPCContext& c;PPCContext saved;unsigned csr;~Restore(){c=saved;_mm_setcsr(csr);}} restore{ctx,saved,csr};
    em2::net::GameplayObservation sampled;
    if(!em2::ReadNativeWorldIdentity(base,sampled.world) || !Readable(base,0x833DAB60,4) || !Readable(base,0x833DA7E4,4) || !Readable(base,0x833DADA0,4) || !Readable(base,0x833DBC3C,1))return false;
    const auto manager=PPC_LOAD_U32(0x833DAB60),registry=PPC_LOAD_U32(0x833DA7E4),resources=PPC_LOAD_U32(0x833DADA0);
    if(!Readable(base,manager,640))return false;
    std::array<uint32_t,2> entities{};const auto type=PPC_LOAD_U8(0x833DBC3C);
    for(unsigned i=0;i<2;++i){
        const auto entity=ResolveChecked(ctx,base,registry,manager+i*320+20);if(!entity)return false;entities[i]=entity;
        for(unsigned word=0;word<4;++word)sampled.player_ids[i][word]=PPC_LOAD_U32(entity+4+word*4);
        const auto types=PPC_LOAD_U32(entity+88),components=PPC_LOAD_U32(entity+36);
        if(!Readable(base,types,unsigned(type)+1))return false;
        const auto slot=PPC_LOAD_U8(types+type);if(slot==255 || !Readable(base,components,unsigned(slot)*4+4))return false;
        const auto entry=PPC_LOAD_U32(components+unsigned(slot)*4);if(entry<8 || !Readable(base,entry-8,184))return false;
        for(unsigned axis=0;axis<3;++axis){const auto value=std::bit_cast<float>(PPC_LOAD_U32(entry-8+172+axis*4));if(!std::isfinite(value))return false;sampled.players[i].position[axis]=value;}
        sampled.players[i].mode=PPC_LOAD_U32(manager+i*320+32);
    }
    if(!SampleResources(ctx,base,entities,sampled.life,sampled.ammo,health))return false;
    if(PPC_LOAD_U32(0x833DAB60)!=manager || PPC_LOAD_U32(0x833DA7E4)!=registry || PPC_LOAD_U32(0x833DADA0)!=resources)return false;
    for(unsigned i=0;i<2;++i)if(ResolveChecked(ctx,base,registry,manager+i*320+20)!=entities[i])return false;
    em2::net::GameplayObservation::World after;
    if(!em2::ReadNativeWorldIdentity(base,after) || after!=sampled.world)return false;
    out=sampled;return true;
}
}
bool em2::ReadNativeWorldIdentity(uint8_t* base,net::GameplayObservation::World& out){
    GuestReadBatch inspection;
    if(!Readable(base,0x833EFA38,4))return false;
    const auto application=PPC_LOAD_U32(0x833EFA38);if(!Readable(base,application,236))return false;
    const auto world=PPC_LOAD_U32(application+232);if(!Readable(base,world,196))return false;
    const auto active=PPC_LOAD_U32(world+144),interior=PPC_LOAD_U32(world+168);
    if((active && !Readable(base,active,12)) || (interior && !Readable(base,interior,4)))return false;
    net::GameplayObservation::World sampled;
    if(!Name(base,PPC_LOAD_U32(active?active:world+184),sampled.level) ||
       !Name(base,PPC_LOAD_U32(active?active+8:world+188),sampled.state) ||
       !Name(base,PPC_LOAD_U32(interior?interior:world+192),sampled.interior,true))return false;
    out=sampled;return true;
}
bool em2::NativeResourceObservationActive(){return resource_observation_depth!=0;}
bool em2::ReadNativeGameplayObservation(PPCContext& ctx,uint8_t* base,net::GameplayObservation& out){return Capture(ctx,base,out,__imp__sub_82A2EBD0);}
bool em2::ReadNativeResources(PPCContext& ctx,uint8_t* base,const std::array<uint32_t,2>& entities,
                              std::array<uint32_t,4>& life,std::array<float,6>& ammo) {
    return SampleResources(ctx,base,entities,life,ammo,__imp__sub_82A2EBD0);
}
bool em2::TestNativeResources() {
    auto check=[](bool ok,const char* text){if(!ok)REXLOG_ERROR("Native resources test: {}",text);return ok;};
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));if(!base)return false;
    struct Release {uint8_t* base;unsigned csr=_mm_getcsr();~Release(){_mm_setcsr(csr);VirtualFree(base,0,MEM_RELEASE);}} release{base};
    if(!VirtualAlloc(base,0x10000,MEM_COMMIT,PAGE_READWRITE) || !VirtualAlloc(base+0x833D0000,0x10000,MEM_COMMIT,PAGE_READWRITE) || !VirtualAlloc(base+0x833E0000,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
    constexpr uint32_t manager=0x1000,resources=0x2000;
    PPC_STORE_U32(0x833DAB60,manager);PPC_STORE_U32(0x833DADA0,resources);PPC_STORE_U8(0x833DBB13,3);
    PPC_STORE_U32(0x833DBB14,0x9000);PPC_STORE_U32(0x8500+204,PPC_CODE_BASE);
    const std::array<uint32_t,2> entities{0x4000,0x5000};
    for(unsigned player=0;player<2;++player) {
        const auto types=0x6000+player*256,components=0x7000+player*256;
        PPC_STORE_U32(entities[player]+88,types);PPC_STORE_U32(entities[player]+36,components);
        PPC_STORE_U8(types+3,2);PPC_STORE_U32(components+8,0x8008+player*256);
        PPC_STORE_U32(0x8008+player*256,0x8500);
        PPC_STORE_U32(manager+player*320+208,5+player*3);
    }
    const std::array<float,6> expected_ammo{12.5f,20.f,7.25f,15.f,3.f,8.f};
    for(unsigned i=0;i<6;++i)PPC_STORE_U32(resources+(i/2)*40+60+(i%2)*4,std::bit_cast<uint32_t>(expected_ammo[i]));
    PPCContext ctx{};ctx.r1.u64=0xF000;ctx.r31.u64=0x12345678;ctx.lr=0x87654321;ctx.fpscr.InitHost();
    const auto expected=ctx;const auto csr=_mm_getcsr();unsigned calls=0;bool args=true;bool invalid_health=false;
    auto health=[&](PPCContext& native,uint8_t* memory) {
        const auto player=native.r3.u32;
        args &= memory==base && player<2 && native.r4.u32==0 && NativeResourceObservationActive();++calls;
        native.f1.f64=invalid_health?NAN:double(4+player*3);native.r31.u64=0xDEADBEEF;native.lr=0;
        _mm_setcsr(_mm_getcsr()^0x8000);
    };
    if(!check(!NativeResourceObservationActive(),"resource observation scope starts inactive"))return false;
    std::array<uint32_t,4> life{};std::array<float,6> ammo{};
    if(!check(SampleResources(ctx,base,entities,life,ammo,health) && args && calls==2 && life==std::array<uint32_t,4>{4,5,7,8} &&
        ammo==expected_ammo && !std::memcmp(&ctx,&expected,sizeof(ctx)) && _mm_getcsr()==csr,"resource capture uses correct channels and restores complete guest context/FP state"))return false;
    {
        const auto saved_life=life;const auto saved_ammo=ammo;DWORD previous=0;bool changed=false;
        auto protect_manager=[&](PPCContext& native,uint8_t*){
            native.f1.f64=4;changed=VirtualProtect(base+manager,4096,PAGE_NOACCESS,&previous)!=0;
        };
        const bool accepted=SampleResources(ctx,base,entities,life,ammo,protect_manager);
        DWORD ignored=0;const bool restored=!changed || VirtualProtect(base+manager,4096,previous,&ignored);
        if(!check(changed && restored && !accepted && life==saved_life && ammo==saved_ammo &&
            !std::memcmp(&ctx,&expected,sizeof(ctx)) && _mm_getcsr()==csr,
            "health callbacks invalidate cached mappings before subsequent life reads and preserve failed outputs"))return false;
    }
    for(unsigned channel=0;channel<3;++channel) {
        for(unsigned field=0;field<2;++field) {
            ctx=expected;ctx.r3.u64=resources;ctx.r4.u64=channel;
            if(field)__imp__sub_827744B8(ctx,base);else __imp__sub_827744A0(ctx,base);
            if(!check(ctx.f1.f64==double(ammo[channel*2+field]),"ammo capture matches original native getter"))return false;
        }
        if(channel<2) {
            ctx=expected;ctx.r3.u64=manager;ctx.r4.u64=channel;__imp__sub_82769EF8(ctx,base);
            if(!check(ctx.r3.u32==life[channel*2+1],"life capacity matches original native player getter"))return false;
        }
    }
    ctx=expected;const auto good_life=life;const auto good_ammo=ammo;
    invalid_health=true;
    if(!check(!SampleResources(ctx,base,entities,life,ammo,health) && life==good_life && ammo==good_ammo &&
        !std::memcmp(&ctx,&expected,sizeof(ctx)) && _mm_getcsr()==csr,"invalid health does not publish partial values or corrupt guest state"))return false;
    if(!check(!NativeResourceObservationActive(),"resource observation scope unwinds after success and failed reads"))return false;
    invalid_health=false;PPC_STORE_U8(0x6003,255);
    if(!check(!SampleResources(ctx,base,entities,life,ammo,health),"missing health component is not reported as zero life"))return false;
    PPC_STORE_U8(0x6003,2);PPC_STORE_U32(resources+60,std::bit_cast<uint32_t>(std::numeric_limits<float>::infinity()));
    if(!check(!SampleResources(ctx,base,entities,life,ammo,health) && life==good_life && ammo==good_ammo,"invalid ammo does not publish a partial observation"))return false;
    PPC_STORE_U32(resources+60,std::bit_cast<uint32_t>(expected_ammo[0]));
    const auto before_life=life;const auto before_ammo=ammo;
    for(unsigned invalid=0;invalid<6;++invalid){
        if(invalid==0)PPC_STORE_U32(0x833DAB60,0x10000);
        if(invalid==1)PPC_STORE_U32(0x833DADA0,0xFFFFFFD0);
        if(invalid==2)PPC_STORE_U32(entities[0]+88,0x10000);
        if(invalid==3)PPC_STORE_U32(entities[0]+36,0xFFFFFFFC);
        if(invalid==4)PPC_STORE_U32(0x7008,0x10000);
        if(invalid==5)PPC_STORE_U32(0x8500+204,0xFFFFFFFF);
        if(!check(!SampleResources(ctx,base,entities,life,ammo,health) && life==before_life && ammo==before_ammo &&
            !std::memcmp(&ctx,&expected,sizeof(ctx)) && _mm_getcsr()==csr,"unmapped/wrapping player/resource/type/component/method paths fail without partial publication"))return false;
        PPC_STORE_U32(0x833DAB60,manager);PPC_STORE_U32(0x833DADA0,resources);PPC_STORE_U32(entities[0]+88,0x6000);PPC_STORE_U32(entities[0]+36,0x7000);PPC_STORE_U32(0x7008,0x8008);PPC_STORE_U32(0x8500+204,PPC_CODE_BASE);
    }
    constexpr uint32_t application=0xA000,world=0xA800,active=0xB000,interior=0xB100;
    PPC_STORE_U32(0x833EFA38,application);PPC_STORE_U32(application+232,world);
    PPC_STORE_U32(world+144,active);PPC_STORE_U32(world+168,interior);
    for(auto field:{active,active+8,interior,world+184,world+188,world+192}){
        const auto pointer=0xB200+(field%7)*128;PPC_STORE_U32(field,pointer);std::memcpy(base+pointer,"test_level.test_state",22);
    }
    em2::net::GameplayObservation::World identity;
    for(bool present:{true,false}){
        PPC_STORE_U32(world+144,present?active:0);PPC_STORE_U32(world+168,present?interior:0);
        const auto before_queries=GuestReadBatch::Counts();
        if(!check(ReadNativeWorldIdentity(base,identity),"active and fallback world records are readable"))return false;
        const auto after_queries=GuestReadBatch::Counts();
        if(!check(after_queries[0]-before_queries[0]<=4 && after_queries[1]-before_queries[1]>=2,
            "world identity reuses validated regions instead of querying every field"))return false;
        unsigned index=0;for(auto* getter:{__imp__sub_82793B50,__imp__sub_82793B88,__imp__sub_82793BA8}){
            ctx=expected;ctx.r3.u64=world;getter(ctx,base);std::array<char,128> name{};
            if(!check(Name(base,PPC_LOAD_U32(ctx.r3.u32),name) && name==(index==0?identity.level:index==1?identity.state:identity.interior),"world identity agrees with original level/state/interior getters"))return false;++index;
        }
    }
    ctx=expected;PPC_STORE_U32(world+144,active);PPC_STORE_U32(world+168,interior);
    const auto identity_before=identity;PPC_STORE_U32(active,0xFFF0);std::memset(base+0xFFF0,'X',16);
    if(!check(!ReadNativeWorldIdentity(base,identity) && identity==identity_before,"a name crossing into uncommitted memory is rejected without publication"))return false;
    PPC_STORE_U32(active,0xB800);std::memcpy(base+0xB800,"test_level",11);
    constexpr uint32_t registry=0xC000,buckets=0xC100;
    for(const auto callback:{0x827B58D0u,0x82A69948u}){
        const auto offset=PPC_IMAGE_BASE+PPC_IMAGE_SIZE+uint64_t(callback-PPC_CODE_BASE)*2;
        if(!VirtualAlloc(base+(offset&~uint64_t(4095)),4096,MEM_COMMIT,PAGE_READWRITE))return false;
    }
    PPC_LOOKUP_FUNC(base,0x827B58D0)=__imp__sub_827B58D0;PPC_LOOKUP_FUNC(base,0x82A69948)=__imp__sub_82A69948;
    PPC_STORE_U32(0x833DA7E4,registry);PPC_STORE_U32(registry+4,0x827B58D0);PPC_STORE_U32(registry+8,0x82A69948);PPC_STORE_U32(registry+16,buckets);PPC_STORE_U32(registry+20,4);PPC_STORE_U8(0x833DBC3C,9);
    for(unsigned i=0;i<2;++i){
        const auto key=manager+i*320+20,node=0xC200+i*16,handle=0x11+i*17,position=0xD000+i*256;
        PPC_STORE_U32(key,handle);ctx=expected;ctx.r3.u64=key;__imp__sub_827B58D0(ctx,base);const auto bucket=ctx.r3.u32%4;
        PPC_STORE_U32(node,handle);PPC_STORE_U32(node+4,entities[i]);PPC_STORE_U32(node+8,PPC_LOAD_U32(buckets+bucket*4));PPC_STORE_U32(buckets+bucket*4,node);
        PPC_STORE_U8(0x6000+i*256+9,1);PPC_STORE_U32(0x7000+i*256+4,position+8);
        for(unsigned axis=0;axis<3;++axis)PPC_STORE_U32(position+172+axis*4,std::bit_cast<uint32_t>(float(i*3+axis)));
        ctx=expected;ctx.r3.u64=registry;ctx.r4.u64=key;__imp__sub_82732A18(ctx,base);const auto native=ctx.r3.u32;ctx=expected;
        if(!check(ResolveChecked(ctx,base,registry,key)==native && native==entities[i] && !std::memcmp(&ctx,&expected,sizeof(ctx)),"bounded resolver matches original registry/hash behavior"))return false;
    }
    em2::net::GameplayObservation sample;
    if(!check(Capture(ctx,base,sample,health) && sample.players[1].position==std::array<float,3>{3,4,5} && sample.life==good_life && sample.ammo==good_ammo &&
        !std::memcmp(&ctx,&expected,sizeof(ctx)) && _mm_getcsr()==csr,"full guarded observation captures physical positions, resources and world names"))return false;
    auto unchanged=sample;
    const auto old_state=PPC_LOAD_U32(active+8);std::memcpy(base+0xBA00,"other.state",12);
    auto changed_world=[&](PPCContext& current,uint8_t* memory){health(current,memory);if(current.r3.u32==1)PPC_STORE_U32(active+8,0xBA00);};
    if(!check(!Capture(ctx,base,sample,changed_world) && !std::memcmp(&sample,&unchanged,sizeof(sample)),"world identity changing during a getter cannot publish a mixed snapshot"))return false;
    PPC_STORE_U32(active+8,old_state);
    auto changed_resources=[&](PPCContext& current,uint8_t* memory){health(current,memory);if(current.r3.u32==1)PPC_STORE_U32(0x833DADA0,0x2100);};
    if(!check(!Capture(ctx,base,sample,changed_resources) && !std::memcmp(&sample,&unchanged,sizeof(sample)),"resource ownership changing during a getter cannot publish a mixed snapshot"))return false;
    PPC_STORE_U32(0x833DADA0,resources);
    PPC_STORE_U32(0xC200,0x999);PPC_STORE_U32(0xC210,0x998);PPC_STORE_U32(0xC200+8,0xC210);PPC_STORE_U32(0xC210+8,0xC200);
    if(!check(!Capture(ctx,base,sample,health) && !std::memcmp(&sample,&unchanged,sizeof(sample)),"cyclic nonmatching registry chains cannot hang or publish observations"))return false;
    DWORD previous=0;if(!VirtualProtect(base+0x1000,4096,PAGE_NOACCESS,&previous))return false;
    const bool denied=!SampleResources(ctx,base,entities,life,ammo,health);
    DWORD ignored=0;VirtualProtect(base+0x1000,4096,previous,&ignored);
    if(!check(denied && life==good_life && ammo==good_ammo,"a retired PAGE_NOACCESS manager is rejected before dereference"))return false;
    REXLOG_INFO("Guarded gameplay observation tests passed: original world getters/registry/hash/ammo/max-life parity; positions/resources; missing/wrapping/uncommitted/NOACCESS memory, cycles, changed world/resource ownership, no partial publication and caller context/FP restoration");return true;
}

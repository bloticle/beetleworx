#include "native_player_spawn.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <windows.h>
#include <immintrin.h>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <rex/logging.h>
#include <rex/cvar.h>
REXCVAR_DECLARE(bool,em2_probe_owned_native_player_input);

PPC_EXTERN_FUNC(__imp__sub_8275C160);
PPC_EXTERN_FUNC(__imp__sub_8281F1B0);
PPC_EXTERN_FUNC(__imp__sub_8246B730);
PPC_EXTERN_FUNC(sub_8246B730);
PPC_EXTERN_FUNC(__imp__sub_827D31D8);
PPC_EXTERN_FUNC(sub_827D31D8);
namespace {
struct Calls {PPCFunc* position=__imp__sub_8275C160;PPCFunc* sweep=__imp__sub_8281F1B0;PPCFunc* world=__imp__sub_8246B730;PPCFunc* property=__imp__sub_827D31D8;};
thread_local const Calls* test_calls=nullptr;
struct StartContacts {uint8_t* base;uint32_t world,shape,collector,excluded_root;unsigned visits=0,self_exclusions=0;};
thread_local StartContacts* start_contacts=nullptr;
bool Readable(uint8_t* base,uint32_t at,size_t size) {
    return base && at>=4096 && size && uint64_t(at)+size<=0x100000000ull &&
        em2::GuestReadBatch::Readable(base+at,size);
}
struct Scope {
    PPCContext& ctx;PPCContext saved;unsigned csr=_mm_getcsr();DWORD error=GetLastError();
    explicit Scope(PPCContext& c):ctx(c),saved(c){}
    ~Scope(){ctx=saved;_mm_setcsr(csr);SetLastError(error);}
};
}
PPC_FUNC_IMPL(sub_827D31D8) {
    auto* query=start_contacts;
    const bool self=query && query->base==base && query->excluded_root && ctx.r3.u32==query->excluded_root &&
        (uint32_t(ctx.lr)==0x828E213C || uint32_t(ctx.lr)==0x828E216C);
    (test_calls?test_calls->property:__imp__sub_827D31D8)(ctx,base);
    // Native collector rejection only: preserve the original property lookup,
    // but skip this actual actor's own root during its explicitly scoped query.
    if(self){ctx.r3.u64=1;++query->self_exclusions;}
}
PPC_FUNC_IMPL(sub_8246B730) {
    // Extend the retail shape-query helper with Havok's optional initial
    // contact collector. Only our scoped query may supply it; every normal
    // world cast retains its original arguments and native implementation.
    if(auto* query=start_contacts;query && query->base==base && ctx.r3.u32==query->world && !ctx.r7.u32 &&
        Readable(base,ctx.r4.u32,4) && PPC_LOAD_U32(ctx.r4.u32)==query->shape) {
        ctx.r7.u64=query->collector;++query->visits;
    }
    (test_calls?test_calls->world:__imp__sub_8246B730)(ctx,base);
}
bool em2::FindNativePlayerSpawn(PPCContext& ctx,uint8_t* base,uint32_t entity,
    const std::array<float,3>& preferred,std::array<float,3>& grounded,std::string& error,uint32_t collision_profile_entity,bool exclude_prototype_body) {
    Scope scope(ctx);error.clear();
    auto fail=[&](const char* why){error=why;return false;};
    for(float value:preferred)if(!std::isfinite(value))return fail("nonfinite candidate");
    if(!Readable(base,entity,96) || !Readable(base,0x833DA7E4,4) || ctx.r1.u32<32768 ||
        !Readable(base,ctx.r1.u32-32768,32768))return fail("native spawn context unavailable");
    const auto registry=PPC_LOAD_U32(0x833DA7E4);
    if(!Readable(base,registry,232))return fail("native registry unavailable");
    const auto manager=PPC_LOAD_U32(registry+228);
    if(!Readable(base,manager,44))return fail("native physics manager unavailable");
    const auto world=PPC_LOAD_U32(manager+4),list=PPC_LOAD_U32(manager+36),count=PPC_LOAD_U32(manager+40);
    if(!world || !count || count>4096 || !Readable(base,list,size_t(count)*4) ||
        !Readable(base,0x833DC854,4) || PPC_LOAD_U32(0x833DC854)!=world)
        return fail("native character collision world unavailable");
    uint32_t body=0,profile_body=0;
    if(!collision_profile_entity)collision_profile_entity=entity;
    if(!Readable(base,collision_profile_entity,96))return fail("native collision profile entity unavailable");
    for(uint32_t i=0;i<count;++i) {
        const auto candidate=PPC_LOAD_U32(list+i*4);
        if(!Readable(base,candidate,304) || PPC_LOAD_U8(candidate+232)!=7)continue;
        const auto owner=PPC_LOAD_U32(candidate+12);
        // Exact fast owner path of 827D2BD0; never guess a shape from an
        // unrelated body or use an actor handle as a pointer.
        if(Readable(base,owner,4)){
            const auto owner_entity=PPC_LOAD_U32(owner);
            if(owner_entity==entity)body=candidate;
            if(owner_entity==collision_profile_entity)profile_body=candidate;
        }
        if(body && profile_body)break;
    }
    if(!body)return fail("prototype has no live native character hull");
    if(!profile_body)return fail("collision profile has no live native character hull");
    const auto shape=PPC_LOAD_U32(body+16),filter=PPC_LOAD_U32(profile_body+44);
    if(!Readable(base,shape,32))return fail("native character shape unavailable");
    const auto stack=scope.saved.r1.u32-2048,origin=stack+512,from=stack+544,to=stack+560,result=stack+576,collector=stack+704;
    const Calls calls=test_calls?*test_calls:Calls{};
    ctx.r1.u64=stack;ctx.r3.u64=entity;ctx.r4.u64=origin;calls.position(ctx,base);
    std::array<float,3> offset{},target{};
    constexpr float lift=.75f,drop=2.f;
    for(unsigned i=0;i<3;++i) {
        const float position=std::bit_cast<float>(PPC_LOAD_U32(origin+i*4));
        const float center=std::bit_cast<float>(PPC_LOAD_U32(body+288+i*4));
        if(!std::isfinite(position) || !std::isfinite(center))return fail("invalid prototype transform");
        offset[i]=center-position;target[i]=preferred[i]+offset[i];
    }
    target[1]+=lift;
    if(REXCVAR_GET(em2_probe_owned_native_player_input))
        REXLOG_INFO("Owned spawn hull: entity {:08X} body {:08X} shape {:08X} filter {:08X} offset {}/{}/{} preferred {}/{}/{}",entity,body,shape,filter,offset[0],offset[1],offset[2],preferred[0],preferred[1],preferred[2]);
    if(REXCVAR_GET(em2_probe_owned_native_player_input)){
        REXLOG_INFO("Owned spawn hull rotation: entity {:08X} x {}/{}/{} y {}/{}/{} z {}/{}/{}",entity,
            std::bit_cast<float>(PPC_LOAD_U32(body+240)),std::bit_cast<float>(PPC_LOAD_U32(body+244)),std::bit_cast<float>(PPC_LOAD_U32(body+248)),
            std::bit_cast<float>(PPC_LOAD_U32(body+256)),std::bit_cast<float>(PPC_LOAD_U32(body+260)),std::bit_cast<float>(PPC_LOAD_U32(body+264)),
            std::bit_cast<float>(PPC_LOAD_U32(body+272)),std::bit_cast<float>(PPC_LOAD_U32(body+276)),std::bit_cast<float>(PPC_LOAD_U32(body+280)));
        if(Readable(base,shape,64))REXLOG_INFO("Owned spawn shape words: entity {:08X} {:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}",entity,
            PPC_LOAD_U32(shape),PPC_LOAD_U32(shape+4),PPC_LOAD_U32(shape+8),PPC_LOAD_U32(shape+12),PPC_LOAD_U32(shape+16),PPC_LOAD_U32(shape+20),PPC_LOAD_U32(shape+24),PPC_LOAD_U32(shape+28),
            PPC_LOAD_U32(shape+32),PPC_LOAD_U32(shape+36),PPC_LOAD_U32(shape+40),PPC_LOAD_U32(shape+44),PPC_LOAD_U32(shape+48),PPC_LOAD_U32(shape+52),PPC_LOAD_U32(shape+56),PPC_LOAD_U32(shape+60));
    }
    bool query_ok=false;float penetration=0;
    auto sweep=[&](const std::array<float,3>& a,const std::array<float,3>& b) {
        std::memset(base+from,0,32);std::memset(base+result,0,64);
        for(unsigned i=0;i<3;++i){PPC_STORE_U32(from+i*4,std::bit_cast<uint32_t>(a[i]));PPC_STORE_U32(to+i*4,std::bit_cast<uint32_t>(b[i]));}
        // Exact native collector class/initialization from 8281F1B0. Initial
        // overlaps matter even when a downward cast is tangent to a wall.
        std::memset(base+collector,0,128);PPC_STORE_U32(collector,0x820A2638);
        PPC_STORE_U32(collector+4,std::bit_cast<uint32_t>(std::numeric_limits<float>::max()));
        PPC_STORE_U32(collector+44,std::bit_cast<uint32_t>(std::numeric_limits<float>::max()));
        PPC_STORE_U32(collector+80,filter);
        StartContacts query{base,world,shape,collector,exclude_prototype_body?body+16:0};
        struct RestoreQuery {StartContacts* previous=start_contacts;~RestoreQuery(){start_contacts=previous;}} restore_query;
        start_contacts=&query;
        // Original engine arbitrary-shape sweep. It owns its temporary
        // collidable/collector and releases them before returning. The shape
        // itself remains borrowed from the live original character.
        ctx.r1.u64=stack;ctx.r3.u64=from;ctx.r4.u64=to;ctx.r5.u64=shape;ctx.r6.u64=result;
        ctx.r7.u64=filter;ctx.r8.u64=0;ctx.r9.u64=0;ctx.r10.u64=0;
        calls.sweep(ctx,base);const bool hit=ctx.r3.u32!=0;
        penetration=PPC_LOAD_U32(collector+48)?std::bit_cast<float>(PPC_LOAD_U32(collector+44)):0.f;
        if(REXCVAR_GET(em2_probe_owned_native_player_input) && penetration<-.02f)
            REXLOG_INFO("Owned spawn initial overlap: entity {:08X} prototype root {:08X} roots {:08X}/{:08X} depth {}",entity,body+16,PPC_LOAD_U32(collector+48),PPC_LOAD_U32(collector+56),penetration);
        query_ok=query.visits==1 && std::isfinite(penetration);return hit;
    };
    auto below=target;below[1]-=lift+drop;
    const bool floor=sweep(target,below);
    if(!query_ok)return fail("native initial-contact query incomplete");
    if(penetration<-.02f)return fail("candidate hull starts inside collision");
    if(!floor)return fail("no supporting floor under character hull");
    //8281F1B0 returns the root hit collidable at+32, not an entity handle.
    //Match native827D31D8's signed owner offset and entity-body tag. Initial
    //group admission needs stable level support, not another player's hull,
    //a phantom or an unattached moving body. The image's motion reflection
    //table8202CE68 identifies5 as MOTION_FIXED (7 is MOTION_CHARACTER).
    const auto support=PPC_LOAD_U32(result+32);
    if(!Readable(base,support,25) || PPC_LOAD_U8(support+24)!=1)
        return fail("support is not a native entity body");
    const int64_t support_owner=int64_t(support)+int8_t(PPC_LOAD_U8(support+16));
    if(support_owner<4096 || support_owner+233>0x100000000ll ||
        !Readable(base,uint32_t(support_owner),233) || PPC_LOAD_U8(uint32_t(support_owner)+232)!=5)
        return fail("support is not fixed level collision");
    const float fraction=std::bit_cast<float>(PPC_LOAD_U32(result+40));
    const float normal_y=std::bit_cast<float>(PPC_LOAD_U32(result+20));
    if(REXCVAR_GET(em2_probe_owned_native_player_input))
        REXLOG_INFO("Owned spawn support: root {:08X} owner {:08X} shape key {:08X} fraction {} normal {} contact {}/{}/{}",support,uint32_t(support_owner),PPC_LOAD_U32(result+36),fraction,normal_y,
            std::bit_cast<float>(PPC_LOAD_U32(result)),std::bit_cast<float>(PPC_LOAD_U32(result+4)),std::bit_cast<float>(PPC_LOAD_U32(result+8)));
    if(!std::isfinite(fraction) || fraction<0 || fraction>1 || !std::isfinite(normal_y) || normal_y<.95f)
        return fail("support is too steep for stable group spawning");
    auto output=preferred;
    output[1]=target[1]+(below[1]-target[1])*fraction-offset[1]+.05f;
    if(!std::isfinite(output[1]))return fail("invalid grounded spawn height");
    std::array<float,3> standing=output;
    for(unsigned i=0;i<3;++i)standing[i]+=offset[i];
    auto above=standing;above[1]+=.01f;
    sweep(standing,above);
    if(!query_ok || penetration<-.02f)return fail("grounded character hull overlaps collision");
    grounded=output;return true;
}

bool em2::TestNativePlayerSpawn() {
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));
    if(!base)return false;
    struct Cleanup {uint8_t* base;const Calls* calls=test_calls;unsigned csr=_mm_getcsr();DWORD error=GetLastError();
        ~Cleanup(){test_calls=calls;VirtualFree(base,0,MEM_RELEASE);_mm_setcsr(csr);SetLastError(error);}} cleanup{base};
    if(!VirtualAlloc(base,0x10000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0x833D0000,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
    constexpr uint32_t entity=0x1000,registry=0x2000,manager=0x3000,list=0x4000,body=0x5000,owner=0x6000,shape=0x7000;
    auto put=[&](uint32_t at,float x){PPC_STORE_U32(at,std::bit_cast<uint32_t>(x));};
    PPC_STORE_U32(0x833DA7E4,registry);PPC_STORE_U32(registry+228,manager);
    PPC_STORE_U32(manager+4,0x8000);PPC_STORE_U32(0x833DC854,0x8000);
    PPC_STORE_U32(manager+36,list);PPC_STORE_U32(manager+40,1);PPC_STORE_U32(list,body);
    PPC_STORE_U8(body+232,7);PPC_STORE_U32(body+12,owner);PPC_STORE_U32(owner,entity);
    PPC_STORE_U32(body+16,shape);PPC_STORE_U32(body+44,0x3808000C);
    PPC_STORE_U32(0x4004,0x5200);PPC_STORE_U32(0x5200+12,0x6100);
    PPC_STORE_U32(0x6100,0x1100);PPC_STORE_U8(0x5200+232,7);PPC_STORE_U32(0x5200+44,0x3808000D);
    put(body+288,-39);put(body+292,4);put(body+296,58);
    const auto position=+[](PPCContext& call,uint8_t* base){
        const auto out=call.r4.u32;
        PPC_STORE_U32(out,std::bit_cast<uint32_t>(-39.f));PPC_STORE_U32(out+4,std::bit_cast<uint32_t>(3.f));
        PPC_STORE_U32(out+8,std::bit_cast<uint32_t>(58.f));PPC_STORE_U32(out+12,0);
        call.r31.u64=0xABCD;_mm_setcsr(_mm_getcsr()|0x20);SetLastError(456);
    };
    const auto sweep=+[](PPCContext& call,uint8_t* base){
        const auto n=PPC_LOAD_U32(0x9000),mode=PPC_LOAD_U32(0x9004);PPC_STORE_U32(0x9000,n+1);
        const bool args=call.r5.u32==0x7000 && call.r7.u32==(mode==14?0x3808000Du:0x3808000Cu) && !call.r8.u32 && !call.r9.u32 && !call.r10.u32 &&
            !(call.r3.u32&15) && !(call.r4.u32&15) && !(call.r6.u32&15);
        const auto x=std::bit_cast<float>(PPC_LOAD_U32(call.r3.u32));
        const auto y=std::bit_cast<float>(PPC_LOAD_U32(call.r3.u32+4));
        const auto end_y=std::bit_cast<float>(PPC_LOAD_U32(call.r4.u32+4));
        if(!args || x!=-37.f || std::abs(y-(n?4.1125f:4.75f))>.00001f || std::abs(end_y-(n?4.1225f:2.f))>.00001f)PPC_STORE_U32(0x9008,1);
        const auto output=call.r6.u32;
        if(mode==16){
            const auto saved=call;
            call.r3.u64=0x5010;call.lr=0x828E213C;sub_827D31D8(call,base);
            if(call.r3.u32!=1 || call.r11.u32!=0x1234)PPC_STORE_U32(0x9008,1);
            call.r3.u64=0xB010;call.lr=0x828E216C;sub_827D31D8(call,base);
            if(call.r3.u32)PPC_STORE_U32(0x9008,1);
            call.r3.u64=0x5010;call.lr=0x1234;sub_827D31D8(call,base);
            if(call.r3.u32)PPC_STORE_U32(0x9008,1);
            call=saved;
        }
        PPC_STORE_U32(output+32,mode==12?0u:0xB010u);
        PPC_STORE_U8(0xB010+24,mode==13?2:1);
        PPC_STORE_U8(0xB010+16,0xF0); //Native signed owner offset -16.
        PPC_STORE_U8(0xB000+232,mode==10?7:mode==11?4:5);
        // Exercise the production optional-collector bridge, not a substitute
        // direct write into the query result.
        PPC_STORE_U32(0xA000,0x7000);call.r3.u64=0x8000;call.r4.u64=0xA000;call.r7.u64=0;
        sub_8246B730(call,base);
        PPC_STORE_U32(output+40,std::bit_cast<uint32_t>(mode==4?2.f:.25f));
        PPC_STORE_U32(output+20,std::bit_cast<uint32_t>(mode==3?.2f:mode==17?.8f:1.f));
        call.r3.u64=mode!=2;call.r27.u64=0xBAD;_mm_setcsr(_mm_getcsr()|0x20);SetLastError(789);
    };
    const auto world=+[](PPCContext& call,uint8_t* base){
        const auto c=call.r7.u32,mode=PPC_LOAD_U32(0x9004),count=PPC_LOAD_U32(0x9000);
        if(!c || (c&15) || PPC_LOAD_U32(c)!=0x820A2638 || PPC_LOAD_U32(c+80)!=(mode==14?0x3808000Du:0x3808000Cu)){PPC_STORE_U32(0x9008,1);return;}
        if(mode==1 || (mode==8 && count==2)){PPC_STORE_U32(c+48,0x5000);PPC_STORE_U32(c+44,std::bit_cast<uint32_t>(-.5f));}
    };
    const auto property=+[](PPCContext& call,uint8_t* base){PPC_STORE_U32(0x9010,PPC_LOAD_U32(0x9010)+1);call.r11.u64=0x1234;call.r3.u64=0;};
    const Calls calls{position,sweep,world,property};test_calls=&calls;
    std::array<uint8_t,304> original_body;std::memcpy(original_body.data(),base+body,original_body.size());
    for(unsigned mode=0;mode<18;++mode) {
        PPC_STORE_U32(0x9000,0);PPC_STORE_U32(0x9004,mode);PPC_STORE_U32(0x9008,0);PPC_STORE_U32(0x9010,0);
        PPC_STORE_U32(manager+40,mode==5?4097:(mode==14 || mode==15)?2:1);PPC_STORE_U32(owner,mode==6?entity+8:entity);
        PPC_STORE_U32(0x833DC854,mode==9?0x8004:0x8000);
        PPCContext ctx{};ctx.fpscr.InitHost();ctx.r1.u64=0xF000;ctx.r3.u64=0xAABBCC;ctx.lr=0x12345678;
        std::array<uint8_t,sizeof(PPCContext)> before;std::memcpy(before.data(),&ctx,sizeof(ctx));
        const auto csr=_mm_getcsr();SetLastError(123);
        std::array<float,3> output{17,18,19};std::string error;
        const auto prototype=mode==7?0u:entity;
        const bool found=FindNativePlayerSpawn(ctx,base,prototype,{-37,3,58},output,error,(mode==14 || mode==15)?(mode==14?0x1100:0x1200):0,mode==16);
        const bool correct=found==(mode==0 || mode==14 || mode==16) && (mode!=16 || PPC_LOAD_U32(0x9010)==6) && !std::memcmp(before.data(),&ctx,sizeof(ctx)) && _mm_getcsr()==csr && GetLastError()==123 &&
            !PPC_LOAD_U32(0x9008) && !std::memcmp(original_body.data(),base+body,original_body.size()) &&
            (found?(std::abs(output[1]-3.1125f)<.00001f && output[0]==-37 && output[2]==58 && error.empty()):
                (output==std::array<float,3>{17,18,19} && !error.empty()));
        if(!correct){REXLOG_ERROR("Native spawn fixture failed mode {}",mode);return false;}
    }
    REXLOG_INFO("Native player spawn passed: borrowed hull/filter and aligned native sweep ABI, body/entity offset, blocked path, absent/steep floor, invalid fraction, fixed support ownership and character/moving/phantom rejection, bounded owner/world reads, unchanged body and complete caller/FP/error restoration; live geometry acceptance separate");
    return true;
}

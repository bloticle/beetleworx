#include "online_local_movement.h"
#include "online.h"
#include "online_movement_frame.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>
PPC_EXTERN_FUNC(__imp__sub_827C5990);
PPC_EXTERN_FUNC(__imp__sub_827C3030);
PPC_EXTERN_FUNC(__imp__sub_827C3D78);
PPC_EXTERN_FUNC(__imp__sub_827C43C0);
PPC_EXTERN_FUNC(__imp__sub_82732A18);
PPC_EXTERN_FUNC(__imp__sub_827B58D0);
PPC_EXTERN_FUNC(__imp__sub_82A69948);
PPC_EXTERN_FUNC(__imp__sub_830ACF88);
PPC_EXTERN_FUNC(__imp__sub_830A7C10);
PPC_EXTERN_FUNC(__imp__sub_830A7C70);
PPC_EXTERN_FUNC(__imp__sub_82783208);
PPC_EXTERN_FUNC(__imp__sub_827CA160);
namespace {
using Binding=em2::net::NativeLocalMovementBinding;
struct Owner {uint64_t epoch=0,generation=0;uint32_t registry=0;DWORD thread=0;unsigned user=0;};
thread_local Owner owner;
thread_local Binding latest;
thread_local em2::net::NativeLocalMovementDiagnostics diagnostics;
struct Environment {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(csr);SetLastError(error);}};
bool Readable(uint32_t p,size_t n,uint8_t* base){return base && p>=4096 && uint64_t(p)+n<=0x100000000ull && em2::GuestReadBatch::Readable(base+p,n);}
float Float(uint32_t p,uint8_t* base){return std::bit_cast<float>(PPC_LOAD_U32(p));}
struct Scratch {
    PPCContext& ctx;PPCContext saved;Environment environment;uint8_t* memory=nullptr;
    std::array<uint8_t,8192> bytes{};uint32_t stack=0;
    Scratch(PPCContext& c,uint8_t* base):ctx(c),saved(c){
        const auto top=c.r1.u32&~15u;
        if(top>=12288 && Readable(top-8192,8192,base)){
            memory=base+top-8192;std::memcpy(bytes.data(),memory,bytes.size());stack=top-4096;ctx.r1.u64=stack;
        }
    }
    ~Scratch(){if(memory)std::memcpy(memory,bytes.data(),bytes.size());ctx=saved;}
};
bool Member(uint32_t list,uint32_t count,uint32_t target,uint32_t limit,uint8_t* base){
    if(!count || count>limit || !Readable(list,size_t(count)*4,base))return false;
    unsigned hits=0;for(unsigned i=0;i<count;++i)hits+=PPC_LOAD_U32(list+i*4)==target;
    return hits==1;
}
bool ReferenceShape(Binding& b,uint8_t* base){
    b.reference=0;b.reference_shape={};
    const auto component=b.controller+24,basic=b.controller+16;
    if((PPC_LOAD_U8(component+44)&0x80) || PPC_LOAD_U8(component+233) ||
       PPC_LOAD_U32(component+284) || PPC_LOAD_U8(component+289) ||
       PPC_LOAD_U32(basic)!=0x8211DE70 || !Readable(0x833DAE58,1,base))return false;
    const auto types=PPC_LOAD_U32(b.entity+88),list=PPC_LOAD_U32(b.entity+36);
    const auto kind=PPC_LOAD_U8(0x833DAE58);
    if(!Readable(types,unsigned(kind)+1,base))return false;
    const auto slot=PPC_LOAD_U8(types+kind);
    if(slot==255 || !Readable(list,unsigned(slot)*4+4,base) || PPC_LOAD_U32(list+unsigned(slot)*4)!=component)return false;
    const auto reference=PPC_LOAD_U32(component+72);
    if(reference==b.entity || !Readable(reference,96,base) || PPC_LOAD_U32(reference)!=0x8208D908 ||
       PPC_LOAD_U32(0x8208D908+172)!=0x827CA160 || PPC_LOAD_U32(reference+44)!=2)return false;
    const auto components=PPC_LOAD_U32(reference+36);
    if(!Readable(components,8,base))return false;
    const auto frame=PPC_LOAD_U32(components),camera=PPC_LOAD_U32(components+4);
    if(!Readable(frame,4,base) || !Readable(camera,4,base) ||
       PPC_LOAD_U32(frame)!=0x82090340 || PPC_LOAD_U32(camera)!=0x820D8970)return false;
    std::array<uint32_t,7> shape{};
    for(unsigned i=0;i<4;++i)shape[i]=PPC_LOAD_U32(reference+4+i*4);
    if(!(shape[0]|shape[1]|shape[2]|shape[3]))return false;
    shape[4]=components;shape[5]=frame;shape[6]=camera;
    b.reference=reference;b.reference_shape=shape;return true;
}
bool Inspect(Binding& b,PPCContext& ctx,uint8_t* base){
    em2::GuestReadBatch reads;
    diagnostics.stage=10;
    if(!owner.epoch || owner.thread!=GetCurrentThreadId() || b.world_epoch!=owner.epoch ||
       b.producer_generation!=owner.generation || b.registry!=owner.registry ||
       !Readable(0x833DA7E4,4,base) || PPC_LOAD_U32(0x833DA7E4)!=b.registry ||
       !Readable(b.registry,24,base) || !Readable(0x833DBAD8,4,base) ||
       PPC_LOAD_U32(0x833DBAD8)!=b.manager || !Readable(b.manager,76,base))return false;
    diagnostics.stage=20;b.player_manager=PPC_LOAD_U32(0x833DAB60);
    if(!Readable(b.player_manager,640,base))return false;
    b.handle=PPC_LOAD_U32(b.player_manager+20);if(!b.handle)return false;
    {
        Scratch scratch(ctx,base);if(!scratch.memory)return false;
        const auto key=scratch.stack+512;PPC_STORE_U32(key,b.handle);
        ctx.r3.u64=b.registry;ctx.r4.u64=key;__imp__sub_82732A18(ctx,base);b.entity=ctx.r3.u32;
    }
    diagnostics.stage=30;
    if(!Readable(b.entity,96,base) || !Readable(0x833E2188,1,base))return false;
    const auto types=PPC_LOAD_U32(b.entity+88),list=PPC_LOAD_U32(b.entity+36);
    const auto kind=PPC_LOAD_U8(0x833E2188);
    if(!Readable(types,unsigned(kind)+1,base))return false;
    const auto slot=PPC_LOAD_U8(types+kind);if(slot==255 || !Readable(list,unsigned(slot)*4+4,base))return false;
    const auto face=PPC_LOAD_U32(list+unsigned(slot)*4);
    if(face<24 || !Readable(face-24,800,base) || PPC_LOAD_U32(face)!=0x8211DCE0)return false;
    b.controller=face-24;
    if(b.listener!=b.controller+80 || PPC_LOAD_U32(b.listener)!=0x8211DCD0 ||
       PPC_LOAD_U32(0x8211DCD0)!=0x82A3C930 || PPC_LOAD_U32(b.controller+64)!=b.entity-8)return false;
    diagnostics.stage=40;
    if(!Readable(b.action,436,base) || !Readable(b.event,24,base) ||
       !Readable(0x833DAEB8,4,base) || !PPC_LOAD_U32(0x833DAEB8) ||
       PPC_LOAD_U32(b.action)!=PPC_LOAD_U32(0x833DAEB8) ||
       PPC_LOAD_U32(b.action+80)!=4 || PPC_LOAD_U32(b.action+408)!=2 ||
       PPC_LOAD_U32(b.action+412)!=0)return false; // Modifier conditions need their own producer proof.
    b.movement_uid=PPC_LOAD_U32(b.action);
    diagnostics.stage=50;
    // ORIGINAL75B0 dispatches inline default(manager+24) and the selected
    // named context(manager+16), only if the latter actually resolves.
    const auto event_count=PPC_LOAD_U32(b.manager+8),events=PPC_LOAD_U32(b.manager+4);
    if(event_count>128 || (event_count && !Readable(events,size_t(event_count)*4,base)))return false;
    for(unsigned i=0;i<event_count;++i)if(!Readable(PPC_LOAD_U32(events+i*4),4,base))return false;
    b.active_event_uid=PPC_LOAD_U32(b.manager+16);
    {
        Scratch scratch(ctx,base);if(!scratch.memory)return false;
        ctx.r3.u64=b.manager;ctx.r4.u64=b.manager+16;__imp__sub_827C3D78(ctx,base);b.active_event=ctx.r3.u32;
    }
    if(!b.active_event || (b.event!=b.active_event && b.event!=b.manager+24))return false;
    b.event_shape={PPC_LOAD_U32(b.event+4),PPC_LOAD_U32(b.event+8),PPC_LOAD_U32(b.event+16),PPC_LOAD_U32(b.event+20)};
    // The live retail Movement event contains 141 actions and 14 listeners.
    // Bound these independent lists separately; retain exact-one membership.
    if(!Member(b.event_shape[0],b.event_shape[1],b.listener,128,base) ||
       !Member(PPC_LOAD_U32(b.event+16),PPC_LOAD_U32(b.event+20),b.action,256,base))return false;
    diagnostics.stage=60;
    const auto first_user=PPC_LOAD_U32(b.action+88);
    if(first_user>1 || PPC_LOAD_U32(b.action+120)!=first_user)return false;
    b.logical_user=first_user;
    if(!Readable(0x833DC87C,4,base))return false;
    b.profile=PPC_LOAD_U32(0x833DC87C);if(!Readable(b.profile,16,base))return false;
    const auto user_slot=PPC_LOAD_U32(b.profile+4+first_user*4),users=PPC_LOAD_U32(b.profile+12);
    if(user_slot>=4 || !Readable(users,16,base))return false;
    b.user_object=PPC_LOAD_U32(users+user_slot*4);
    if(!Readable(b.user_object,4,base) || !Readable(PPC_LOAD_U32(b.user_object),12,base))return false;
    {
        Scratch scratch(ctx,base);if(!scratch.memory)return false;const auto list=scratch.stack+512;
        std::memset(base+list,0,20);ctx.r3.u64=b.manager;ctx.r4.u64=0x02400017;
        ctx.r5.u64=first_user;ctx.r6.u64=list;ctx.r7.u64=0;
        __imp__sub_827C43C0(ctx,base);
        if(PPC_LOAD_U32(list+16)!=1)return false;b.user=PPC_LOAD_U32(list);
    }
    if(b.user!=owner.user)return false;
    b.input=PPC_LOAD_U32(b.manager+72);
    if(!Readable(b.input,88,base))return false;
    b.device=PPC_LOAD_U32(b.input+72+b.user*4);
    if(!Readable(b.device,72,base) || PPC_LOAD_U32(b.device+56)<2)return false;
    const auto history=PPC_LOAD_U32(b.device+60);if(!Readable(history,64,base))return false;
    // Actual 827C2D98 axis codes. Dest selectors and inversion are kept native;
    // do not import the look action's particular X/Y destination assumptions.
    diagnostics.stage=70;bool axes[2]{};
    for(unsigned i=0;i<2;++i){const auto p=b.action+88+i*32,key=PPC_LOAD_U32(p+4);
        const unsigned axis=key==0x02400017?0:key==0x02400018?1:2;
        if(axis==2 || axes[axis] || PPC_LOAD_U32(p+8)>5 || PPC_LOAD_U8(p+16)>1)return false;
        axes[axis]=true;
    }
    diagnostics.stage=80;unsigned n=0;b.action_shape[n++]=PPC_LOAD_U32(b.action+80);
    b.action_shape[n++]=PPC_LOAD_U32(b.action+408);b.action_shape[n++]=PPC_LOAD_U32(b.action+412);
    for(unsigned off=88;off<152;off+=4)b.action_shape[n++]=PPC_LOAD_U32(b.action+off);
    for(unsigned off=416;off<=432;off+=4)b.action_shape[n++]=PPC_LOAD_U32(b.action+off);
    const float deadzone=Float(b.action+416,base),saturation=Float(b.action+420,base),power=Float(b.action+424,base),gain=Float(b.action+428,base);
    if(!std::isfinite(deadzone) || !std::isfinite(saturation) || !std::isfinite(power) || !std::isfinite(gain) ||
       deadzone<0 || saturation<=deadzone || saturation>4 || power<=0 || power>16 || gain<0 || gain>8 || PPC_LOAD_U32(b.action+432)>2)return false;
    diagnostics.stage=90;n=0;for(auto p:{b.manager,b.input,b.device})b.device_shape[n++]=PPC_LOAD_U32(p);
    b.device_shape[n++]=history;b.device_shape[n++]=PPC_LOAD_U32(b.device+56);b.device_shape[n++]=PPC_LOAD_U32(b.device+68);
    for(unsigned axis=0;axis<2;++axis)for(unsigned off=16;off<=28;off+=4)b.device_shape[n++]=PPC_LOAD_U32(history+axis*32+off);
    for(unsigned axis=0;axis<2;++axis){const auto p=history+axis*32;
        if(int32_t(PPC_LOAD_U32(p+16))!=-32768 || PPC_LOAD_U32(p+20)!=32767 ||
           int32_t(PPC_LOAD_U32(p+24))!=-100 || PPC_LOAD_U32(p+28)!=100)return false;
    }
    // A math-only binding can exist before the ordinary camera is ready. A
    // complete Room producer additionally requires this pinned native identity.
    ReferenceShape(b,base);
    diagnostics.stage=100;return true;
}
struct Query {Binding binding;std::array<float,2> axes{};unsigned hits=0;};
thread_local Query* query=nullptr;
struct QueryScope {Query* saved=query;explicit QueryScope(Query& q){query=&q;}~QueryScope(){query=saved;}};
bool Override(PPCContext& ctx){
    if(!query || ctx.lr!=0x827C5810 || ctx.r3.u32!=query->binding.manager ||
       ctx.r4.u32!=query->binding.user || ctx.r5.u32!=query->binding.device)return false;
    const auto key=ctx.r6.u32;const unsigned axis=key==0x02400017?0:key==0x02400018?1:2;
    if(axis==2)return false;
    ctx.fpscr.disableFlushMode();ctx.f1.f64=query->axes[axis];++query->hits;return true;
}
// Same signed device stage proved against LEFT axes of ORIGINAL830ACF88 below.
float Axis(int16_t input,bool vertical,float deadzone){
    int value=vertical?-int(input):int(input);
    if(std::abs(value)<=int(float(double(deadzone)*32768.)))value=0;
    const float fraction=float(double(float(value+32768))/65535.);
    const int output=int(float(double(fraction)*200.))-100;
    return float(double(float(output))*double(.01f));
}
bool Math(const Binding& binding,const std::array<float,2>& axes,PPCContext& ctx,uint8_t* base,
          em2::net::NativeActorAction& output,bool inject=true,PPCFunc* original=__imp__sub_827C5990){
    if(query)return false;Scratch scratch(ctx,base);if(!scratch.memory)return false;
    const auto x=scratch.stack+512,y=x+4;PPC_STORE_U32(x,0);PPC_STORE_U32(y,0);
    Query q{binding,axes};QueryScope scope(q);if(!inject)query=nullptr;
    ctx.r3.u64=binding.manager;ctx.r4.u64=binding.action;ctx.r5.u64=1;ctx.r6.u64=x;ctx.r7.u64=y;
    original(ctx,base);
    const float a=Float(x,base),b=Float(y,base);
    diagnostics.math_return=ctx.r3.u32;diagnostics.axis_queries=q.hits;diagnostics.math_x=a;diagnostics.math_y=b;
    if(ctx.r3.u32!=1 || (inject && q.hits!=2) || !std::isfinite(a) || !std::isfinite(b) || std::abs(a)>1 || std::abs(b)>1)return false;
    output={binding.event,binding.action,a,b};return true;
}
}
PPC_FUNC_IMPL(sub_827C3030){if(!Override(ctx))__imp__sub_827C3030(ctx,base);}
bool em2::net::ArmNativeLocalMovement(uint64_t epoch,uint32_t registry,uint8_t* base,unsigned selected_user){
    Environment preserve;
    if(!epoch || selected_user>3 || query || owner.generation==UINT64_MAX || !Readable(0x833DA7E4,4,base) ||
       !registry || PPC_LOAD_U32(0x833DA7E4)!=registry)return false;
    owner={epoch,owner.generation+1,registry,GetCurrentThreadId(),selected_user};latest={};diagnostics={};return true;
}
void em2::net::DisarmNativeLocalMovement(){const auto generation=owner.generation;owner={};owner.generation=generation;latest={};}
bool em2::net::CaptureNativeLocalMovementBinding(PPCContext& ctx,uint8_t* base,Binding& output){
    Environment preserve;output={};
    diagnostics.stage=1;
    if(query || ctx.lr!=0x827C439C || GameplayInputGate().active() || !Readable(0x833DBAD8,4,base))return false;
    Binding b;b.world_epoch=owner.epoch;b.producer_generation=owner.generation;b.registry=owner.registry;
    b.manager=PPC_LOAD_U32(0x833DBAD8);b.listener=ctx.r3.u32;b.event=ctx.r4.u32;b.action=ctx.r5.u32;
    if(!Inspect(b,ctx,base))return false;output=b;return true;
}
void em2::net::ObserveNativeLocalMovementBinding(PPCContext& ctx,uint8_t* base) noexcept {
    if(!owner.epoch)return;
    Environment preserve;
    ++diagnostics.observations;diagnostics.lr=uint32_t(ctx.lr);diagnostics.listener=ctx.r3.u32;
    diagnostics.event=ctx.r4.u32;diagnostics.action=ctx.r5.u32;
    try{Binding binding;if(CaptureNativeLocalMovementBinding(ctx,base,binding)){latest=binding;++diagnostics.captured;}}catch(...){latest={};}
    diagnostics.best_stage=std::max(diagnostics.best_stage,diagnostics.stage);
}
em2::net::NativeLocalMovementDiagnostics em2::net::LocalMovementDiagnostics() noexcept{return diagnostics;}
bool em2::net::NativeLocalMovementBindingFor(PPCContext& ctx,uint8_t* base,Binding& output){
    Environment preserve;output={};if(!owner.epoch || !latest.world_epoch)return false;
    auto current=latest;if(!Inspect(current,ctx,base) || current!=latest){latest={};return false;}
    output=current;return true;
}
bool em2::net::DiscoverNativeLocalMovementBinding(PPCContext& ctx,uint8_t* base,Binding& output){
    Environment preserve;output={};
    ++diagnostics.discoveries;
    diagnostics.discovery_event=diagnostics.discovery_count=diagnostics.discovery_index=0;
    diagnostics.discovery_uid=diagnostics.discovery_candidates=diagnostics.discovery_inspect_stage=0;
    diagnostics.lr=uint32_t(ctx.lr);diagnostics.listener=diagnostics.event=diagnostics.action=0;
    auto stage=[&](uint32_t value){diagnostics.stage=diagnostics.discovery_stage=value;};
    stage(201);if(!owner.epoch)return false;
    stage(202);if(owner.thread!=GetCurrentThreadId())return false;
    stage(203);if(query)return false;
    stage(204);if(GameplayInputGate().active())return false;
    stage(205);if(!Readable(0x833DA7E4,4,base) || PPC_LOAD_U32(0x833DA7E4)!=owner.registry)return false;
    stage(206);if(!Readable(0x833DAB60,4,base) || !Readable(0x833DBAD8,4,base) || !Readable(0x833DAEB8,4,base))return false;
    const auto players=PPC_LOAD_U32(0x833DAB60),manager=PPC_LOAD_U32(0x833DBAD8);
    stage(207);if(!Readable(players,640,base))return false;
    stage(208);if(!Readable(manager,76,base))return false;
    uint32_t entity=0,active=0;
    {
        stage(209);Scratch scratch(ctx,base);if(!scratch.memory)return false;
        const auto key=scratch.stack+512;PPC_STORE_U32(key,PPC_LOAD_U32(players+20));
        stage(210);if(!Readable(owner.registry,24,base))return false;
        ctx.r3.u64=owner.registry;ctx.r4.u64=key;__imp__sub_82732A18(ctx,base);entity=ctx.r3.u32;
        const auto count=PPC_LOAD_U32(manager+8),events=PPC_LOAD_U32(manager+4);
        stage(211);diagnostics.discovery_count=count;
        if(count>128 || (count && !Readable(events,size_t(count)*4,base)))return false;
        stage(212);for(unsigned i=0;i<count;++i){diagnostics.discovery_index=i;
            if(!Readable(PPC_LOAD_U32(events+i*4),4,base))return false;}
        stage(213);
        ctx.r3.u64=manager;ctx.r4.u64=manager+16;__imp__sub_827C3D78(ctx,base);active=ctx.r3.u32;
    }
    diagnostics.discovery_event=active;
    stage(213);if(!active)return false;
    stage(214);if(!Readable(entity,96,base) || !Readable(0x833E2188,1,base))return false;
    const auto types=PPC_LOAD_U32(entity+88),list=PPC_LOAD_U32(entity+36);
    const auto kind=PPC_LOAD_U8(0x833E2188);
    stage(215);if(!Readable(types,unsigned(kind)+1,base))return false;
    const auto slot=PPC_LOAD_U8(types+kind);
    stage(216);if(slot==255 || !Readable(list,unsigned(slot)*4+4,base))return false;
    const auto face=PPC_LOAD_U32(list+unsigned(slot)*4);stage(217);if(face<24)return false;
    diagnostics.listener=face+56;
    unsigned candidates=0;Binding selected;
    for(const auto event:{manager+24,active}){
        stage(218);diagnostics.event=diagnostics.discovery_event=event;
        if(!Readable(event,24,base))return false;
        const auto actions=PPC_LOAD_U32(event+16),count=PPC_LOAD_U32(event+20);
        stage(219);diagnostics.discovery_count=count;
        if(count>256 || (count && !Readable(actions,size_t(count)*4,base)))return false;
        for(unsigned i=0;i<count;++i){
            stage(220);diagnostics.discovery_index=i;
            const auto action=PPC_LOAD_U32(actions+i*4);
            diagnostics.action=action;
            if(!Readable(action,4,base))return false;
            diagnostics.discovery_uid=PPC_LOAD_U32(action);
            if(diagnostics.discovery_uid!=PPC_LOAD_U32(0x833DAEB8))continue;
            Binding b;b.world_epoch=owner.epoch;b.producer_generation=owner.generation;b.registry=owner.registry;
            b.manager=manager;b.listener=face+56;b.event=event;b.action=action;
            // Every candidate must pass precisely the same live lease checks.
            // Multiple valid default/current routes are deliberately ambiguous.
            stage(221);const bool valid=Inspect(b,ctx,base);diagnostics.discovery_inspect_stage=diagnostics.stage;
            if(valid){selected=b;diagnostics.discovery_candidates=++candidates;
                if(candidates>1){stage(222);latest={};return false;}}
        }
    }
    stage(224);if(candidates!=1){latest={};return false;}
    latest=selected;output=selected;stage(225);return true;
}
bool em2::net::ProduceNativeLocalMovement(const Binding& binding,const Pad& pad,PPCContext& ctx,uint8_t* base,NativeActorAction& output,unsigned sampled_user){
    Environment preserve;output={};if(query || sampled_user!=owner.user || sampled_user!=binding.user || GameplayInputGate().active())return false;
    auto current=binding;if(!Inspect(current,ctx,base) || current!=binding || !Readable(0x833B65FC,4,base))return false;
    const auto deadzone=Float(0x833B65FC,base);if(!std::isfinite(deadzone) || deadzone<0 || deadzone>=1)return false;
    return Math(binding,{Axis(pad.left_x,false,deadzone),Axis(pad.left_y,true,deadzone)},ctx,base,output);
}
bool em2::net::ProduceNativeLocalRoomMovement(const Binding& binding,const Pad& pad,uint64_t actor_epoch,
    PPCContext& ctx,uint8_t* base,RoomMovement& output,unsigned sampled_user){
    Environment preserve;output={};
    if(!actor_epoch || !binding.reference)return false;
    NativeActorAction action;
    if(!ProduceNativeLocalMovement(binding,pad,ctx,base,action,sampled_user))return false;
    RoomMovement result;result.x=action.x;result.y=action.y;result.pad=pad;
    result.actor_epoch=actor_epoch;result.source=RoomMovementSource::NativeActionV1;
    {
        Scratch scratch(ctx,base);if(!scratch.memory)return false;
        const auto copy=scratch.stack+1024;
        std::memcpy(base+copy,base+binding.controller+16,320);
        // Original83208 obtains the SAME reference virtual172 WorldTransform
        // as ordinary835A8 and uses its third row, flattens Y then normalizes.
        // Only this private BasicController copy receives its latch/cache writes.
        // Poison the transform so a partial/failed native property cannot be
        // mistaken for a useful camera. The helper retains query success in r3.
        for(unsigned i=0;i<12;++i)PPC_STORE_U32(scratch.stack-80+i*4,0x7FC00000);
        ctx.r3.u64=copy;__imp__sub_82783208(ctx,base);
        if(ctx.r3.u32!=1)return false;
        for(unsigned i=0;i<3;++i)if(!std::isfinite(Float(scratch.stack-48+i*4,base)))return false;
        result.forward_x=Float(copy+256,base);result.forward_z=Float(copy+264,base);
        if(Float(copy+260,base)!=0 || !ValidRoomMovement(result))return false;
    }
    auto current=binding;if(!Inspect(current,ctx,base) || current!=binding)return false;
    output=result;return true;
}
namespace {
void ThrowMovementMath(PPCContext& ctx,uint8_t* base){__imp__sub_827C5990(ctx,base);throw std::runtime_error("movement math unwind fixture");}
void FixtureNativeUserDevice(PPCContext& ctx,uint8_t* base){ctx.r3.u64=PPC_LOAD_U32(ctx.r3.u32+4);}
void FixtureMovementReference(PPCContext& ctx,uint8_t* base){
    if(ctx.lr!=0x827CA1B4 || ctx.r4.u32!=0x833DA808 || ctx.r6.u32!=0)
        throw std::runtime_error("Movement reference must use original NiEntity WorldTransform dispatch");
    const auto reference=PPC_LOAD_U32(ctx.r3.u32+4);
    if(PPC_LOAD_U32(reference+240)==2)throw std::runtime_error("Movement reference query unwind fixture");
    if(PPC_LOAD_U32(reference+240)){ctx.r3.u64=0;return;}
    std::memcpy(base+ctx.r5.u32,base+reference+256,48);ctx.r3.u64=1;
}
void FixtureMovementReferenceUnsupported(PPCContext& ctx,uint8_t* base){
    if(ctx.lr!=0x827CA1B4 || ctx.r4.u32!=0x833DA808 || ctx.r6.u32!=0 ||
       PPC_LOAD_U32(ctx.r3.u32)!=0x820D8970)
        throw std::runtime_error("unexpected native camera-component property fallback");
    // ORIGINAL NiEntity scans the next component when Frame declines the
    // property. This deliberately incomplete fixture Camera also declines;
    // it must not enter a real native getter with an unconstructed object.
    ctx.r3.u64=0;
}
}
bool em2::net::TestNativeLocalMovement(){
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    struct Restore {uint8_t* base;Owner saved_owner=owner;Binding saved_latest=latest;decltype(diagnostics) saved_diag=diagnostics;Query* saved_query=query;Environment environment;
        ~Restore(){owner=saved_owner;latest=saved_latest;diagnostics=saved_diag;query=saved_query;if(base)VirtualFree(base,0,MEM_RELEASE);}}restore{base};
    const auto path=std::filesystem::path(__FILE__).parent_path().parent_path()/"private/image.bin";
    std::error_code error;const auto size=std::filesystem::file_size(path,error);
    if(error || !base || !VirtualAlloc(base,0x50000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+PPC_IMAGE_BASE,size,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+PPC_IMAGE_BASE+PPC_IMAGE_SIZE,4096,MEM_COMMIT,PAGE_READWRITE))return false;
    std::ifstream image(path,std::ios::binary);if(!image.read(reinterpret_cast<char*>(base+PPC_IMAGE_BASE),size))return false;
    PPC_LOOKUP_FUNC(base,uint32_t(PPC_CODE_BASE))=__imp__sub_830A7C10;
    PPC_LOOKUP_FUNC(base,uint32_t(PPC_CODE_BASE)+4)=__imp__sub_830A7C70;
    PPC_LOOKUP_FUNC(base,uint32_t(PPC_CODE_BASE)+8)=__imp__sub_827B58D0;
    PPC_LOOKUP_FUNC(base,uint32_t(PPC_CODE_BASE)+12)=__imp__sub_82A69948;
    PPC_LOOKUP_FUNC(base,uint32_t(PPC_CODE_BASE)+16)=FixtureNativeUserDevice;
    // Real NiEntity dispatcher invokes our isolated property fixture through
    // its real Frame-component virtual slot; no production vtable is patched.
    const auto lookup=reinterpret_cast<uintptr_t>(&PPC_LOOKUP_FUNC(base,0x827CA160));
    if(!VirtualAlloc(reinterpret_cast<void*>(lookup&~uintptr_t(4095)),4096,MEM_COMMIT,PAGE_READWRITE))return false;
    PPC_LOOKUP_FUNC(base,0x827CA160)=__imp__sub_827CA160;
    PPC_LOOKUP_FUNC(base,uint32_t(PPC_CODE_BASE)+20)=FixtureMovementReference;
    PPC_LOOKUP_FUNC(base,uint32_t(PPC_CODE_BASE)+24)=FixtureMovementReferenceUnsupported;
    constexpr uint32_t registry=0x2000,players=0x3000,manager=0x4000,action=0x5000,event=0x6000;
    constexpr uint32_t input=0x7000,device=0x8000,history=0x9000,sdk=0xA000;
    constexpr uint32_t entity=0x10000,controller=0x11000,types=0x12000,components=0x13000;
    constexpr uint32_t buckets=0x14000,node=0x15000,handle=0x1234;
    auto put=[&](uint32_t p,float f){PPC_STORE_U32(p,std::bit_cast<uint32_t>(f));};
    auto context=[] {PPCContext c{};c.fpscr.InitHost();c.r1.u64=0x4F000;c.r13.u64=0x13572468;c.lr=0x24681357;return c;};
    auto setup=[&](unsigned user,unsigned curve,unsigned inversion){
        std::memset(base,0,0x50000);owner={};latest={};query=nullptr;
        PPC_STORE_U32(0x833DA7E4,registry);PPC_STORE_U32(0x833DAB60,players);PPC_STORE_U32(0x833DBAD8,manager);
        PPC_STORE_U32(0x833DAEB8,0x8207F5A4);PPC_STORE_U8(0x833E2188,3);
        PPC_STORE_U32(registry+4,uint32_t(PPC_CODE_BASE)+8);PPC_STORE_U32(registry+8,uint32_t(PPC_CODE_BASE)+12);
        PPC_STORE_U32(registry+16,buckets);PPC_STORE_U32(registry+20,4);PPC_STORE_U32(players+20,handle);
        PPC_STORE_U32(0x16000,handle);auto hash=context();hash.r3.u64=0x16000;__imp__sub_827B58D0(hash,base);
        PPC_STORE_U32(buckets+(hash.r3.u32%4)*4,node);PPC_STORE_U32(node,handle);PPC_STORE_U32(node+4,entity);
        PPC_STORE_U32(entity+88,types);PPC_STORE_U32(entity+36,components);std::memset(base+types,255,256);PPC_STORE_U8(types+3,0);
        PPC_STORE_U32(components,controller+24);PPC_STORE_U32(controller+24,0x8211DCE0);PPC_STORE_U32(controller+80,0x8211DCD0);
        PPC_STORE_U32(controller+64,entity-8);
        PPC_STORE_U32(manager,0x12348765);PPC_STORE_U32(manager+72,input);PPC_STORE_U32(input,0x12345678);
        PPC_STORE_U32(manager+4,0x1D000);PPC_STORE_U32(manager+8,1);PPC_STORE_U32(0x1D000,event);
        PPC_STORE_U32(manager+16,0x820A4248);PPC_STORE_U32(manager+24,0x8208C4BC);PPC_STORE_U32(event,0x820A4248);
        PPC_STORE_U32(0x833DC87C,0x19000);PPC_STORE_U32(0x19000+4+user*4,user);PPC_STORE_U32(0x19000+12,0x1A000);
        PPC_STORE_U32(0x1A000+user*4,0x1B000);PPC_STORE_U32(0x1B000,0x1C000);PPC_STORE_U32(0x1B000+4,user);
        PPC_STORE_U32(0x1C000+8,uint32_t(PPC_CODE_BASE)+16);
        PPC_STORE_U32(input+72+user*4,device);PPC_STORE_U32(device,0xD000);PPC_STORE_U32(device+56,4);PPC_STORE_U32(device+60,history);
        PPC_STORE_U32(0xD000+48,uint32_t(PPC_CODE_BASE));PPC_STORE_U32(0xD000+56,uint32_t(PPC_CODE_BASE)+4);
        for(unsigned axis=0;axis<4;++axis){const auto p=history+axis*32;PPC_STORE_U32(p+16,uint32_t(-32768));PPC_STORE_U32(p+20,32767);PPC_STORE_U32(p+24,uint32_t(-100));PPC_STORE_U32(p+28,100);}
        PPC_STORE_U32(event+4,0x17000);PPC_STORE_U32(event+8,1);PPC_STORE_U32(0x17000,controller+80);
        PPC_STORE_U32(event+16,0x18000);PPC_STORE_U32(event+20,1);PPC_STORE_U32(0x18000,action);
        PPC_STORE_U32(action,0x8207F5A4);PPC_STORE_U32(action+80,4);PPC_STORE_U32(action+408,2);
        // Exact passive retail Movement observation: 20261111-081853,
        // A2EB1290. LEFT Y first to dest0, LEFT X second to dest3. This
        // deliberately differs from the camera/look action's axis order.
        for(unsigned axis=0;axis<2;++axis){const auto p=action+88+axis*32;PPC_STORE_U32(p,user);PPC_STORE_U32(p+4,axis?0x02400017:0x02400018);
            PPC_STORE_U32(p+8,axis?3:0);PPC_STORE_U8(p+16,uint8_t((inversion>>axis)&1));}
        put(action+416,.2f);put(action+420,.95f);put(action+424,1.6f);put(action+428,1.f);PPC_STORE_U32(action+432,curve);
    };
    bool ok=true;unsigned checks=0,cases=0;
    auto check=[&](bool condition,const char* why){++checks;if(!condition)REXLOG_ERROR("Native local Movement: {}",why);ok&=condition;return condition;};
    // Independent original producer/getter oracle for LEFT stick axes. Includes
    // all int16 values, asymmetric+32768 Y endpoint and six device deadzones.
    setup(0,0,0);
    for(float dz:{0.f,.1f,.2f,8689.f/32768.f,.3f,.5f}){
        put(0x833B65FC,dz);
        for(int raw=-32768;raw<=32767;++raw){
            PPC_STORE_U16(sdk+8,uint16_t(raw));PPC_STORE_U16(sdk+10,uint16_t(raw));
            auto c=context();c.r3.u64=device;c.r4.u64=sdk;__imp__sub_830ACF88(c,base);
            for(unsigned axis=0;axis<2;++axis){
                c=context();c.r3.u64=device;c.r4.u64=axis;__imp__sub_830A7C10(c,base);
                c.r5.u64=c.r3.u64;c.r3.u64=device;c.r4.u64=axis;__imp__sub_830A7C70(c,base);
                const auto expected=float(double(float(c.r3.s32))*double(.01f));
                if(std::bit_cast<uint32_t>(expected)!=std::bit_cast<uint32_t>(Axis(int16_t(raw),axis!=0,dz)))return check(false,"LEFT device stage differs from original producer/raw getter");
            }
        }
    }
    const std::array<Pad,12> samples={Pad{},Pad{0,0,0,32767,0},Pad{0,0,0,-32768,0},Pad{0,0,0,0,32767},Pad{0,0,0,0,-32768},
        Pad{0,0,0,32767,-32768},Pad{0,0,0,-32768,32767},Pad{0,0,0,8688,-8688},Pad{0,0,0,8689,-8689},Pad{0,0,0,8690,-8690},Pad{0,0,0,20000,-15000},Pad{0,0,0,1,-1}};
    for(unsigned user:{0u,1u})for(unsigned curve:{0u,1u,2u})for(unsigned inverse:{0u,1u,2u,3u})for(unsigned tuning=0;tuning<3;++tuning){
        setup(user,curve,inverse);
        if(tuning==1){put(action+416,0.f);put(action+424,1.f);put(action+428,.5f);}
        if(tuning==2){put(action+416,.35f);put(action+420,.8f);put(action+424,2.3f);put(action+428,.75f);}
        auto capture=context();capture.lr=0x827C439C;capture.r3.u64=controller+80;capture.r4.u64=event;capture.r5.u64=action;
        Binding binding;
        if(!check(!CaptureNativeLocalMovementBinding(capture,base,binding),"default producer is unarmed") ||
           !check(ArmNativeLocalMovement(7,registry,base,user) && CaptureNativeLocalMovementBinding(capture,base,binding),"actual-dispatch-shaped selected retail listener binds"))return false;
        const auto capture_before=capture;ObserveNativeLocalMovementBinding(capture,base);Binding retained;
        if(!check(NativeLocalMovementBindingFor(capture,base,retained) && retained==binding && !std::memcmp(&capture,&capture_before,sizeof(capture)),"observer retains and accessor revalidates lease without altering caller"))return false;
        put(0x833B65FC,8689.f/32768.f);
        for(const auto& pad:samples){
            PPC_STORE_U16(sdk+8,uint16_t(pad.left_x));PPC_STORE_U16(sdk+10,uint16_t(pad.left_y));
            auto c=context();c.r3.u64=device;c.r4.u64=sdk;__imp__sub_830ACF88(c,base);
            // Do not use the injected axis values as the oracle. Original math
            // reaches original56F8→3030→native virtual raw device getters.
            NativeActorAction expected,actual;c=context();const auto saved=c;const auto csr=_mm_getcsr();const auto last=GetLastError();
            std::array<uint8_t,0x50000> before;std::memcpy(before.data(),base,before.size());
            const bool expected_ok=Math(binding,{},c,base,expected,false);
            const auto expected_diag=diagnostics;
            const bool actual_ok=ProduceNativeLocalMovement(binding,pad,c,base,actual,user);
            if(!expected_ok || !actual_ok)REXLOG_ERROR("Movement math case user={} curve={} inverse={} tuning={} pad={}/{} expectedOK={} nativeReturn={} nativeXY={}/{} injectedOK={} nativeReturn={} hits={} injectedXY={}/{} stage={}",
                user,curve,inverse,tuning,pad.left_x,pad.left_y,expected_ok,expected_diag.math_return,expected_diag.math_x,expected_diag.math_y,
                actual_ok,diagnostics.math_return,diagnostics.axis_queries,diagnostics.math_x,diagnostics.math_y,diagnostics.stage);
            if(!check(expected_ok && actual_ok,"original action math and isolated producer both succeed") ||
               !check(std::bit_cast<uint32_t>(actual.x)==std::bit_cast<uint32_t>(expected.x) && std::bit_cast<uint32_t>(actual.y)==std::bit_cast<uint32_t>(expected.y),"native action outputs match bit-for-bit") ||
               !check(!std::memcmp(&c,&saved,sizeof(c)) && !std::memcmp(before.data(),base,before.size()) && _mm_getcsr()==csr && GetLastError()==last && !query,"context scratch device action FP and LastError preserved"))return false;
            if(!pad.left_x && !pad.left_y && !check(actual.x==0 && actual.y==0,"explicit neutral remains neutral"))return false;
            ++cases;
        }
        NativeActorAction ignored;auto c=context();
        if(!check(!ProduceNativeLocalMovement(binding,{},c,base,ignored,1-user),"another sampled user cannot feed selected retail device"))return false;
        const auto previous_power=Float(action+424,base);put(action+424,previous_power+.25f);
        if(!check(!ProduceNativeLocalMovement(binding,{},c,base,ignored,user),"changed action curve invalidates lease"))return false;
        put(action+424,previous_power);PPC_STORE_U32(0x18000,action+4);
        if(!check(!ProduceNativeLocalMovement(binding,{},c,base,ignored,user),"replaced selected event action invalidates lease"))return false;
        PPC_STORE_U32(0x18000,action);PPC_STORE_U32(0x833DA7E4,registry+4);
        if(!check(!ProduceNativeLocalMovement(binding,{},c,base,ignored,user),"changed world rejects stale binding"))return false;
        PPC_STORE_U32(0x833DA7E4,registry);
        PPC_STORE_U32(manager+16,0x8208C4BC);
        if(!check(!ProduceNativeLocalMovement(binding,{},c,base,ignored,user) && !NativeLocalMovementBindingFor(c,base,retained),"context change invalidates production and retained accessor"))return false;
        PPC_STORE_U32(manager+16,0x820A4248);
        if(!check(!NativeLocalMovementBindingFor(c,base,retained),"retired accessor cannot silently resurrect old context lease"))return false;
        ArmNativeLocalMovement(8,registry,base,user);
        if(!check(!ProduceNativeLocalMovement(binding,{},c,base,ignored,user),"rearm invalidates old world lease"))return false;
        DisarmNativeLocalMovement();
    }
    setup(0,0,0);ArmNativeLocalMovement(9,registry,base);auto capture=context();capture.lr=0x827C439C;capture.r3.u64=controller+80;capture.r4.u64=event;capture.r5.u64=action;Binding b;
    if(!CaptureNativeLocalMovementBinding(capture,base,b))return false;
    {auto c=context();const auto saved=c;const auto csr=_mm_getcsr();const auto last=GetLastError();std::array<uint8_t,8192> bytes;std::memcpy(bytes.data(),base+c.r1.u32-8192,8192);NativeActorAction output;
        try{Math(b,{.5f,.25f},c,base,output,true,ThrowMovementMath);return false;}catch(const std::runtime_error&){}
        if(!check(!query && !std::memcmp(&c,&saved,sizeof(c)) && !std::memcmp(bytes.data(),base+c.r1.u32-8192,8192) && _mm_getcsr()==csr && GetLastError()==last,"native exception restores scratch/context/FP/LastError and releases injection scope"))return false;
    }
    {Query q{b,{.5f,.25f}};QueryScope scope(q);NativeActorAction result;auto c=context();
        if(!check(!Math(b,{0,0},c,base,result) && query==&q,"nested evaluation cannot replace current query"))return false;
        for(unsigned guard=0;guard<6;++guard){auto call=context();call.lr=0x827C5810;call.r3.u64=b.manager;call.r4.u64=b.user;call.r5.u64=b.device;call.r6.u64=0x02400017;
            if(guard==0)call.lr=0;if(guard==1)call.r3.u64=b.manager+4;if(guard==2)call.r4.u64=1-b.user;
            if(guard==3)call.r5.u64=b.device+4;if(guard==4)call.r6.u64=0x02400019;if(guard==5)call.r6.u64=0x01400003;
            const auto saved=call;if(!check(!Override(call) && !std::memcmp(&call,&saved,sizeof(call)),"unrelated native axis call remains untouched"))return false;
        }
    }
    ArmNativeLocalMovement(10,registry,base,1);
    if(!check(!CaptureNativeLocalMovementBinding(capture,base,b),"binding user must match explicitly selected native user"))return false;
    // Authored player0 may route to physical device3 through the REAL native
    // profile resolver; direct indexing by action.user would use the wrong pad.
    ArmNativeLocalMovement(11,registry,base,3);PPC_STORE_U32(0x1B000+4,3);PPC_STORE_U32(input+84,device);
    if(!check(CaptureNativeLocalMovementBinding(capture,base,b) && b.logical_user==0 && b.user==3,
        "original profile/device route distinguishes logical action user from physical pad"))return false;
    {auto c=context();NativeActorAction out;
        if(!check(ProduceNativeLocalMovement(b,{},c,base,out,3) && !ProduceNativeLocalMovement(b,{},c,base,out,0),"only resolved physical source may supply Pad values"))return false;
        ObserveNativeLocalMovementBinding(capture,base);PPC_STORE_U32(0x1B000+4,2);Binding retained;
        if(!check(!ProduceNativeLocalMovement(b,{},c,base,out,3) && !NativeLocalMovementBindingFor(c,base,retained),"physical device remap invalidates cached binding and producer"))return false;
        PPC_STORE_U32(0x1B000+4,3);
        if(!check(!NativeLocalMovementBindingFor(c,base,retained),"old retained binding does not resurrect after device remap"))return false;
    }
    // Native default event remains active only alongside a resolvable named
    // event. Preserve that real75B0 guard instead of accepting it unconditionally.
    PPC_STORE_U32(manager+28,0x17000);PPC_STORE_U32(manager+32,1);
    PPC_STORE_U32(manager+40,0x18000);PPC_STORE_U32(manager+44,1);
    capture.r4.u64=manager+24;
    if(!check(CaptureNativeLocalMovementBinding(capture,base,b),"inline default event is active with current named context"))return false;
    ObserveNativeLocalMovementBinding(capture,base);PPC_STORE_U32(manager+16,0x76543210);
    {auto c=context();NativeActorAction out;Binding retained;
        if(!check(!ProduceNativeLocalMovement(b,{},c,base,out,3) && !NativeLocalMovementBindingFor(c,base,retained),"unresolved named context also invalidates default event lease"))return false;
    }
    // Actual passive retail event: 141 actions, including Movement. It is a
    // distinct list from the much smaller set of actor listeners.
    PPC_STORE_U32(manager+16,0x820A4248);capture.r4.u64=event;
    for(unsigned i=0;i<257;++i)PPC_STORE_U32(0x18000+i*4,action+4);
    PPC_STORE_U32(0x18000+140*4,action);PPC_STORE_U32(event+20,141);
    if(!check(CaptureNativeLocalMovementBinding(capture,base,b),"141-action retail event admits Movement at final index"))return false;
    {auto c=context();NativeActorAction out;
        if(!check(ProduceNativeLocalMovement(b,{},c,base,out,3),"large retail action list remains valid for original math production"))return false;
    }
    PPC_STORE_U32(0x18000,action);
    if(!check(!CaptureNativeLocalMovementBinding(capture,base,b),"duplicate Movement membership rejects large event"))return false;
    PPC_STORE_U32(0x18000,action+4);PPC_STORE_U32(event+20,257);
    if(!check(!CaptureNativeLocalMovementBinding(capture,base,b),"action list exceeding256 rejects before membership scan"))return false;
    PPC_STORE_U32(event+20,141);PPC_STORE_U32(event+8,129);
    if(!check(!CaptureNativeLocalMovementBinding(capture,base,b),"listener list retains its separate128-entry bound"))return false;
    // Native reference property path and ORIGINAL projected normalization.
    // The real NiEntity dispatcher below traverses a typed Frame component.
    setup(0,2,0);put(0x833B65FC,.2f);
    constexpr uint32_t reference=0x20000,frame=0x21000,camera=0x22000,reference_list=0x23000;
    PPC_STORE_U32(controller+16,0x8211DE70);PPC_STORE_U8(0x833DAE58,6);PPC_STORE_U8(types+6,0);
    PPC_STORE_U32(controller+96,reference);PPC_STORE_U32(reference,0x8208D908);
    for(unsigned i=0;i<4;++i)PPC_STORE_U32(reference+4+i*4,0x12345678+i);
    PPC_STORE_U32(reference+36,reference_list);PPC_STORE_U32(reference+44,2);
    PPC_STORE_U32(reference_list,frame);PPC_STORE_U32(reference_list+4,camera);
    PPC_STORE_U32(frame,0x82090340);PPC_STORE_U32(frame+4,reference);PPC_STORE_U32(camera,0x820D8970);
    PPC_STORE_U32(0x82090340+172,uint32_t(PPC_CODE_BASE)+20);
    PPC_STORE_U32(0x820D8970+172,uint32_t(PPC_CODE_BASE)+24);
    for(unsigned i=0;i<12;++i)put(reference+256+i*4,i%5==0?1.f:0.f);
    auto c=context();
    if(!check(ArmNativeLocalMovement(0xABCDEF,registry,base),"Room producer explicitly arms world lease"))return false;
    const auto discovery_before=c;std::vector<uint8_t> discovery_memory(base,base+0x50000);
    if(!check(DiscoverNativeLocalMovementBinding(c,base,b) && b.reference==reference && diagnostics.captured==0 &&
        std::memcmp(&c,&discovery_before,sizeof(c))==0 && std::memcmp(base,discovery_memory.data(),discovery_memory.size())==0,
        "pre-tick discovery resolves selected retail binding without a prior dispatch or guest/context writes"))return false;
    capture=c;capture.r3.u64=controller+80;capture.r4.u64=event;capture.r5.u64=action;
    Binding invalid;
    if(!check(!CaptureNativeLocalMovementBinding(capture,base,invalid),"discovery does not weaken listener capture LR provenance"))return false;
    for(const auto pitched:std::array<std::array<float,3>,5>{{{3,-12,4},{-3,12,-4},{1,27,0},{0,-19,-2},{0,0,1}}}){
        for(unsigned i=0;i<3;++i)put(reference+256+32+i*4,pitched[i]);
        RoomMovement room;const Pad pad{0x1234,42,63,20000,-15000,500,-700};
        NativeActorAction action_result;
        if(!check(ProduceNativeLocalMovement(b,pad,c,base,action_result),"Room test uses original native Movement action oracle"))return false;
        std::vector<uint8_t> before(base,base+0x50000);const auto saved=c;
        _mm_setcsr(0x1F80);SetLastError(0x1234ABCD);
        const bool produced=ProduceNativeLocalRoomMovement(b,pad,0xFEDCBA9876543210ull,c,base,room);
        const auto err=GetLastError();const auto csr=_mm_getcsr();
        const auto length=std::sqrt(double(pitched[0])*pitched[0]+double(pitched[2])*pitched[2]);
        if(!check(produced && ValidRoomMovement(room) && std::abs(room.forward_x-pitched[0]/length)<1e-6 &&
            std::abs(room.forward_z-pitched[2]/length)<1e-6 && std::bit_cast<uint32_t>(room.x)==std::bit_cast<uint32_t>(action_result.x) &&
            std::bit_cast<uint32_t>(room.y)==std::bit_cast<uint32_t>(action_result.y) && room.pad==pad && room.actor_epoch==0xFEDCBA9876543210ull &&
            room.source==RoomMovementSource::NativeActionV1,"pitched/yawed native reference yields exact native action and projected positive forward, explicit authoritative epoch and full Pad"))return false;
        if(!check(err==0x1234ABCD && csr==0x1F80 && std::memcmp(&c,&saved,sizeof(c))==0 &&
            std::memcmp(base,before.data(),before.size())==0,"Room production preserves complete caller, guest scratch, controller caches, reference, FP and LastError"))return false;
    }
    RoomMovement room;
    if(!check(!ProduceNativeLocalRoomMovement(b,{},0,c,base,room) && room.actor_epoch==0,"unassigned authoritative incarnation cannot become a command"))return false;
    auto reject_room=[&](const char* why){return check(!ProduceNativeLocalRoomMovement(b,{},77,c,base,room) && room.actor_epoch==0,why);};
    for(const auto offset:{24u+44,24u+233,24u+284,24u+289}){
        if(offset==24+284)PPC_STORE_U32(controller+offset,1);else PPC_STORE_U8(controller+offset,offset==24+44?0x80:1);
        if(!reject_room("transition/captured/alternate/scripted reference route rejects ordinary Room producer"))return false;
        if(offset==24+284)PPC_STORE_U32(controller+offset,0);else PPC_STORE_U8(controller+offset,0);
    }
    for(const auto bad:std::array<std::array<float,3>,5>{{{0,0,0},{0,1,0},{1e-10f,1,0},{INFINITY,0,1},{1, NAN,1}}}){
        for(unsigned i=0;i<3;++i)put(reference+256+32+i*4,bad[i]);
        if(!reject_room("zero, vertical, epsilon-degenerate or nonfinite native transforms reject"))return false;
    }
    put(reference+256+32,3);put(reference+256+36,-12);put(reference+256+40,4);
    PPC_STORE_U32(reference+240,1);
    if(!reject_room("failed native property query cannot manufacture a valid view direction"))return false;
    PPC_STORE_U32(reference+240,2);
    {const auto saved=c;std::vector<uint8_t> before(base,base+0x50000);bool threw=false;
        try{ProduceNativeLocalRoomMovement(b,{},77,c,base,room);}catch(const std::runtime_error&){threw=true;}
        if(!check(threw && std::memcmp(&c,&saved,sizeof(c))==0 && std::memcmp(base,before.data(),before.size())==0,
            "native property exception preserves complete caller and scratch with no real controller mutation"))return false;
    }
    PPC_STORE_U32(reference+240,0);PPC_STORE_U32(reference+4,0x76543210);
    if(!reject_room("reused camera address with different native GUID invalidates retained producer"))return false;
    Binding retained;
    if(!check(!NativeLocalMovementBindingFor(c,base,retained),"reused reference invalidates retained accessor"))return false;
    PPC_STORE_U32(reference+4,0x12345678);
    if(!check(!NativeLocalMovementBindingFor(c,base,retained),"restoring bytes cannot resurrect retired reference lease"))return false;
    if(!check(DiscoverNativeLocalMovementBinding(c,base,b),"new explicit discovery can establish a new validated reference lease"))return false;
    PPC_STORE_U32(reference_list+4,frame);
    if(!reject_room("replaced camera component rejects stale native reference identity"))return false;
    PPC_STORE_U32(reference_list+4,camera);
    PPC_STORE_U32(manager+28,0x17000);PPC_STORE_U32(manager+32,1);PPC_STORE_U32(manager+40,0x18000);PPC_STORE_U32(manager+44,1);
    if(!check(!DiscoverNativeLocalMovementBinding(c,base,invalid),"valid Movement in both default and current context rejects ambiguous discovery"))return false;
    PPC_STORE_U32(manager+44,0);PPC_STORE_U32(manager+16,0x76543210);
    if(!check(!DiscoverNativeLocalMovementBinding(c,base,invalid) && diagnostics.discovery_stage==213,
        "discovery precisely identifies unresolved active context before native input phase"))return false;
    PPC_STORE_U32(manager+16,0x820A4248);PPC_STORE_U32(manager+44,1);PPC_STORE_U32(0x18000,0);
    if(!check(!DiscoverNativeLocalMovementBinding(c,base,invalid) && diagnostics.discovery_stage==220 &&
        diagnostics.discovery_event==manager+24 && diagnostics.discovery_count==1 && diagnostics.discovery_index==0 && diagnostics.action==0,
        "discovery identifies unreadable default-event action before scanning active event"))return false;
    PPC_STORE_U32(0x18000,action);PPC_STORE_U32(manager+44,0);PPC_STORE_U32(0x833DA7E4,registry+4);
    if(!check(!DiscoverNativeLocalMovementBinding(c,base,invalid) && diagnostics.discovery_stage==205,
        "discovery distinguishes registry lifecycle drift from event/binding rejection"))return false;
    if(ok)REXLOG_INFO("Native local Movement passed {} checks/{} action cases: original LEFT producer/getters, original action curves, native NiEntity reference projection, pre-tick discovery and complete context preservation; no gameplay dispatch",checks,cases);
    return ok;
}

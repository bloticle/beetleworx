#include "online_cutscene_skip.h"
#include "online_cutscene_input.h"
#include "online_input_clock.h"
#include "controls.h"
#include "simulation.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <limits>
#include <rex/logging.h>
#include <rex/cvar.h>
REXCVAR_DECLARE(bool,em2_private_profile);
PPC_EXTERN_FUNC(__imp__sub_82770EE8);
PPC_EXTERN_FUNC(__imp__sub_82770F58);
PPC_EXTERN_FUNC(__imp__sub_82B210B0);
PPC_EXTERN_FUNC(__imp__sub_82B1DF70);
PPC_EXTERN_FUNC(__imp__sub_830A8200);
PPC_EXTERN_FUNC(__imp__sub_830ACA80);
PPC_EXTERN_FUNC(__imp__sub_827C3278);
PPC_EXTERN_FUNC(__imp__sub_827C5F30);
PPC_EXTERN_FUNC(__imp__sub_830A7FD0);
PPC_EXTERN_FUNC(__imp__sub_830A8030);
PPC_EXTERN_FUNC(__imp__sub_830A8090);
PPC_EXTERN_FUNC(__imp__sub_830A7968);
namespace em2::net {
namespace {
struct Environment {unsigned fp=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(fp);SetLastError(error);}};
bool Readable(uint8_t* base,uint32_t p,size_t n){return base && p>=4096 && uint64_t(p)+n<=0x100000000ull && GuestReadBatch::Readable(base+p,n);}
using Record=std::array<uint8_t,436>;
void Normalize(Record& a){
    std::fill(a.begin()+4,a.begin()+80,uint8_t(0));std::fill(a.begin()+84,a.begin()+88,uint8_t(0));
    a[105]=0;std::fill(a.begin()+112,a.begin()+120,uint8_t(0));std::fill(a.begin()+120,a.begin()+408,uint8_t(0));
}
constexpr std::array<const char*,3> Names{"DisplaySkipMovieButton","MovieSkipStart","MovieSkipStop"};
constexpr std::array<uint32_t,3> Kinds{2,1,3};
void FixtureControlMode(PPCContext& ctx,uint8_t*){ctx.r3.u64=3;}
void FixtureSceneMode(PPCContext& ctx,uint8_t*){ctx.r3.u64=6;}
void FixtureMovieMode(PPCContext& ctx,uint8_t*){ctx.r3.u64=2;}
void FixtureSceneObject(PPCContext& ctx,uint8_t*){ctx.r3.u64=0x5000;}
thread_local uint8_t* fixture_base=nullptr;
thread_local uint64_t fixture_tick=0;
thread_local unsigned fixture_callbacks=0;
thread_local bool fixture_movie_timer_lease=true;
bool AgreedTick(uint64_t& tick,uint8_t* base){if(fixture_base && fixture_base==base){tick=fixture_tick;return true;}return em2::CurrentAgreedSimulationTick(tick);}
void FixtureSceneCallback(PPCContext& ctx,uint8_t* base){++fixture_callbacks;__imp__sub_82B210B0(ctx,base);}
void FixtureMovieCallback(PPCContext& ctx,uint8_t* base){++fixture_callbacks;
    fixture_movie_timer_lease=fixture_movie_timer_lease && NativeMovieAcceptTimerOwned(ctx.r3.u32+32,0x82B1E070,ctx,base) &&
        !NativeMovieAcceptTimerOwned(ctx.r3.u32+88,0x82B1E070,ctx,base) && !NativeMovieAcceptTimerOwned(ctx.r3.u32+32,0x82B1DD88,ctx,base);
    __imp__sub_82B1DF70(ctx,base);}
struct Binding {
    uint32_t registry=0,manager=0,input=0,device=0,buttons=0,threshold=0;unsigned user=0;
    bool movie=false;
    std::array<uint32_t,3> actions{};std::array<Record,3> shapes{};
    bool operator==(const Binding&)const=default;
};
bool Discover(uint32_t registry,unsigned user,PPCContext& ctx,uint8_t* base,Binding& out,bool movie=false){
    if(user>3)return false;
    NativeCutsceneUiSnapshot ui;if(!InspectNativeCutsceneUi(ctx,base,registry,ui) || !Readable(base,ui.manager,176) || !Readable(base,0x833B65F8,4))return false;
    Binding b;b.registry=registry;b.manager=ui.manager;b.user=user;b.movie=movie;b.threshold=PPC_LOAD_U32(0x833B65F8);
    b.input=PPC_LOAD_U32(b.manager+72);if(!b.threshold || b.threshold>255 || !Readable(base,b.input,88))return false;
    b.device=PPC_LOAD_U32(b.input+72+user*4);
    if(!Readable(base,b.device,96) || PPC_LOAD_U32(b.device)!=0x821DE6E8 || PPC_LOAD_U32(b.device+48)!=16 ||
       PPC_LOAD_U32(0x821DE6E8+60)!=0x830A7FD0 || PPC_LOAD_U32(0x821DE6E8+64)!=0x830A8030 ||
       PPC_LOAD_U32(0x821DE6E8+68)!=0x830A8090 || PPC_LOAD_U32(0x821DE6E8+112)!=0x830A7968)return false;
    b.buttons=PPC_LOAD_U32(b.device+52);if(!Readable(base,b.buttons,320))return false;
    unsigned contexts=0;
    for(const auto& c:ui.contexts)if(c.name==(movie?"Menu":"CutScene")){
        ++contexts;
        for(const auto& a:c.actions)for(unsigned i=0;i<(movie?1u:Names.size());++i)if(a.name==(movie?"Accept":Names[i])){
            if(b.actions[i] || a.kind!=Kinds[i] || a.condition || a.curve || a.bindings.size()!=1 ||
               a.bindings[0][0]!=7 || a.bindings[0][1]!=0x01400009 || a.bindings[0][2] || a.bindings[0][3] ||
               PPC_LOAD_U8(a.action+104) || a.bindings[0][5])return false;
            b.actions[i]=a.action;std::memcpy(b.shapes[i].data(),base+a.action,436);Normalize(b.shapes[i]);
        }
    }
    if(contexts!=1 || std::any_of(b.actions.begin(),b.actions.begin()+(movie?1:3),[](auto a){return !a;}))return false;
    out=std::move(b);return true;
}
struct Candidate {
    std::array<uint8_t,96> device{};std::array<uint8_t,320> buttons{};std::array<Record,3> actions{};
    bool down=false,changed=false;NativeCutsceneSkipSample result;
};
Candidate Fresh(const Binding& b,uint8_t* base){
    Candidate c;std::memcpy(c.device.data(),base+b.device,96);c.actions=b.shapes;
    for(unsigned at:{20u,40u,44u})std::fill_n(c.device.begin()+at,4,uint8_t(0));return c;
}
struct Evaluation {const Binding& binding;Candidate& candidate;bool validate=true;};
bool Evaluate(PPCContext& ctx,uint8_t* base,NativeInputClockScratch& scratch,void* opaque){
    auto& e=*static_cast<Evaluation*>(opaque);const auto& b=e.binding;auto& c=e.candidate;
    const auto manager=scratch.data(),input=manager+176,device=input+96,buttons=device+96,action=buttons+320,output=action+440;
    if(scratch.capacity()<output+8-manager || !Readable(base,b.manager,176))return false;
    std::memcpy(base+manager,base+b.manager,176);std::memset(base+input,0,96);
    std::memcpy(base+device,c.device.data(),96);std::memcpy(base+buttons,c.buttons.data(),320);
    PPC_STORE_U32(manager+72,input);PPC_STORE_U32(input+72+b.user*4,device);
    PPC_STORE_U32(device+52,buttons);PPC_STORE_U32(device+56,0);PPC_STORE_U32(device+60,0);
    if(!scratch.OwnButtonHistory(buttons,16))return false;
    ctx.r3.u64=device;__imp__sub_830A8200(ctx,base);
    if(c.changed){ctx.r3.u64=device;ctx.r4.u64=c.down?0x1000:0;__imp__sub_830ACA80(ctx,base);}
    for(unsigned i=0;i<(b.movie?1u:3u);++i){
        std::memcpy(base+action,c.actions[i].data(),436);
        if(Kinds[i]==2 && !scratch.OwnActionRecord(action))return false;
        ctx.r3.u64=manager;ctx.r4.u64=action;__imp__sub_827C3278(ctx,base);const auto enabled=ctx.r3.u32;if(enabled>1)return false;
        PPC_STORE_U32(output,0);PPC_STORE_U32(output+4,0);
        ctx.r3.u64=manager;ctx.r4.u64=action;ctx.r5.u64=enabled;ctx.r6.u64=output;ctx.r7.u64=output+4;
        __imp__sub_827C5F30(ctx,base);
        if(ctx.r3.u32>1 || !std::isfinite(std::bit_cast<float>(PPC_LOAD_U32(output))) || !std::isfinite(std::bit_cast<float>(PPC_LOAD_U32(output+4))))return false;
        c.result.dispatch[i]=ctx.r3.u32!=0;std::memcpy(c.actions[i].data(),base+action,436);
    }
    std::memcpy(c.device.data(),base+device,96);std::memcpy(c.buttons.data(),base+buttons,320);
    if(e.validate){Binding current;if(!Discover(b.registry,b.user,ctx,base,current,b.movie) || current!=b)return false;}
    return true;
}
}
struct NativeCutsceneSkipSource::Impl {
    DWORD thread=0;uint8_t* base=nullptr;Binding binding;Candidate committed;NativeInputClockState clock;
    uint64_t epoch=0,next=0,last=0;bool armed=false,cached=false,exhausted=false,down=false;
};
NativeCutsceneSkipSource::NativeCutsceneSkipSource():impl_(std::make_unique<Impl>()){}
NativeCutsceneSkipSource::~NativeCutsceneSkipSource()=default;
bool NativeCutsceneSkipSource::Arm(uint32_t registry,unsigned user,uint64_t epoch,uint64_t first,PPCContext& ctx,uint8_t* base){
    return ArmMode(registry,user,epoch,first,false,ctx,base);
}
bool NativeCutsceneSkipSource::ArmMovieAccept(uint32_t registry,unsigned user,uint64_t epoch,uint64_t first,PPCContext& ctx,uint8_t* base){
    return ArmMode(registry,user,epoch,first,true,ctx,base);
}
bool NativeCutsceneSkipSource::ArmMode(uint32_t registry,unsigned user,uint64_t epoch,uint64_t first,bool movie,PPCContext& ctx,uint8_t* base){
    Environment env;auto& s=*impl_;Binding binding;
    if(s.armed || !epoch || (epoch&1) || !Discover(registry,user,ctx,base,binding,movie) || !InitializeNativeInputClock(s.clock,epoch,first,ctx,base))return false;
    s.committed=Fresh(binding,base);s.binding=std::move(binding);s.epoch=epoch;s.next=first;s.thread=GetCurrentThreadId();s.base=base;s.armed=true;return true;
}
bool NativeCutsceneSkipSource::Produce(const Pad& pad,unsigned user,uint64_t epoch,uint64_t tick,PPCContext& ctx,uint8_t* base,NativeCutsceneSkipSample& out){
    Environment env;out={};auto& s=*impl_;Binding current;
    if(!s.armed || s.thread!=GetCurrentThreadId() || base!=s.base || user!=s.binding.user || epoch!=s.epoch ||
       !Discover(s.binding.registry,user,ctx,base,current,s.binding.movie) || current!=s.binding)return false;
    const bool down=bool(pad.buttons&0x1000);
    if(s.cached && s.last==tick){if(down!=s.down)return false;out=s.committed.result;return true;}
    if(s.exhausted || tick!=s.next)return false;
    auto next=s.committed;next.down=down;next.changed=!s.cached || down!=s.down;next.result={epoch,tick,{}};
    Evaluation evaluation{s.binding,next};if(!RunNativeInputClock(s.clock,epoch,tick,ctx,base,Evaluate,&evaluation))return false;
    s.committed=std::move(next);s.last=tick;s.down=down;s.cached=true;s.exhausted=tick==UINT64_MAX;if(!s.exhausted)s.next=tick+1;
    out=s.committed.result;return true;
}

namespace {
struct BorrowContext {
    PPCContext& ctx;PPCContext saved;Environment env;uint8_t* stack=nullptr;std::array<uint8_t,8192> bytes{};
    BorrowContext(PPCContext& c,uint8_t* base):ctx(c),saved(c){const auto top=c.r1.u32&~15u;
        if(top>=12288 && Readable(base,top-8192,8192)){stack=base+top-8192;std::memcpy(bytes.data(),stack,8192);ctx.r1.u64=top-4096;}}
    ~BorrowContext(){if(stack)std::memcpy(stack,bytes.data(),8192);ctx=saved;}
};
struct Target {
    uint32_t registry=0,profile=0,object=0,table=0,event=0,mode=0;std::array<uint32_t,3> actions{};
    bool operator==(const Target&)const=default;
};
bool ReadTarget(uint32_t registry,PPCContext& ctx,uint8_t* base,Target& out){
    BorrowContext preserve(ctx,base);if(!preserve.stack)return false;
    NativeGameOverState state;uint32_t mode=0;NativeCutsceneUiSnapshot ui;
    if(!ReadNativeControlState(ctx,base,state,mode) || (mode!=6 && mode!=1) || state.registry!=registry || (mode==1 && !ReadNativeCutsceneContext(ctx,base,registry)) || !InspectNativeCutsceneUi(ctx,base,registry,ui))return false;
    Target target;target.registry=registry;target.mode=mode;target.profile=PPC_LOAD_U32(0x833DAD98);
    if(!Readable(base,target.profile,16))return false;
    const auto profile_table=PPC_LOAD_U32(target.profile+12);if(!Readable(base,profile_table,32))return false;
    const auto getter=PPC_LOAD_U32(profile_table+28);
    if((getter&3) || getter<PPC_CODE_BASE || uint64_t(getter)>=uint64_t(PPC_CODE_BASE)+PPC_CODE_SIZE || !PPC_LOOKUP_FUNC(base,getter))return false;
    ctx.r3.u64=target.profile;ctx.r4.u64=mode;__imp__sub_82770EE8(ctx,base);target.object=ctx.r3.u32;
    if(!Readable(base,target.object,mode==1?32:96))return false;target.table=PPC_LOAD_U32(target.object);
    // Original mode-6 ending-movie or mode-1 direct-movie handler. Reject
    // restriction, Pause, GameOver and retail character listeners; preserve
    // each native handler's own allow-skipping/authoring guard.
    if(!Readable(base,target.table,48))return false;
    if(REXCVAR_GET(em2_private_profile)){
        static thread_local uint32_t observed=0;
        if(observed!=target.object){observed=target.object;REXLOG_INFO("Private native cutscene UI target observed: object {:08X} table {:08X} handler {:08X}",target.object,target.table,PPC_LOAD_U32(target.table+44));}
    }
    if(PPC_LOAD_U32(target.table+44)!=(mode==1?0x82B1EDF8u:0x82B210B0u) ||
       (mode==1 && target.table!=0x8215B7B0))return false;
    unsigned contexts=0;
    for(const auto& c:ui.contexts)if(c.name=="CutScene"){
        ++contexts;target.event=c.event;unsigned listeners=0;
        for(const auto& listener:c.listeners)if(listener[0]==target.profile){if(listener[2]!=0x82770F58)return false;++listeners;}
        if(listeners!=1)return false;
        for(const auto& a:c.actions)for(unsigned i=0;i<3;++i)if(a.name==Names[i]){
            if(target.actions[i] || a.kind!=Kinds[i] || a.condition || a.curve || a.bindings.size()!=1 ||
               a.bindings[0][0]!=7 || a.bindings[0][1]!=0x01400009)return false;
            target.actions[i]=a.action;
        }
    }
    if(contexts!=1 || std::any_of(target.actions.begin(),target.actions.end(),[](auto a){return !a;}))return false;
    out=target;return true;
}
bool DirectHoldReady(uint64_t tick,uint64_t stamp,float duration,bool allowed,bool held,float& elapsed){
    elapsed=0;
    constexpr uint64_t origin=100000000,step=1000000;
    if(!allowed || !held || !std::isfinite(duration) || duration<0 || duration>3600 ||
       tick>(uint64_t(INT64_MAX)-origin)/step || stamp<origin || (stamp-origin)%step)return false;
    const auto now=origin+tick*step;if(stamp>now)return false;
    elapsed=float(float(now-stamp)*float(1./30000000.));
    return elapsed>=duration;
}
struct TimerLease {void* owner=nullptr;DWORD thread=0;uint8_t* base=nullptr;uint64_t controls=0;Target target;};
thread_local TimerLease timer_lease;
}
struct NativeCutsceneSkipReceiver::Impl {
    NativeCutsceneSkipSource source;Target target;DWORD thread=0;uint8_t* base=nullptr;
    uint64_t epoch=0,next=0,controls=0;bool armed=false,faulted=false;
};
NativeCutsceneSkipReceiver::NativeCutsceneSkipReceiver():impl_(std::make_unique<Impl>()){}
NativeCutsceneSkipReceiver::~NativeCutsceneSkipReceiver(){if(timer_lease.owner==impl_.get())timer_lease={};}
bool NativeCutsceneSkipTargetAvailable(uint32_t registry,PPCContext& ctx,uint8_t* base){Target target;return ReadTarget(registry,ctx,base,target);}
bool NativeCutsceneSkipTimerOwned(uint32_t timer,uint32_t caller,PPCContext& ctx,uint8_t* base){
    Environment env;const auto lease=timer_lease;
    if(!lease.owner || lease.thread!=GetCurrentThreadId() || lease.base!=base ||
       lease.controls!=em2::SynchronizedControlsGeneration() || timer!=lease.target.object+(lease.target.mode==1?24u:88u) ||
       (lease.target.mode==1?caller!=0x82B1EF94:(caller!=0x82B211B8 && caller!=0x82B22628)))return false;
    Target current;if(!ReadTarget(lease.target.registry,ctx,base,current) || current!=lease.target)return false;
    // The start stamp is a simulation mutation. Elapsed reads can also serve
    // the retained prompt between native updates using the last published tick.
    uint64_t tick=0;return caller==0x82B22628 || em2::CurrentAgreedSimulationTick(tick);
}
bool NativeDirectMovieSkipReady(uint32_t object,PPCContext& ctx,uint8_t* base,float& elapsed){
    Environment env;elapsed=0;const auto lease=timer_lease;uint64_t tick=0;
    if(!lease.owner || lease.target.mode!=1 || lease.target.object!=object || lease.thread!=GetCurrentThreadId() ||
       lease.base!=base || lease.controls!=em2::SynchronizedControlsGeneration() || !em2::CurrentAgreedSimulationTick(tick))return false;
    Target current;if(!ReadTarget(lease.target.registry,ctx,base,current) || current!=lease.target ||
       !PPC_LOAD_U8(object+12) || PPC_LOAD_U8(object+13))return false;
    return DirectHoldReady(tick,PPC_LOAD_U64(object+24),std::bit_cast<float>(PPC_LOAD_U32(object+16)),
        PPC_LOAD_U8(object+21)==1,PPC_LOAD_U8(object+20)==1,elapsed);
}
bool NativeCutsceneSkipReceiver::Consume(const RoomFrame& frame,uint32_t registry,uint64_t epoch,PPCContext& ctx,uint8_t* base){
    Environment env;auto& s=*impl_;uint64_t tick=0,generation=0;Frame published,neutral;neutral.tick=frame.tick;
    if(s.faulted || !AgreedTick(tick,base) || tick!=frame.tick || frame.tick==UINT64_MAX ||
       !NativeCutsceneReturnPrefix(frame,frame.players_mask,epoch) ||
       !em2::ReadPublishedSynchronizedControls(generation,published) || !generation || published!=neutral)return false;
    Target target;if(!ReadTarget(registry,ctx,base,target))return false;
    if(!s.armed){
        if(timer_lease.owner || !s.source.Arm(registry,0,epoch,tick,ctx,base))return false;
        s.target=target;s.thread=GetCurrentThreadId();s.base=base;s.epoch=epoch;s.next=tick;s.controls=generation;s.armed=true;
        timer_lease={impl_.get(),s.thread,base,generation,target};
        REXLOG_INFO("Native Room cutscene UI receiver armed: tick {} epoch {} handler {:08X}; original authored guard retained",tick,epoch,target.mode==1?0x82B1EDF8u:0x82B210B0u);
    }
    if(s.thread!=GetCurrentThreadId() || s.base!=base || s.epoch!=epoch || s.next!=tick || s.controls!=generation || s.target!=target)return false;
    bool all_ui=true;Pad intent;
    for(unsigned i=0;i<kOnlineRoomCapacity;++i)if(frame.players_mask&(1u<<i)){
        const auto& command=frame.players[i];all_ui=all_ui && command.source==RoomMovementSource::NativeCutsceneUiV6;
        intent.buttons|=command.pad.buttons;
    }
    // UI availability is part of each delayed command. Wait for every human's
    // source to certify the same UI phase before any skip decision can fire.
    if(!all_ui)intent={};
    NativeCutsceneSkipSample sample;
    if(!s.source.Produce(intent,0,epoch,tick,ctx,base,sample))return false;
    s.faulted=true; // Partial callbacks may never be retried as a fresh frame.
    for(unsigned i=0;i<3;++i)if(sample.dispatch[i]){
        Target current;if(!ReadTarget(registry,ctx,base,current) || current!=target)return false;
        BorrowContext preserve(ctx,base);if(!preserve.stack)return false;
        ctx.r3.u64=target.profile;ctx.r4.u64=target.event;ctx.r5.u64=target.actions[i];ctx.f1.f64=0;ctx.f2.f64=0;
        __imp__sub_82770F58(ctx,base);
        REXLOG_INFO("Native Room cutscene UI callback delivered: tick {} epoch {} action '{}' native result {} timer {}; canonical UI only, no character input",tick,epoch,Names[i],ctx.r3.u32,PPC_LOAD_U64(target.object+(target.mode==1?24u:88u)));
        if(target.mode==1)REXLOG_INFO("Native Room direct movie skip settings: tick {} allowed {} held {} duration {}; native fields after '{}'",tick,
            PPC_LOAD_U8(target.object+21),PPC_LOAD_U8(target.object+20),std::bit_cast<float>(PPC_LOAD_U32(target.object+16)),Names[i]);
    }
    s.next=tick+1;s.faulted=false;return true;
}

namespace {
struct MovieTarget {
    uint32_t registry=0,profile=0,object=0,table=0,event=0,action=0,entry=0;
    bool operator==(const MovieTarget&)const=default;
};
struct MovieTimerCall {
    PPCContext* context=nullptr;uint8_t* base=nullptr;DWORD thread=0;
    uint64_t controls=0,tick=0;MovieTarget target;
};
thread_local const MovieTimerCall* movie_timer_call=nullptr;
bool ReadMovieTarget(uint32_t registry,PPCContext& ctx,uint8_t* base,MovieTarget& out){
    BorrowContext preserve(ctx,base);if(!preserve.stack)return false;
    NativeGameOverState state;uint32_t mode=0,handler=0;NativeCutsceneUiSnapshot ui;MovieTarget target;target.registry=registry;
    if(!ReadNativeControlState(ctx,base,state,mode) || mode!=2 || state.registry!=registry ||
       !ReadNativeCutsceneContext(ctx,base,registry) ||
       !ReadNativeControlTarget(ctx,base,registry,2,target.profile,target.object,handler) || handler!=0x82B1DF70 ||
       !Readable(base,target.object,40) || !InspectNativeCutsceneUi(ctx,base,registry,ui))return false;
    target.table=PPC_LOAD_U32(target.object);target.entry=PPC_LOAD_U32(target.object+12);
    if(target.table!=0x8215B2B0 || !Readable(base,target.entry,16) || PPC_LOAD_U32(target.entry)!=0)return false;
    unsigned contexts=0;
    for(const auto& context:ui.contexts)if(context.name=="Menu"){
        ++contexts;target.event=context.event;unsigned listeners=0;
        for(const auto& listener:context.listeners)if(listener[0]==target.profile){if(listener[2]!=0x82770F58)return false;++listeners;}
        if(listeners!=1)return false;
        for(const auto& action:context.actions)if(action.name=="Accept"){
            if(target.action || action.kind!=2 || action.condition || action.curve)return false;target.action=action.action;
        }
    }
    if(contexts!=1 || !target.action)return false;out=target;return true;
}
}
struct NativeMovieAcceptReceiver::Impl {
    DWORD thread=0;uint8_t* base=nullptr;MovieTarget target;NativeCutsceneSkipSource source;
    uint64_t epoch=0,next=0,controls=0;bool armed=false,faulted=false;
};
NativeMovieAcceptReceiver::NativeMovieAcceptReceiver():impl_(std::make_unique<Impl>()){}
NativeMovieAcceptReceiver::~NativeMovieAcceptReceiver()=default;
bool NativeMovieAcceptTimerOwned(uint32_t timer,uint32_t caller,PPCContext& ctx,uint8_t* base){
    const auto* permit=movie_timer_call;uint64_t tick=0;
    if(!permit || caller!=0x82B1E070 || permit->context!=&ctx || permit->base!=base || permit->thread!=GetCurrentThreadId() ||
       permit->controls!=em2::SynchronizedControlsGeneration() || timer!=permit->target.object+32 ||
       !AgreedTick(tick,base) || tick!=permit->tick)return false;
    MovieTarget current;return ReadMovieTarget(permit->target.registry,ctx,base,current) && current==permit->target;
}
bool NativeMovieAcceptTargetAvailable(uint32_t registry,PPCContext& ctx,uint8_t* base){MovieTarget target;Binding binding;
    return ReadMovieTarget(registry,ctx,base,target) && Discover(registry,0,ctx,base,binding,true) && binding.actions[0]==target.action;}
bool NativeMovieAcceptReceiver::Consume(const RoomFrame& frame,uint32_t registry,uint64_t epoch,PPCContext& ctx,uint8_t* base){
    Environment env;auto& s=*impl_;uint64_t tick=0,generation=0;Frame published,neutral;neutral.tick=frame.tick;
    if(s.faulted || !AgreedTick(tick,base) || tick!=frame.tick || tick==UINT64_MAX ||
       !NativeCutsceneReturnPrefix(frame,frame.players_mask,epoch) ||
       !em2::ReadPublishedSynchronizedControls(generation,published) || !generation || published!=neutral)return false;
    MovieTarget target;if(!ReadMovieTarget(registry,ctx,base,target))return false;
    if(!s.armed){
        if(!s.source.ArmMovieAccept(registry,0,epoch,tick,ctx,base))return false;
        s.target=target;s.thread=GetCurrentThreadId();s.base=base;s.epoch=epoch;s.next=tick;s.controls=generation;s.armed=true;
        REXLOG_INFO("Native Room movie Accept receiver armed: tick {} epoch {} handler 82B1DF70; original skip-delay and unskippable guards retained",tick,epoch);
    }
    if(s.thread!=GetCurrentThreadId() || s.base!=base || s.epoch!=epoch || s.next!=tick || s.controls!=generation || s.target!=target)return false;
    bool all_ui=true;Pad intent;
    for(unsigned i=0;i<kOnlineRoomCapacity;++i)if(frame.players_mask&(1u<<i)){
        const auto& command=frame.players[i];all_ui=all_ui && command.source==RoomMovementSource::NativeCutsceneUiV6;intent.buttons|=command.pad.buttons;
    }
    if(!all_ui)intent={};NativeCutsceneSkipSample sample;
    if(!s.source.Produce(intent,0,epoch,tick,ctx,base,sample))return false;
    if(sample.dispatch[1] || sample.dispatch[2])return false;
    s.faulted=true;
    if(sample.dispatch[0]){
        MovieTarget current;if(!ReadMovieTarget(registry,ctx,base,current) || current!=target)return false;
        const auto delay=int32_t(PPC_LOAD_U32(target.entry+12));
        BorrowContext preserve(ctx,base);if(!preserve.stack)return false;
        MovieTimerCall permit{&ctx,base,GetCurrentThreadId(),generation,tick,target};const auto* previous=movie_timer_call;
        struct RestorePermit {const MovieTimerCall* previous;~RestorePermit(){movie_timer_call=previous;}}restore_permit{previous};
        movie_timer_call=&permit;
        ctx.r3.u64=target.profile;ctx.r4.u64=target.event;ctx.r5.u64=target.action;ctx.f1.f64=0;ctx.f2.f64=0;
        __imp__sub_82770F58(ctx,base);
        REXLOG_INFO("Native Room movie Accept delivered: tick {} epoch {} native result {} skip delay {} remaining {:08X} completed {}; original Menu callback, no character input",tick,epoch,ctx.r3.u32,delay,PPC_LOAD_U32(target.object+12),PPC_LOAD_U8(target.object+4));
    }
    s.next=tick+1;s.faulted=false;return true;
}

bool TestNativeCutsceneSkipMath(){
    Environment environment;
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    struct Release {uint8_t* base;~Release(){if(base)VirtualFree(base,0,MEM_RELEASE);}}release{base};
    const auto path=std::filesystem::path(__FILE__).parent_path().parent_path()/"private/image.bin";std::error_code error;const auto size=std::filesystem::file_size(path,error);
    if(!base || error || !VirtualAlloc(base,0x80000,MEM_COMMIT,PAGE_READWRITE) || !VirtualAlloc(base+PPC_IMAGE_BASE,size,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+PPC_IMAGE_BASE+PPC_IMAGE_SIZE,4096,MEM_COMMIT,PAGE_READWRITE))return false;
    std::ifstream image(path,std::ios::binary);if(!image.read(reinterpret_cast<char*>(base+PPC_IMAGE_BASE),size))return false;
    auto map=[&](uint32_t at,PPCFunc* fn){auto* pointer=&PPC_LOOKUP_FUNC(base,at);if(!VirtualAlloc(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pointer)&~uintptr_t(4095)),4096,MEM_COMMIT,PAGE_READWRITE))return false;*pointer=fn;return true;};
    if(!map(0x830A7FD0,__imp__sub_830A7FD0) || !map(0x830A8030,__imp__sub_830A8030) || !map(0x830A8090,__imp__sub_830A8090) || !map(0x830A7968,__imp__sub_830A7968))return false;
    PPCContext ctx{};ctx.fpscr.InitHost();ctx.r1.u64=0x7F000;ctx.lr=0x12345678;
    constexpr uint32_t manager=0x10000,device=0x20000,records=0x28000;
    PPC_STORE_U8(0x83396930,0);PPC_STORE_U64(0x833D03C0,100000000);PPC_STORE_U64(0x833D03C8,1000003);PPC_STORE_U64(0x833D04D8,1000003);
    PPC_STORE_U32(0x833D04E0,std::bit_cast<uint32_t>(float(1./1000003.)));PPC_STORE_U32(0x833B65F8,32);
    // No live context dispatch is installed. The original action lookup sees
    // no default subscriptions; wildcard user7 must find only our private slot.
    PPC_STORE_U32(device,0x821DE6E8);PPC_STORE_U32(device+48,16);
    Binding b;b.manager=manager;b.device=device;
    for(unsigned i=0;i<3;++i){const auto p=records+i*512,name=records+2048+i*96;
        std::memcpy(base+name,Names[i],std::strlen(Names[i])+1);PPC_STORE_U32(p,name);PPC_STORE_U32(p+80,Kinds[i]);PPC_STORE_U32(p+88,7);PPC_STORE_U32(p+92,0x01400009);PPC_STORE_U32(p+408,1);
        std::memcpy(b.shapes[i].data(),base+p,436);b.actions[i]=p;}
    unsigned checks=0;auto check=[&](bool ok,const char* why){++checks;if(!ok)REXLOG_ERROR("Native cutscene skip math: {}",why);return ok;};
    {float elapsed=0;constexpr uint64_t stamp=200000000;
        if(!check(!DirectHoldReady(114,stamp,.5f,true,true,elapsed) && std::abs(elapsed-14.f/30)<1e-6f &&
                  DirectHoldReady(115,stamp,.5f,true,true,elapsed) && elapsed==.5f,
                  "direct-movie hold keeps the configured native half-second boundary on agreed ticks"))return false;
        if(!check(!DirectHoldReady(189,stamp,3.f,true,true,elapsed) && DirectHoldReady(190,stamp,3.f,true,true,elapsed) && elapsed==3.f,
            "the live three-second native setting requires ninety ticks rather than the illustrative half-second setting"))return false;
        for(unsigned bad=0;bad<8;++bad){
            const auto at=bad==0?99ull:bad==1?UINT64_MAX:115ull;
            const auto start=bad==2?0ull:bad==3?stamp+1:stamp;
            const auto duration=bad==4?std::numeric_limits<float>::quiet_NaN():bad==5?-1.f:.5f;
            if(!check(!DirectHoldReady(at,start,duration,bad!=6,bad!=7,elapsed),
                "future/invalid stamp, exhausted clock, invalid duration, unskippable or released hold cannot stop a direct movie"))return false;
        }
        if(!check(!NativeDirectMovieSkipReady(0x5000,ctx,base,elapsed),"direct movie completion requires a live owned receiver lease"))return false;
    }
    std::array<uint8_t,sizeof(PPCContext)> context;std::memcpy(context.data(),&ctx,sizeof(ctx));
    std::array<uint8_t,8192> stack;std::memcpy(stack.data(),base+ctx.r1.u32-8192,8192);
    for(unsigned user=0;user<4;++user){
        b.user=user;auto c=Fresh(b,base);NativeInputClockState clock;
        if(!InitializeNativeInputClock(clock,2+user*2,0,ctx,base))return false;
        unsigned display=0,start=0,stop=0;int held_start=-1;
        for(unsigned tick=0;tick<28;++tick){auto next=c;next.down=tick>=2 && tick<20;next.changed=tick==0 || next.down!=c.down;next.result={2+user*2,tick,{}};
            Evaluation e{b,next,false};
            if(!check(RunNativeInputClock(clock,2+user*2,tick,ctx,base,Evaluate,&e),"original interpreter completes wildcard binding with private device history"))return false;
            display+=next.result.dispatch[0];start+=next.result.dispatch[1];stop+=next.result.dispatch[2];if(next.result.dispatch[1])held_start=int(tick);
            if(!check(next.result.dispatch[0]==(tick==2) && next.result.dispatch[2]==(tick==20),"prompt press and release retain exact native edges"))return false;
            c=std::move(next);
        }
        if(!check(display==1 && start==1 && stop==1 && held_start>2 && held_start<20,"native held action has its own delay/latch instead of a fabricated per-tick callback"))return false;
    }
    if(!check(!std::memcmp(context.data(),&ctx,sizeof(ctx)) && !std::memcmp(stack.data(),base+ctx.r1.u32-8192,8192),"original interpreter preserves caller and entire borrowed stack"))return false;
    for(unsigned i=0;i<3;++i)if(!check(!std::memcmp(base+b.actions[i],b.shapes[i].data(),436),"live action records remain untouched"))return false;
    // Complete public source lifecycle against an isolated, bounded native UI
    // context. Only the control-state getter is a fixture; skip/device/action
    // interpreters and action selection are original recompiled functions.
    constexpr uint32_t registry=0x2000,profile=0x3000,table=0x4000,event=0x11000,name=0x12000,list=0x13000,input=0x23000;
    if(!map(uint32_t(PPC_CODE_BASE),FixtureControlMode))return false;
    PPC_STORE_U32(0x833DA7E4,registry);PPC_STORE_U32(0x833DAD98,profile);PPC_STORE_U32(0x833E36CC,0);
    PPC_STORE_U32(profile+12,table);PPC_STORE_U32(table+20,uint32_t(PPC_CODE_BASE));
    PPC_STORE_U32(0x833DBAD8,manager);PPC_STORE_U32(manager+4,list);PPC_STORE_U32(manager+8,1);PPC_STORE_U32(list,event);
    std::memcpy(base+name,"CutScene",9);std::memcpy(base+name+96,"UI_Global",10);
    PPC_STORE_U32(manager+16,name);PPC_STORE_U32(manager+24,name+96);PPC_STORE_U32(event,name);
    PPC_STORE_U32(event+16,list+96);PPC_STORE_U32(event+20,3);for(unsigned i=0;i<3;++i)PPC_STORE_U32(list+96+i*4,b.actions[i]);
    PPC_STORE_U32(manager+72,input);PPC_STORE_U32(input+80,device);PPC_STORE_U32(device+52,device+256);
    NativeCutsceneSkipSource source;NativeCutsceneSkipSample sample;Pad pad;
    if(!check(!source.Arm(registry,2,1,0,ctx,base) && source.Arm(registry,2,2,100,ctx,base) && !source.Arm(registry,2,2,100,ctx,base),"source arms once with an explicit even input epoch and current native UI binding"))return false;
    if(!check(!source.Produce(pad,1,2,100,ctx,base,sample) && !source.Produce(pad,2,4,100,ctx,base,sample) && !source.Produce(pad,2,2,101,ctx,base,sample),"wrong device, epoch and skipped source ticks cannot advance history"))return false;
    bool foreign=true;std::thread other([&]{auto local=ctx;NativeCutsceneSkipSample result;foreign=source.Produce({},2,2,100,local,base,result);});other.join();
    if(!check(!foreign,"foreign thread cannot evaluate the source"))return false;
    pad.buttons=0x1000;
    if(!check(source.Produce(pad,2,2,100,ctx,base,sample) && sample.dispatch==std::array<bool,3>{true,false,false},"first original press emits prompt only"))return false;
    const auto cached=sample;
    if(!check(source.Produce(pad,2,2,100,ctx,base,sample) && sample==cached && !source.Produce({},2,2,100,ctx,base,sample),"same-tick retry is cached while a conflicting press state is rejected"))return false;
    if(!source.Produce(pad,2,2,101,ctx,base,sample))return false;
    if(!check(source.Produce({},2,2,102,ctx,base,sample) && sample.dispatch==std::array<bool,3>{false,false,true},"short tap releases without fabricating a hold action"))return false;
    // Replacing a selected action may not partially commit its clock/history.
    const auto action=PPC_LOAD_U32(list+96);PPC_STORE_U32(list+96,list+512);
    const bool stale=!source.Produce({},2,2,103,ctx,base,sample);PPC_STORE_U32(list+96,action);
    if(!check(stale && source.Produce({},2,2,103,ctx,base,sample),"stale definition rejects transactionally before source time commits"))return false;
    pad.buttons=0xFFFF;pad.left_trigger=255;pad.left_x=20000;
    if(!check(source.Produce(pad,2,2,104,ctx,base,sample) && sample.dispatch==std::array<bool,3>{true,false,false},"other buttons and axes cannot introduce pause, quick-exit, join or gameplay decisions"))return false;
    // Native receiver fixture uses original mode-6 handler with its authored
    // unskippable guard. Only the state getter and agreed clock are injected.
    // Count calls while forwarding every one to original 82B210B0.
    if(!map(uint32_t(PPC_CODE_BASE),FixtureSceneMode) || !map(uint32_t(PPC_CODE_BASE)+4,FixtureSceneObject) ||
       !map(0x82B210B0,FixtureSceneCallback))return false;
    PPC_STORE_U32(table+28,uint32_t(PPC_CODE_BASE)+4);PPC_STORE_U32(profile,0x8207B4B4);
    PPC_STORE_U32(0x5000,0x8215C7C8);PPC_STORE_U8(0x5000+56,1);
    PPC_STORE_U32(event+4,list+256);PPC_STORE_U32(event+8,1);PPC_STORE_U32(list+256,profile);PPC_STORE_U32(input+72,device);
    if(!check(NativeCutsceneSkipTargetAvailable(registry,ctx,base),"original mode-6 state and exact profile subscription admit the scene handler"))return false;
    if(!check(em2::BeginSynchronizedControls(200),"receiver fixture owns a new controls generation"))return false;
    struct FixtureScope {uint64_t controls=em2::SynchronizedControlsGeneration();~FixtureScope(){fixture_base=nullptr;em2::EndSynchronizedControls(controls);}}fixture_scope;
    fixture_base=base;fixture_callbacks=0;
    NativeCutsceneSkipReceiver receiver;RoomFrame ui;ui.players_mask=7;
    for(unsigned i=0;i<3;++i)ui.players[i]=NativeCutsceneUiCommand(2,false);
    for(uint64_t tick=200;tick<224;++tick){
        fixture_tick=tick;ui.tick=tick;ui.players[2]=NativeCutsceneUiCommand(2,tick<220);
        ui.players[0]=tick==200?NativeCutsceneCommand(2):NativeCutsceneUiCommand(2,false);Frame canonical;canonical.tick=tick;
        if(!em2::PublishSynchronizedControls(canonical))return false;
        if(!check(receiver.Consume(ui,registry,2,ctx,base),"extra-peer UI reaches original mode-6 handler under neutral character controls"))return false;
        if(tick==200 && !check(fixture_callbacks==0,"one peer awaiting UI context prevents all skip callbacks despite another peer holding A"))return false;
        const auto calls=fixture_callbacks;
        if(!check(!receiver.Consume(ui,registry,2,ctx,base) && fixture_callbacks==calls,"duplicate native delivery cannot repeat callbacks"))return false;
    }
    if(!check(fixture_callbacks==3 && PPC_LOAD_U8(0x5000+80)==0 && PPC_LOAD_U64(0x5000+88)==0,
        "prompt/hold/release each reach original handler once and authored unskippable guard leaves skip state and timer untouched"))return false;
    fixture_tick=224;ui.tick=224;Frame wrong;wrong.tick=224;wrong.players[0].buttons=0x1000;
    if(!check(em2::PublishSynchronizedControls(wrong) && !receiver.Consume(ui,registry,2,ctx,base) && fixture_callbacks==3,
        "a UI receiver cannot publish or consume character button input"))return false;
    Frame neutral;neutral.tick=225;fixture_tick=225;ui.tick=225;
    if(!em2::PublishSynchronizedControls(neutral))return false;
    PPC_STORE_U32(table+20,uint32_t(PPC_CODE_BASE));if(!map(uint32_t(PPC_CODE_BASE),FixtureControlMode))return false;
    if(!check(!NativeCutsceneSkipTargetAvailable(registry,ctx,base) && !receiver.Consume(ui,registry,2,ctx,base) && fixture_callbacks==3,
        "restriction mode3 cannot masquerade as the authored cinematic handler"))return false;
    // State 2 has a separate Menu/Accept receiver. Run its original handler
    // against a real -1 skip-delay guard without opening or closing a movie.
    if(!map(uint32_t(PPC_CODE_BASE),FixtureMovieMode) || !map(0x82B1DF70,FixtureMovieCallback))return false;
    PPC_STORE_U32(0x5000,0x8215B2B0);PPC_STORE_U32(0x5000+12,0x5400);PPC_STORE_U8(0x5000+4,0);
    PPC_STORE_U32(0x5400,0);PPC_STORE_U32(0x5400+12,0xFFFFFFFF);
    std::memset(base+name,0,96);std::memcpy(base+name,"Menu",5);std::memcpy(base+name+192,"Accept",7);
    PPC_STORE_U32(event+20,1);PPC_STORE_U32(b.actions[0],name+192);
    Record live_movie_action;std::memcpy(live_movie_action.data(),base+b.actions[0],436);
    if(!check(NativeMovieAcceptTargetAvailable(registry,ctx,base) && !NativeCutsceneSkipTargetAvailable(registry,ctx,base),
        "movie Menu target is distinct from cinematic CutScene target"))return false;
    NativeMovieAcceptReceiver movie_receiver;fixture_callbacks=0;fixture_movie_timer_lease=true;
    for(uint64_t tick=226;tick<231;++tick){
        fixture_tick=tick;ui.tick=tick;
        for(unsigned i=0;i<3;++i)ui.players[i]=NativeCutsceneUiCommand(2,false);
        ui.players[2]=NativeCutsceneUiCommand(2,tick!=229);
        if(tick==226)ui.players[0]=NativeCutsceneCommand(2);
        Frame canonical;canonical.tick=tick;if(!em2::PublishSynchronizedControls(canonical))return false;
        if(!check(movie_receiver.Consume(ui,registry,2,ctx,base),"movie Accept uses original private press math and original profile callback"))return false;
        if(tick==226 && !check(fixture_callbacks==0,"all movie peers must certify UI before a held Accept can dispatch"))return false;
        const auto calls=fixture_callbacks;
        if(!check(!movie_receiver.Consume(ui,registry,2,ctx,base) && fixture_callbacks==calls,"movie callback cannot repeat on duplicate delivery"))return false;
    }
    if(!check(fixture_callbacks==2 && PPC_LOAD_U32(0x5000+12)==0x5400 && PPC_LOAD_U8(0x5000+4)==0 &&
        PPC_LOAD_U32(0x5400+12)==0xFFFFFFFF && !std::memcmp(live_movie_action.data(),base+b.actions[0],436),
        "original unskippable movie guard and live Menu action remain unchanged after actual native Accept callbacks"))return false;
    if(!check(fixture_movie_timer_lease && !NativeMovieAcceptTimerOwned(0x5020,0x82B1E070,ctx,base),
        "movie timer permission exists only inside its exact original callback, for its own elapsed timer and caller"))return false;
    PPC_STORE_U32(0x5400,4);
    if(!check(!NativeMovieAcceptTargetAvailable(registry,ctx,base),"credits cannot obtain movie Accept authority"))return false;
    REXLOG_INFO("Native cutscene skip math passed {} original-interpreter, source-lifecycle and guarded callback checks across four wildcard slots; injected state/clock, no real story skip claim",checks);return true;
}
}

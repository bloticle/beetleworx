#include "online_retail_input.h"
#include "allocation_trace.h"
#include "controls.h"
#include "simulation.h"
#include "online_room_bootstrap.h"
#include "online_retail_jump.h"
#include "guest_read_batch.h"
#include <windows.h>
#include <immintrin.h>
#include <bit>
#include <cstring>
#include <limits>
#include <filesystem>
#include <fstream>
#include <thread>
#include <stdexcept>
#include <rex/logging.h>
PPC_EXTERN_FUNC(__imp__sub_82732A18);
PPC_EXTERN_FUNC(__imp__sub_82A3C930);
PPC_EXTERN_FUNC(__imp__sub_827B58D0);
PPC_EXTERN_FUNC(__imp__sub_82A69948);
PPC_EXTERN_FUNC(__imp__sub_827CA160);
PPC_EXTERN_FUNC(__imp__sub_827835A8);
PPC_EXTERN_FUNC(__imp__sub_827874B0);
PPC_EXTERN_FUNC(__imp__sub_827C3D78);
PPC_EXTERN_FUNC(__imp__sub_8276BA50);
namespace {
using namespace em2::net;
using Result=ActorInputResult;
struct Environment {unsigned fp=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(fp);SetLastError(error);}};
struct Preserve {
    PPCContext& ctx;std::array<uint8_t,sizeof(PPCContext)> saved;Environment environment;
    uint8_t* memory=nullptr;std::array<uint8_t,8192> bytes{};uint32_t stack=0;
    Preserve(PPCContext& c,uint8_t* base):ctx(c){std::memcpy(saved.data(),&c,sizeof(c));const auto top=c.r1.u32&~15u;
        if(base && top>=12288 && em2::GuestReadBatch::Readable(base+top-8192,8192)){
            memory=base+top-8192;std::memcpy(bytes.data(),memory,bytes.size());stack=top-4096;ctx.r1.u64=stack;}}
    ~Preserve(){if(memory)std::memcpy(memory,bytes.data(),bytes.size());std::memcpy(&ctx,saved.data(),sizeof(ctx));}
};
bool Readable(uint32_t p,size_t n,uint8_t* base){return base && p>=4096 && uint64_t(p)+n<=0x100000000ull && em2::GuestReadBatch::Readable(base+p,n);}
struct Stage {RoomMovement command{};bool prepared=false,delivered=false,consumed=false;};
struct RetailState {
    DWORD thread=0;uint8_t* base=nullptr;uint64_t serial=0,world=0,controls=0,next=0,tick=0;
    uint16_t mask=0;bool active=false,started=false,faulted=false,camera=false;
    Frame published{};std::array<RetailActorInput,2> actors{};std::array<Stage,2> stages{};
    RetailJumpAdmission jump=nullptr;
};
thread_local RetailState state;
thread_local uint64_t serial=0;
thread_local int replay_slot=-1,update_slot=-1;
// Private allocated-image fixture only; production always uses the real clock.
thread_local uint8_t* fixture_base=nullptr;
thread_local uint64_t fixture_tick=0;
bool Clock(uint64_t& tick,uint8_t* base){if(fixture_base && fixture_base==base){tick=fixture_tick;return true;}return em2::CurrentAgreedSimulationTick(tick);}
bool Owner(uint8_t* base){return state.thread==GetCurrentThreadId() && state.world && state.base==base;}
bool Guid(uint32_t entity,std::array<uint32_t,4>& out,uint8_t* base){if(!Readable(entity,96,base))return false;uint32_t any=0;
    for(unsigned i=0;i<4;++i){out[i]=PPC_LOAD_U32(entity+4+i*4);any|=out[i];}return any!=0;}
bool Resolve(uint32_t registry,uint32_t handle,PPCContext& ctx,uint8_t* base,uint32_t& entity){
    Preserve preserve(ctx,base);if(!preserve.memory)return false;const auto key=preserve.stack+512;
    PPC_STORE_U32(key,handle);ctx.r3.u64=registry;ctx.r4.u64=key;__imp__sub_82732A18(ctx,base);entity=ctx.r3.u32;return entity!=0;}
uint32_t Typed(uint32_t entity,uint32_t global,uint8_t* base){
    if(!Readable(global,1,base) || !Readable(entity,96,base))return 0;
    const auto types=PPC_LOAD_U32(entity+88),list=PPC_LOAD_U32(entity+36);const auto kind=PPC_LOAD_U8(global);
    if(!Readable(types,unsigned(kind)+1,base))return 0;const auto slot=PPC_LOAD_U8(types+kind);
    if(slot==255 || !Readable(list,unsigned(slot)*4+4,base))return 0;return PPC_LOAD_U32(list+unsigned(slot)*4);
}
bool Inspect(RetailActorInput& a,PPCContext& ctx,uint8_t* base){
    em2::GuestReadBatch reads;Environment environment;
    if(a.slot>1 || !a.world_epoch || !a.actor_epoch || !a.generation ||
       !a.load_generation || a.load_generation==UINT64_MAX || em2::CurrentNativeLoadGeneration()!=a.load_generation ||
       !Readable(0x833DA7E4,4,base) || PPC_LOAD_U32(0x833DA7E4)!=a.registry || !Readable(a.registry,24,base) ||
       !Readable(0x833DAB60,4,base))return false;
    const auto manager=PPC_LOAD_U32(0x833DAB60);
    if(!Readable(manager,640,base) || !PPC_LOAD_U8(manager+357))return false;
    const auto profile=PPC_LOAD_U32(0x833DAD98);
    if(!Readable(profile,103,base) || PPC_LOAD_U8(profile+102))return false;
    a.manager=manager;const auto record=manager+a.slot*320;
    a.handle=PPC_LOAD_U32(record+20);
    if(!a.handle || PPC_LOAD_U32(record+32)!=0 || PPC_LOAD_U32(manager+(1-a.slot)*320+20)==a.handle ||
       !Resolve(a.registry,a.handle,ctx,base,a.entity) || !Readable(a.entity,96,base) ||
       PPC_LOAD_U32(a.entity)!=0x8208D908 || !Guid(a.entity,a.entity_guid,base))return false;
    uint32_t other=0;const auto other_handle=PPC_LOAD_U32(manager+(1-a.slot)*320+20);
    if(other_handle && Resolve(a.registry,other_handle,ctx,base,other) && other==a.entity)return false;
    const auto face=Typed(a.entity,0x833E2188,base);
    if(face<24 || !Readable(face-24,800,base) || PPC_LOAD_U32(face)!=0x8211DCE0)return false;
    a.controller=face-24;a.listener=a.controller+80;
    if(PPC_LOAD_U32(a.controller)!=0x8211DF18 || PPC_LOAD_U32(a.listener)!=0x8211DCD0 ||
       PPC_LOAD_U32(0x8211DCD0)!=0x82A3C930 || PPC_LOAD_U32(a.controller+64)!=a.entity-8 ||
       PPC_LOAD_U32(a.controller+16)!=0x8211DE70 || Typed(a.entity,0x833DAE58,base)!=face)return false;
    const auto component=face;
    if((PPC_LOAD_U8(component+44)&0x80) || PPC_LOAD_U8(component+233) ||
       PPC_LOAD_U32(component+284) || PPC_LOAD_U8(component+289))return false;
    const auto motion_face=Typed(a.entity,0x833DAF54,base);
    if(motion_face<8 || !Readable(motion_face-8,896,base))return false;a.motion=motion_face-8;
    if(PPC_LOAD_U32(a.motion)!=0x8210B598 || PPC_LOAD_U32(0x8210B598+144)!=0x829F9BA0 ||
       PPC_LOAD_U32(a.motion+48)!=a.entity-8)return false;
    a.reference=PPC_LOAD_U32(component+72);
    if(a.reference==a.entity || !Readable(a.reference,96,base) || PPC_LOAD_U32(a.reference)!=0x8208D908 ||
       PPC_LOAD_U32(0x8208D908+172)!=0x827CA160 || PPC_LOAD_U32(a.reference+44)!=2 || !Guid(a.reference,a.reference_guid,base))return false;
    const auto components=PPC_LOAD_U32(a.reference+36);if(!Readable(components,8,base))return false;
    const auto frame=PPC_LOAD_U32(components),camera=PPC_LOAD_U32(components+4);
    if(!Readable(frame,4,base) || !Readable(camera,4,base) || PPC_LOAD_U32(frame)!=0x82090340 || PPC_LOAD_U32(camera)!=0x820D8970)return false;
    const auto uid=PPC_LOAD_U32(0x833DAEB8+a.slot*12);if(!uid)return false;
    for(unsigned offset:{280u,284u,288u,292u,296u,300u})if(PPC_LOAD_U32(a.listener+offset)==uid)return false;
    return true;
}
bool Current(unsigned slot,PPCContext& ctx,uint8_t* base){if(!Owner(base) || slot>1 || !(state.mask&(1u<<slot)))return false;
    auto a=state.actors[slot];return Inspect(a,ctx,base) && a==state.actors[slot];}
bool CurrentControllerOwner(unsigned slot,PPCContext& ctx,uint8_t* base){
    // Quarantine persists if the same actor enters pause/transition after a
    // partially delivered frame. It must not claim a replacement/load reuse.
    const auto& a=state.actors[slot];
    if(em2::CurrentNativeLoadGeneration()!=a.load_generation || !Readable(0x833DA7E4,4,base) ||
       PPC_LOAD_U32(0x833DA7E4)!=a.registry || !Readable(a.manager,640,base) ||
       PPC_LOAD_U32(0x833DAB60)!=a.manager || PPC_LOAD_U32(a.manager+slot*320+20)!=a.handle ||
       PPC_LOAD_U32(a.manager+slot*320+32)!=0)return false;
    uint32_t entity=0;std::array<uint32_t,4> guid{};
    if(!Resolve(a.registry,a.handle,ctx,base,entity) || entity!=a.entity || !Guid(entity,guid,base) || guid!=a.entity_guid ||
       Typed(entity,0x833E2188,base)!=a.controller+24 || !Readable(a.controller,800,base))return false;
    return PPC_LOAD_U32(a.controller)==0x8211DF18 && PPC_LOAD_U32(a.controller+24)==0x8211DCE0 &&
        PPC_LOAD_U32(a.listener)==0x8211DCD0 && PPC_LOAD_U32(a.controller+64)==entity-8;
}
bool Published(uint64_t tick){Frame frame;uint64_t generation=0;
    return em2::ReadPublishedSynchronizedControls(generation,frame) && generation==state.controls && frame.tick==tick &&
        (!state.active || frame==state.published);}
bool Active(uint64_t tick,uint8_t* base){uint64_t agreed=0;return Owner(base) && !state.faulted && state.active && state.tick==tick &&
    Clock(agreed,base) && agreed==tick && Published(tick);}
Result Validate(unsigned slot,const RoomMovement& command,PPCContext& ctx,uint8_t* base){
    if(!Owner(base))return Result::WrongThread;if(state.faulted || !Current(slot,ctx,base))return Result::Stale;
    if(!ValidRoomMovement(command) || command.actor_epoch!=state.actors[slot].actor_epoch)return Result::InvalidInput;
    if(command.pad.left_trigger || command.pad.right_trigger || (!state.camera && (command.pad.right_x || command.pad.right_y)))return Result::UnsupportedAction;
    if(command.source==RoomMovementSource::NativeActionV2){if(!state.jump || !state.jump(slot,command,ctx,base))return Result::UnsupportedAction;}
    else if(command.pad.buttons || command.jump_count || command.jump_button_mask)return Result::UnsupportedAction;
    return Result::Applied;
}
bool Same(float a,float b){return std::bit_cast<uint32_t>(a)==std::bit_cast<uint32_t>(b);}
bool Member(uint32_t list,uint32_t count,uint32_t value,unsigned limit,uint8_t* base){
    if(!count || count>limit || !Readable(list,size_t(count)*4,base))return false;
    unsigned hits=0;for(unsigned i=0;i<count;++i)hits+=PPC_LOAD_U32(list+i*4)==value;return hits==1;
}
bool ActionFor(const RetailActorInput& actor,const NativeActorAction& action,PPCContext& ctx,uint8_t* base){
    if(!Readable(action.action,436,base) || !Readable(action.event,24,base) || !Readable(0x833DBAD8,4,base))return false;
    const auto manager=PPC_LOAD_U32(0x833DBAD8);if(!Readable(manager,76,base))return false;
    if(action.event!=manager+24){
        // Same native selected-context resolution as the original dispatcher;
        // a retired event/action allocation cannot satisfy UID equality alone.
        Preserve preserve(ctx,base);if(!preserve.memory)return false;
        ctx.r3.u64=manager;ctx.r4.u64=manager+16;__imp__sub_827C3D78(ctx,base);
        if(ctx.r3.u32!=action.event)return false;
    }
    return PPC_LOAD_U32(action.action+80)==4 && PPC_LOAD_U32(action.action+408)==2 && !PPC_LOAD_U32(action.action+412) &&
        Member(PPC_LOAD_U32(action.event+4),PPC_LOAD_U32(action.event+8),actor.listener,128,base) &&
        Member(PPC_LOAD_U32(action.event+16),PPC_LOAD_U32(action.event+20),action.action,256,base);
}
struct ControllerDone {};
void FixtureNoState(PPCContext& ctx,uint8_t*){ctx.r3.u64=0;}
void FixtureFrameEnd(PPCContext&,uint8_t*){throw ControllerDone{};}
void FixtureProperty(PPCContext& ctx,uint8_t* base){
    if(ctx.lr!=0x827CA1B4 || ctx.r4.u32!=0x833DA808 || ctx.r6.u32)throw std::runtime_error("retail reference ABI");
    const auto reference=PPC_LOAD_U32(ctx.r3.u32+4);
    if(PPC_LOAD_U32(ctx.r3.u32)==0x820D8970){ctx.r3.u64=0;return;}
    std::memcpy(base+ctx.r5.u32,base+reference+256,48);ctx.r3.u64=1;
}
void FixtureMotion(PPCContext& ctx,uint8_t* base){
    if(!ApplyRetailBasisMotion(ctx,base,__imp__sub_827874B0))__imp__sub_827874B0(ctx,base);
}
void FixtureBoundedController(PPCContext& ctx,uint8_t* base){
    try{__imp__sub_827835A8(ctx,base);}catch(const ControllerDone&){return;}
    throw std::runtime_error("retail native controller continuation missing");
}
}
namespace em2::net {
bool ArmRetailInput(uint64_t world,uint32_t registry,uint64_t controls,uint16_t mask,const std::array<uint64_t,2>& epochs,uint64_t first,PPCContext& ctx,uint8_t* base){
    Environment environment;
    if(state.world){
        if(!Owner(base) || state.world!=world || state.controls!=controls || state.mask!=mask || state.active || state.started || state.faulted ||
           controls!=em2::SynchronizedControlsGeneration())return false;
        for(unsigned i=0;i<2;++i)if((mask&(1u<<i)) &&
            (state.actors[i].actor_epoch!=epochs[i] || state.actors[i].registry!=registry || !Current(i,ctx,base)))return false;
        return true; // Rebase alone may change the proposed cutover tick.
    }
    if(!world || !registry || !controls || !mask || (mask&~3u) || first==UINT64_MAX || serial==UINT64_MAX ||
       controls!=em2::SynchronizedControlsGeneration())return false;
    const auto load=em2::CurrentNativeLoadGeneration();if(!load || load==UINT64_MAX)return false;
    RetailState candidate;candidate.thread=GetCurrentThreadId();candidate.base=base;candidate.serial=serial+1;candidate.world=world;
    candidate.controls=controls;candidate.mask=mask;candidate.next=first;
    for(unsigned i=0;i<2;++i)if(mask&(1u<<i)){
        auto& a=candidate.actors[i];a.world_epoch=world;a.actor_epoch=epochs[i];a.generation=candidate.serial;a.load_generation=load;a.slot=i;a.registry=registry;
        if(!Inspect(a,ctx,base))return false;
    }
    if(mask==3 && (candidate.actors[0].controller==candidate.actors[1].controller || candidate.actors[0].reference==candidate.actors[1].reference))return false;
    serial=candidate.serial;state=candidate;return true;
}
bool RebaseRetailInputFirstTick(uint64_t next,PPCContext& ctx,uint8_t* base){
    Environment environment;const auto* gate=CurrentRoomBootstrapGate();
    if(!Owner(base) || state.active || state.started || state.faulted || !next || next==UINT64_MAX ||
       !gate || !gate->active() || !gate->completion_pending() || gate->committed_tick()!=next-1 || !Published(next-1))return false;
    for(unsigned i=0;i<2;++i)if((state.mask&(1u<<i)) && !Current(i,ctx,base))return false;
    state.next=next;return true;
}
void DisarmRetailInput(){if(state.world && state.thread!=GetCurrentThreadId())return;state={};replay_slot=update_slot=-1;}
bool RetailActorInputFor(unsigned slot,PPCContext& ctx,uint8_t* base,RetailActorInput& out){Environment preserve;out={};if(!Current(slot,ctx,base))return false;out=state.actors[slot];return true;}
bool RetailInputQuarantineFor(unsigned slot,PPCContext& ctx,uint8_t* base,RetailActorInput& out){
    Environment environment;out={};
    if(!Owner(base) || slot>1 || !(state.mask&(1u<<slot)) || !state.started || !Published(state.tick) ||
       !CurrentControllerOwner(slot,ctx,base))return false;
    // Published() compares the saved frame during an active tick. Completed
    // tick gaps must retain the same exact frame too, not merely its tick.
    Frame frame;uint64_t generation=0;
    if(!em2::ReadPublishedSynchronizedControls(generation,frame) || generation!=state.controls || frame!=state.published)return false;
    out=state.actors[slot];return true;
}
bool SetRetailJumpAdmission(RetailJumpAdmission admission){if(!state.world || state.thread!=GetCurrentThreadId() || state.active || (admission && state.started))return false;state.jump=admission;return true;}
bool SetRetailCameraInputAllowed(bool allowed){if(!state.world || state.thread!=GetCurrentThreadId() || state.active || state.started)return false;state.camera=allowed;return true;}
ActorInputResult ValidateRetailMovement(unsigned slot,const RoomMovement& c,PPCContext& ctx,uint8_t* base){Environment environment;return Validate(slot,c,ctx,base);}
ActorInputResult ValidateRetailMovementFrame(const RoomFrame& frame,PPCContext& ctx,uint8_t* base){Environment environment;
    if(!Owner(base))return Result::WrongThread;if((frame.players_mask&3)!=state.mask || frame.tick!=state.next || state.active)return Result::InvalidTick;
    for(unsigned i=0;i<2;++i)if(state.mask&(1u<<i)){const auto r=Validate(i,frame.players[i],ctx,base);if(r!=Result::Applied)return r;}return Result::Applied;
}
ActorInputResult ValidateRetailMovementAction(unsigned slot,const NativeActorAction& action,PPCContext& ctx,uint8_t* base){
    Environment environment;
    if(!Owner(base))return Result::WrongThread;
    if(state.faulted || !Current(slot,ctx,base))return Result::Stale;
    if(!std::isfinite(action.x) || !std::isfinite(action.y) || std::abs(action.x)>1 || std::abs(action.y)>1 ||
       !Readable(action.action,4,base))return Result::InvalidInput;
    if(PPC_LOAD_U32(action.action)!=PPC_LOAD_U32(0x833DAEB8+slot*12))return Result::UnsupportedAction;
    return ActionFor(state.actors[slot],action,ctx,base)?Result::Applied:Result::InvalidContext;
}
bool BeginRetailBasisTick(uint32_t registry,uint64_t tick,PPCContext& ctx,uint8_t* base){Environment environment;uint64_t agreed=0;
    if(!Owner(base) || state.faulted || state.active || state.next!=tick || !Clock(agreed,base) || agreed!=tick || !Published(tick))return false;
    for(unsigned i=0;i<2;++i)if((state.mask&(1u<<i)) && (state.actors[i].registry!=registry || !Current(i,ctx,base)))return false;
    uint64_t generation=0;Frame frame;if(!em2::ReadPublishedSynchronizedControls(generation,frame) || generation!=state.controls)return false;
    state.published=frame;state.tick=tick;state.active=true;state.started=true;state.stages={};return true;
}
ActorInputResult PrepareRetailMovement(unsigned slot,const RoomMovement& c,uint64_t tick,PPCContext& ctx,uint8_t* base){Environment environment;
    if(!Active(tick,base))return Result::InvalidTick;const auto result=Validate(slot,c,ctx,base);if(result!=Result::Applied)return result;
    auto& stage=state.stages[slot];if(stage.prepared || c.pad!=state.published.players[slot])return Result::InvalidInput;
    stage.command=c;stage.prepared=true;return Result::Applied;
}
ActorInputResult ReplayRetailMovement(unsigned slot,const NativeActorAction& action,uint64_t tick,PPCContext& ctx,uint8_t* base){
    Environment environment;if(!Active(tick,base))return Result::InvalidTick;if(!Current(slot,ctx,base))return Result::Stale;
    if(replay_slot!=-1 || update_slot!=-1)return Result::Nested;auto& s=state.stages[slot];
    if(!s.prepared || s.delivered || !Same(action.x,s.command.x) || !Same(action.y,s.command.y))return Result::InvalidInput;
    const auto admitted=ValidateRetailMovementAction(slot,action,ctx,base);if(admitted!=Result::Applied)return admitted;
    // Native callback keeps retail lookup, task/FSM, locks and individual gates.
    Preserve preserve(ctx,base);if(!preserve.memory)return Result::InvalidContext;
    s.delivered=true;replay_slot=int(slot);struct Reset {~Reset(){replay_slot=-1;}}reset;
    ctx.r3.u64=state.actors[slot].listener;ctx.r4.u64=action.event;ctx.r5.u64=action.action;ctx.f1.f64=action.x;ctx.f2.f64=action.y;
    try{__imp__sub_82A3C930(ctx,base);}catch(...){state.faulted=true;throw;}return Result::Applied;
}
bool RetailInputTickFor(unsigned slot,uint64_t tick,PPCContext& ctx,uint8_t* base,RetailActorInput& out,bool& delivered,RoomMovement* command){
    Environment environment;out={};delivered=false;if(command)*command={};if(!Active(tick,base) || !Current(slot,ctx,base))return false;
    out=state.actors[slot];delivered=state.stages[slot].delivered;if(command && state.stages[slot].prepared)*command=state.stages[slot].command;return true;
}
bool FinishRetailInputTick(uint64_t tick,PPCContext& ctx,uint8_t* base){Environment environment;
    if(!Active(tick,base) || replay_slot!=-1 || update_slot!=-1 || tick==UINT64_MAX)return false;
    for(unsigned i=0;i<2;++i)if(state.mask&(1u<<i))if(!Current(i,ctx,base) || !state.stages[i].prepared || !state.stages[i].delivered || !state.stages[i].consumed){state.faulted=true;return false;}
    state.active=false;state.next=tick+1;return true;
}
bool SuppressRetailMovement(PPCContext& ctx,uint8_t* base){Environment environment;
    // An armed reservation never changes bootstrap input. Once a gameplay
    // frame begins, a partial/stopped frame remains quarantined until explicit
    // teardown, even outside the transient native clock or after a fault.
    if(!Owner(base) || !state.started || !Readable(ctx.r5.u32,4,base))return false;
    for(unsigned i=0;i<2;++i){RetailActorInput actor;
       if(ctx.r3.u32!=state.actors[i].listener || !RetailInputQuarantineFor(i,ctx,base,actor) ||
          PPC_LOAD_U32(ctx.r5.u32)!=PPC_LOAD_U32(0x833DAEB8+i*12))continue;
        if(replay_slot==int(i))return false;ctx.r3.u64=0;return true;
    }
    return false;
}
void UpdateRetailBasisController(PPCContext& ctx,uint8_t* base,PPCFunc* next){
    int selected=-1;{Environment environment;if(Owner(base) && Active(state.tick,base))for(unsigned i=0;i<2;++i)
        if((state.mask&(1u<<i)) && ctx.r3.u32==state.actors[i].controller+24){selected=int(i);break;}}
    if(selected<0){const auto previous=update_slot;update_slot=-1;struct Restore{int prior;~Restore(){update_slot=prior;}}restore{previous};next(ctx,base);return;}
    auto& stage=state.stages[selected];
    {Environment environment;if(update_slot!=-1 || !Current(selected,ctx,base) || !stage.prepared || !stage.delivered || stage.consumed){state.faulted=true;return;}}
    update_slot=selected;struct Reset{~Reset(){update_slot=-1;}}reset;
    try{next(ctx,base);}catch(...){state.faulted=true;throw;}
    if(!stage.consumed)state.faulted=true;
}
bool ApplyRetailBasisMotion(PPCContext& ctx,uint8_t* base,PPCFunc* native){
    if(update_slot<0)return false;const auto slot=unsigned(update_slot);auto& stage=state.stages[slot];const auto& a=state.actors[slot];
    bool admitted=false;{Environment environment;admitted=Active(state.tick,base) && Current(slot,ctx,base) && stage.prepared && stage.delivered && !stage.consumed &&
        ctx.lr==0x827837E8 && ctx.r27.u32==a.controller+24 && ctx.r3.u32==a.motion && ctx.r31.u32==ctx.r1.u32 &&
        ctx.r6.u32==ctx.r1.u32+96 && Readable(ctx.r6.u32,16,base);}
    if(!admitted){state.faulted=true;return true;} // Never fall through to a receiver-local basis inside a claimed retail update.
    const auto argument=ctx.r6.u32;std::array<uint8_t,16> saved;std::memcpy(saved.data(),base+argument,16);
    struct Restore{uint8_t* destination;const std::array<uint8_t,16>& saved;~Restore(){std::memcpy(destination,saved.data(),16);}}restore{base+argument,saved};
    stage.consumed=true;
    PPC_STORE_U32(argument,std::bit_cast<uint32_t>(stage.command.forward_x));PPC_STORE_U32(argument+4,0);
    PPC_STORE_U32(argument+8,std::bit_cast<uint32_t>(stage.command.forward_z));PPC_STORE_U32(argument+12,0);
    native(ctx,base);return true;
}
bool TestNativeRetailInput(){
    Environment environment;
    // This fixture owns a private 4GiB guest image, never a running world.
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    struct Restore {
        uint8_t* base;RetailState prior=state;uint64_t serial_before=serial;uint8_t* prior_fixture=fixture_base;uint64_t prior_tick=fixture_tick;
        int prior_replay=replay_slot,prior_update=update_slot;uint64_t controls=0;
        ~Restore(){if(controls)em2::EndSynchronizedControls(controls);state=prior;serial=serial_before;fixture_base=prior_fixture;fixture_tick=prior_tick;
            replay_slot=prior_replay;update_slot=prior_update;if(base)VirtualFree(base,0,MEM_RELEASE);}
    }restore{base};state={};replay_slot=update_slot=-1;fixture_base=nullptr;
    if(em2::SynchronizedControlsGeneration())return false;
    const auto path=std::filesystem::path(__FILE__).parent_path().parent_path()/"private/image.bin";
    std::error_code error;const auto size=std::filesystem::file_size(path,error);
    if(!base || error || !VirtualAlloc(base,0x60000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+PPC_IMAGE_BASE,size,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+PPC_IMAGE_BASE+PPC_IMAGE_SIZE,4096,MEM_COMMIT,PAGE_READWRITE))return false;
    std::ifstream image(path,std::ios::binary);if(!image.read(reinterpret_cast<char*>(base+PPC_IMAGE_BASE),size))return false;
    auto map=[&](uint32_t address,PPCFunc* function){auto* slot=&PPC_LOOKUP_FUNC(base,address);
        if(!VirtualAlloc(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(slot)&~uintptr_t(4095)),4096,MEM_COMMIT,PAGE_READWRITE))return false;
        *slot=function;return true;};
    if(!map(uint32_t(PPC_CODE_BASE),__imp__sub_827B58D0) || !map(uint32_t(PPC_CODE_BASE)+4,__imp__sub_82A69948) ||
       !map(uint32_t(PPC_CODE_BASE)+8,FixtureNoState) || !map(uint32_t(PPC_CODE_BASE)+12,FixtureFrameEnd) ||
       !map(0x827CA160,__imp__sub_827CA160) || !map(0x82759BB8,FixtureProperty) || !map(0x827874B0,FixtureMotion))return false;
    constexpr uint32_t registry=0x2000,manager=0x3000,buckets=0x5000,profile=0x6000,fsm=0x7000,input_manager=0xC000,event=input_manager+24;
    constexpr std::array<uint32_t,2> entities{0x10000,0x12000},controllers{0x18000,0x1A000},motions{0x20000,0x21000},references{0x24000,0x26000};
    PPC_STORE_U32(0x833DA7E4,registry);PPC_STORE_U32(0x833DAB60,manager);PPC_STORE_U32(0x833DAD98,profile);
    PPC_STORE_U32(0x833DBAD8,input_manager);PPC_STORE_U32(event+4,0xB000);PPC_STORE_U32(event+8,2);
    PPC_STORE_U32(event+16,0xB100);PPC_STORE_U32(event+20,2);
    PPC_STORE_U32(registry+4,uint32_t(PPC_CODE_BASE));PPC_STORE_U32(registry+8,uint32_t(PPC_CODE_BASE)+4);
    PPC_STORE_U32(registry+16,buckets);PPC_STORE_U32(registry+20,4);PPC_STORE_U8(manager+357,1);
    // ORIGINAL8276BA50 identifies retail P2 through registry's reverse
    // entity->handle table at+36, not the forward handle table alone.
    PPC_STORE_U32(registry+40,uint32_t(PPC_CODE_BASE));PPC_STORE_U32(registry+44,uint32_t(PPC_CODE_BASE)+4);
    PPC_STORE_U32(registry+52,buckets+256);PPC_STORE_U32(registry+56,4);
    PPC_STORE_U8(0x833E2188,3);PPC_STORE_U8(0x833DAE58,6);PPC_STORE_U8(0x833DAF54,4);PPC_STORE_U8(0x833DBB13,5);
    // These interned identifiers are runtime BSS in the executable image;
    // native input setup populates them before retail actors enter gameplay.
    PPC_STORE_U32(0x833DAEB8,0x8207F5A4);PPC_STORE_U32(0x833DAEC4,0x8207F5C4);
    PPC_STORE_U32(fsm+20,uint32_t(PPC_CODE_BASE)+8);PPC_STORE_U32(fsm+28,uint32_t(PPC_CODE_BASE)+8);PPC_STORE_U32(fsm+12,uint32_t(PPC_CODE_BASE)+12);
    PPC_STORE_U32(0x833DF47C,0x43000);PPC_STORE_U32(0x43000+48,0x43100);
    auto context=[](){PPCContext c{};c.fpscr.InitHost();c.r1.u64=0x5F000;c.lr=0x12345678;c.r13.u64=0x13572468;return c;};
    auto ctx=context();auto put=[&](uint32_t at,float f){PPC_STORE_U32(at,std::bit_cast<uint32_t>(f));};
    for(unsigned i=0;i<2;++i){const auto entity=entities[i],controller=controllers[i],motion=motions[i],reference=references[i];
        const auto types=0x30000u+i*0x1000,list=0x32000u+i*0x1000,node=0x8000u+i*16;
        PPC_STORE_U32(manager+i*320+20,i+1);PPC_STORE_U32(entity,0x8208D908);PPC_STORE_U32(entity+4,100+i);
        PPC_STORE_U32(entity+88,types);PPC_STORE_U32(entity+36,list);std::memset(base+types,255,256);
        PPC_STORE_U8(types+3,0);PPC_STORE_U8(types+6,0);PPC_STORE_U8(types+4,1);
        PPC_STORE_U32(list,controller+24);PPC_STORE_U32(list+4,motion+8);
        PPC_STORE_U32(controller,0x8211DF18);PPC_STORE_U32(controller+16,0x8211DE70);PPC_STORE_U32(controller+24,0x8211DCE0);
        PPC_STORE_U32(controller+80,0x8211DCD0);PPC_STORE_U32(controller+64,entity-8);PPC_STORE_U32(controller+96,reference);PPC_STORE_U32(controller+104,fsm);
        PPC_STORE_U32(motion,0x8210B598);PPC_STORE_U32(motion+48,entity-8);PPC_STORE_U32(motion+568,0x1E);put(motion+876,1);
        PPC_STORE_U32(reference,0x8208D908);PPC_STORE_U32(reference+4,200+i);PPC_STORE_U32(reference+36,reference+512);PPC_STORE_U32(reference+44,2);
        PPC_STORE_U32(reference+512,reference+528);PPC_STORE_U32(reference+516,reference+544);
        PPC_STORE_U32(reference+528,0x82090340);PPC_STORE_U32(reference+532,reference);
        PPC_STORE_U32(reference+544,0x820D8970);PPC_STORE_U32(reference+548,reference);
        for(unsigned j=0;j<12;++j)put(reference+256+j*4,j%5==0?1.f:0.f);
        // Deliberately pitched local camera; peer basis must replace only the
        // original stack vector after the native flatten/normalize routine.
        put(reference+256+32,3);put(reference+256+36,-12);put(reference+256+40,4);
        PPC_STORE_U32(0x9000,i+1);auto hash=context();hash.r3.u64=0x9000;__imp__sub_827B58D0(hash,base);
        const auto bucket=buckets+(hash.r3.u32%4)*4;PPC_STORE_U32(node,i+1);PPC_STORE_U32(node+4,entity);
        PPC_STORE_U32(node+8,PPC_LOAD_U32(bucket));PPC_STORE_U32(bucket,node);
        PPC_STORE_U32(0x9000,entity);hash=context();hash.r3.u64=0x9000;__imp__sub_827B58D0(hash,base);
        const auto reverse_bucket=buckets+256+(hash.r3.u32%4)*4,reverse_node=node+256;
        PPC_STORE_U32(reverse_node,entity);PPC_STORE_U32(reverse_node+4,i+1);
        PPC_STORE_U32(reverse_node+8,PPC_LOAD_U32(reverse_bucket));PPC_STORE_U32(reverse_bucket,reverse_node);
    }
    em2::RecordNativeLoadLevel("retail-fixture");
    unsigned checks=0;auto check=[&](bool value,const char* why){++checks;if(!value)REXLOG_ERROR("Native retail Movement: {}",why);return value;};
    for(unsigned i=0;i<2;++i){auto native=context();native.r3.u64=manager;native.r4.u64=entities[i];native.r5.u64=1;
        __imp__sub_8276BA50(native,base);
        if(!check(native.r3.u32==i,"original reverse-registry retail P2 classification distinguishes the two actual records"))return false;}
    if(!check(em2::BeginSynchronizedControls(10),"controls generation acquired"))return false;
    restore.controls=em2::SynchronizedControlsGeneration();
    std::array<uint8_t,sizeof(PPCContext)> caller;std::memcpy(caller.data(),&ctx,sizeof(ctx));
    std::array<uint8_t,8192> stack;std::memcpy(stack.data(),base+ctx.r1.u32-8192,8192);
    const auto fp=_mm_getcsr();const auto last=GetLastError();
    if(!check(ArmRetailInput(77,registry,restore.controls,3,{101,102},10,ctx,base),"actual two retail records and own typed controllers/references admitted"))return false;
    RetailActorInput a,b;
    if(!check(RetailActorInputFor(0,ctx,base,a) && RetailActorInputFor(1,ctx,base,b) && a.reference!=b.reference &&
       a.generation==b.generation && a.load_generation!=0,"independent retail leases pin native load incarnation"))return false;
    if(!check(ArmRetailInput(77,registry,restore.controls,3,{101,102},12,ctx,base) && state.next==10 &&
       !RebaseRetailInputFirstTick(12,ctx,base),"idempotent admission cannot rebase without actual completed bootstrap ownership"))return false;
    if(!check(!std::memcmp(caller.data(),&ctx,sizeof(ctx)) && !std::memcmp(stack.data(),base+ctx.r1.u32-8192,8192) &&
       fp==_mm_getcsr() && last==GetLastError(),"admission restores complete context, scratch, FP and LastError"))return false;
    RoomFrame frame;frame.tick=10;frame.players_mask=3;
    for(unsigned i=0;i<2;++i){auto& c=frame.players[i];c.actor_epoch=101+i;c.source=RoomMovementSource::NativeActionV1;c.x=.5f;c.y=-.25f;
        c.forward_x=i?-1.f:1.f;c.forward_z=0;}
    if(!check(ValidateRetailMovementFrame(frame,ctx,base)==Result::Applied && ValidateRetailMovementFrame(frame,ctx,base)==Result::Applied,"repeatable read-only complete frame preflight"))return false;
    auto bad=frame.players[0];bad.actor_epoch++;
    if(!check(ValidateRetailMovement(0,bad,ctx,base)==Result::InvalidInput,"foreign actor epoch rejected"))return false;
    bad=frame.players[0];bad.source=RoomMovementSource::NativeActionV2;bad.jump_button_mask=0x1000;
    if(!check(ValidateRetailMovement(0,bad,ctx,base)==Result::UnsupportedAction,"Jump never silently ignored without its armed adapter"))return false;
    bad=frame.players[0];bad.pad.right_x=23000;bad.pad.right_y=-19000;
    if(!check(ValidateRetailMovement(0,bad,ctx,base)==Result::UnsupportedAction && SetRetailCameraInputAllowed(true) &&
       ValidateRetailMovement(0,bad,ctx,base)==Result::Applied,"native canonical camera path requires explicit owner capability"))return false;
    bad.pad.left_trigger=1;
    if(!check(ValidateRetailMovement(0,bad,ctx,base)==Result::UnsupportedAction,"camera capability never admits trigger intent"))return false;
    Frame raw;raw.tick=10;if(!check(em2::PublishSynchronizedControls(raw),"exact controls published"))return false;
    PPC_STORE_U32(0xA000,PPC_LOAD_U32(0x833DAEB8));auto bootstrap_input=context();
    bootstrap_input.r3.u64=a.listener;bootstrap_input.r5.u64=0xA000;
    if(!check(!SuppressRetailMovement(bootstrap_input,base),"armed-only reservation never quarantines raw bootstrap callbacks"))return false;
    if(!check(!BeginRetailBasisTick(registry,10,ctx,base),"production rejects outside agreed clock despite published controls"))return false;
    fixture_base=base;fixture_tick=10;
    if(!check(BeginRetailBasisTick(registry,10,ctx,base) && !BeginRetailBasisTick(registry,10,ctx,base),"private clock fixture claims exactly one published tick"))return false;
    bool foreign=false;std::thread other([&]{auto c=context();foreign=BeginRetailBasisTick(registry,10,c,base);});other.join();
    if(!check(!foreign,"foreign thread cannot mutate admission"))return false;
    for(unsigned i=0;i<2;++i){const auto action=0xA000u+i*512;PPC_STORE_U32(action,PPC_LOAD_U32(0x833DAEB8+i*12));
        // Separate 436-byte action records (not overlapping used tail fields).
        PPC_STORE_U32(action+80,4);PPC_STORE_U32(action+408,2);
        PPC_STORE_U32(0xB000+i*4,state.actors[i].listener);PPC_STORE_U32(0xB100+i*4,action);
        if(!check(PrepareRetailMovement(i,frame.players[i],10,ctx,base)==Result::Applied,"stage exact immutable sender sample"))return false;
        auto ordinary=context();ordinary.r3.u64=state.actors[i].listener;ordinary.r5.u64=action;
        if(!check(SuppressRetailMovement(ordinary,base),"ordinary shared native Movement quarantined only within admitted tick"))return false;
        NativeActorAction input{event,action,.5f,-.25f};
        auto retired=input;retired.event=event+4;
        if(!check(ValidateRetailMovementAction(i,input,ctx,base)==Result::Applied &&
           ValidateRetailMovementAction(i,input,ctx,base)==Result::Applied &&
           ValidateRetailMovementAction(i,retired,ctx,base)!=Result::Applied && !state.stages[i].delivered,
           "local native descriptor preflight is repeatable, rejects foreign event and never claims delivery"))return false;
        const auto replay_result=ReplayRetailMovement(i,input,10,ctx,base);
        if(replay_result!=Result::Applied || std::bit_cast<float>(PPC_LOAD_U32(state.actors[i].listener+172))!=input.x ||
           std::bit_cast<float>(PPC_LOAD_U32(state.actors[i].listener+168))!=input.y)
            REXLOG_ERROR("Native retail fixture callback slot={} result={} x={} y={} expectedX={} expectedY={}",i,int(replay_result),
                std::bit_cast<float>(PPC_LOAD_U32(state.actors[i].listener+172)),std::bit_cast<float>(PPC_LOAD_U32(state.actors[i].listener+168)),input.x,input.y);
        if(!check(replay_result==Result::Applied &&
           std::bit_cast<float>(PPC_LOAD_U32(state.actors[i].listener+172))==input.x &&
           std::bit_cast<float>(PPC_LOAD_U32(state.actors[i].listener+168))==input.y,"ORIGINAL derived and base retail handler preserves independent native caches"))return false;
        if(!check(ReplayRetailMovement(i,input,10,ctx,base)!=Result::Applied,"duplicate native replay rejected"))return false;
    }
    if(!check(!std::memcmp(caller.data(),&ctx,sizeof(ctx)) && !std::memcmp(stack.data(),base+ctx.r1.u32-8192,8192),"native callbacks restore context and scratch"))return false;
    for(unsigned i=0;i<2;++i){auto call=context();call.r3.u64=controllers[i]+24;
        UpdateRetailBasisController(call,base,FixtureBoundedController);
        if(!check(state.stages[i].consumed && std::bit_cast<float>(PPC_LOAD_U32(motions[i]+176))==frame.players[i].forward_x &&
           PPC_LOAD_U32(motions[i]+180)==0 && std::bit_cast<float>(PPC_LOAD_U32(motions[i]+184))==frame.players[i].forward_z,
           "ORIGINAL controller projection and Motion setter receive own sender basis, not local pitched camera"))return false;
    }
    if(!check(FinishRetailInputTick(10,ctx,base) && !FinishRetailInputTick(10,ctx,base),"full native Movement tick completes once"))return false;
    auto ordinary=context();ordinary.r3.u64=a.listener;ordinary.r5.u64=0xA000;
    if(!check(SuppressRetailMovement(ordinary,base),"ordinary input remains quarantined between completed gameplay ticks"))return false;
    RetailActorInput retained;
    if(!check(RetailInputQuarantineFor(0,ctx,base,retained) && retained.generation==a.generation,
       "completed exact controls frame retains same retail quarantine lease"))return false;
    bool foreign_quarantine=true;std::thread foreign_owner([&]{auto c=context();RetailActorInput out;foreign_quarantine=RetailInputQuarantineFor(0,c,base,out);});foreign_owner.join();
    if(!check(!foreign_quarantine,"foreign thread cannot acquire retained quarantine"))return false;
    // Independent lifecycle for the native Jump fixture. Never reset a live
    // production stream to install a capability after gameplay has started.
    em2::EndSynchronizedControls(restore.controls);restore.controls=0;
    if(!check(em2::BeginSynchronizedControls(10),"fresh isolated Jump fixture controls lifecycle"))return false;
    restore.controls=em2::SynchronizedControlsGeneration();
    if(!check(!RetailInputQuarantineFor(0,ctx,base,retained),"replacement controls generation cannot inherit old quarantine"))return false;
    DisarmRetailInput();
    if(!check(ArmRetailInput(77,registry,restore.controls,3,{101,102},10,ctx,base),"fresh Jump receiver leases"))return false;
    auto drive=[&](const RoomFrame& input){
        if(ValidateRetailMovementFrame(input,ctx,base)!=Result::Applied || ValidateRetailJumpFrame(input,ctx,base)!=Result::Applied)return false;
        Frame pads;pads.tick=input.tick;for(unsigned i=0;i<2;++i)pads.players[i]=input.players[i].pad;
        fixture_tick=input.tick;
        if(!em2::PublishSynchronizedControls(pads) || !BeginRetailBasisTick(registry,input.tick,ctx,base))return false;
        for(unsigned i=0;i<2;++i){const auto action=0xA000u+i*512;
            NativeActorAction native{event,action,input.players[i].x,input.players[i].y};
            if(PrepareRetailMovement(i,input.players[i],input.tick,ctx,base)!=Result::Applied ||
               ReplayRetailMovement(i,native,input.tick,ctx,base)!=Result::Applied)return false;
        }
        for(unsigned i=0;i<2;++i)if(ReplayRetailJumpBatch(i,input.players[i],input.tick,ctx,base)!=Result::Applied)return false;
        for(unsigned i=0;i<2;++i){
            // Jump fixture owns its input FSM table; install only our bounded
            // post-Motion continuation for this private controller update.
            const auto previous=PPC_LOAD_U32(controllers[i]+104);PPC_STORE_U32(controllers[i]+104,fsm);
            struct RestoreFsm {uint8_t* base;uint32_t at,previous;~RestoreFsm(){PPC_STORE_U32(at,previous);}}restore_fsm{base,controllers[i]+104,previous};
            auto call=context();call.r3.u64=controllers[i]+24;UpdateRetailBasisController(call,base,FixtureBoundedController);
        }
        return FinishRetailJumpTick(input.tick,ctx,base)==Result::Applied && FinishRetailInputTick(input.tick,ctx,base);
    };
    if(!check(TestRetailJumpReceiver(ctx,base,drive),"native Jump replay consumes complete agreed retail frames"))return false;
    // A partial native frame cannot release ordinary input when its clock
    // scope unwinds. Exercise a real missing-stage fault, not a fake success.
    raw={};raw.tick=state.next;fixture_tick=raw.tick;
    if(!check(em2::PublishSynchronizedControls(raw) && BeginRetailBasisTick(registry,raw.tick,ctx,base),"partial next frame admitted"))return false;
    auto missing=context();missing.r3.u64=controllers[0]+24;
    UpdateRetailBasisController(missing,base,FixtureBoundedController);
    fixture_base=nullptr;PPC_STORE_U8(controllers[0]+257,1);
    ordinary=context();ordinary.r3.u64=controllers[0]+80;ordinary.r5.u64=0xA000;
    if(!check(state.faulted && SuppressRetailMovement(ordinary,base) && ordinary.r3.u32==0 &&
       !FinishRetailInputTick(raw.tick,ctx,base),"faulted partial frame remains quarantined outside clock and through camera transition"))return false;
    PPC_STORE_U8(controllers[0]+257,1);
    if(!check(!RetailActorInputFor(0,ctx,base,a),"transition path invalidates retained lease"))return false;
    PPC_STORE_U8(controllers[0]+257,0);PPC_STORE_U32(references[0]+4,999);
    if(!check(!RetailActorInputFor(0,ctx,base,a),"reused reference address with changed GUID rejected"))return false;
    PPC_STORE_U32(references[0]+4,200);em2::RecordNativeLoadLevel("retail-fixture");
    if(!check(!RetailActorInputFor(0,ctx,base,a),"same-destination native reload invalidates lease even with identical handles"))return false;
    REXLOG_INFO("Native retail Movement: {} checks passed (native physics/contacts not executed)",checks);return true;
}
}

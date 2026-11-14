#include "online_actor_frames.h"
#include "online_room.h"
#include "online_actor_jump.h"
#include "online_button_actions.h"
#include "online_cutscene_input.h"
#include "simulation.h"
#include <algorithm>
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <windows.h>
#include <immintrin.h>
#include <cmath>
#include <bit>
#include <cstring>
#include <thread>
#include <rex/logging.h>
#include <rex/cvar.h>
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DEFINE_BOOL(em2_probe_owned_button_tasks,false,"Debug","Private native owned-action diagnostics: compare extra and retail task flags without changing policy or dispatch");
namespace em2::net {
namespace {
struct ButtonPermit {uint64_t peer,generation,tick;NativeActorAction action;};
thread_local const ButtonPermit* button_permit=nullptr;
struct Environment {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(csr);SetLastError(error);}};
void LogButtonTasks(const char* source,uint64_t peer,uint64_t tick,uint32_t listener,uint8_t* base){
    auto readable=[&](uint32_t p,size_t n){return p>=4096 && uint64_t(p)+n<=0x100000000ull && GuestReadBatch::Readable(base+p,n);};
    if(!readable(listener,120))return;
    const auto array=PPC_LOAD_U32(listener+112),count=PPC_LOAD_U32(listener+116);
    if(count>128 || !readable(array,size_t(count)*4))return;
    for(unsigned i=0;i<count;++i){const auto task=PPC_LOAD_U32(array+i*4);
        if(!readable(task,12))continue;const auto table=PPC_LOAD_U32(task);if(!readable(table,72))continue;
        REXLOG_INFO("Private button task snapshot: {} peer {} tick {} ordinal {} table {:08X} handler {:08X} name {:08X} flags {:08X} entity {:08X}",source,peer,tick,i,table,PPC_LOAD_U32(table+40),PPC_LOAD_U32(table+68),PPC_LOAD_U32(task+8),PPC_LOAD_U32(task+4));
    }
}
bool SameWorld(uint32_t registry,uint8_t* base){return base && GuestReadBatch::Readable(base+0x833DA7E4,4) && PPC_LOAD_U32(0x833DA7E4)==registry;}
ActorLookBasis Basis(uint64_t peer,uint64_t generation,uint64_t tick,const RoomMovement& movement){
    return {peer,generation,tick,{movement.forward_z,0,-movement.forward_x},{movement.forward_x,0,movement.forward_z}};
}
NativeActorAction Action(const RoomMovement& movement,unsigned user){return {0,0x833DAEB8+user*12,movement.x,movement.y};}
std::array<NativeActorJump,kMaxRoomJumpEvents> JumpCommands(uint64_t generation,uint64_t tick,const RoomMovement& movement,unsigned user){
    std::array<NativeActorJump,kMaxRoomJumpEvents> commands{};
    for(unsigned i=0;i<movement.jump_count;++i){const auto& event=movement.jump_events[i];
        const bool down=event.edge==RoomJumpEdge::Pressed;
        commands[i]={generation,tick,event.sequence,down?OwnedJumpEdge::Pressed:OwnedJumpEdge::Released,{0,(down?0x833DAEBCu:0x833DAEC0u)+user*12,1,0},movement.input_epoch};
    }
    return commands;
}
bool ButtonCommands(const NativeTriggerBinding& definitions,const RoomMovement& movement,unsigned user,
                    std::vector<NativeActorAction>& actions,std::array<NativeActorJump,kMaxRoomJumpEvents>& jumps,uint8_t* base){
    if(!ValidateNativeButtonDefinitionSnapshot(definitions,base) || !MatchNativeButtonActions(definitions,movement.button_actions,actions))return false;
    const auto press=PPC_LOAD_U32(0x833DAEBC+user*12),release=PPC_LOAD_U32(0x833DAEC0+user*12);
    unsigned count=0;
    for(const auto& action:actions){const auto uid=PPC_LOAD_U32(action.action);if(uid!=press && uid!=release)continue;
        if(count>=movement.jump_count || movement.jump_events[count].edge!=(uid==press?RoomJumpEdge::Pressed:RoomJumpEdge::Released))return false;
        jumps[count++].action=action;
    }
    return count==movement.jump_count;
}
}
bool detail::OwnedButtonDispatchAuthorized(uint64_t peer,uint64_t generation,uint64_t tick,const NativeActorAction& action)noexcept{
    return button_permit && button_permit->peer==peer && button_permit->generation==generation && button_permit->tick==tick &&
        button_permit->action.event==action.event && button_permit->action.action==action.action && button_permit->action.x==action.x && button_permit->action.y==action.y;
}
bool OwnedMovementFrames::Configure(uint16_t roster,uint16_t retail,const std::array<uint64_t,kOnlineRoomCapacity>& epochs,uint64_t first,uint32_t registry,RetailAdmission admission,uint64_t input_epoch){
    if(configured_ || !input_epoch || !registry || !(roster&1) || (retail&~uint16_t(3)) || (retail&~roster))return false;
    for(unsigned i=0;i<kOnlineRoomCapacity;++i)if(bool(roster&(1u<<i))!=(epochs[i]!=0))return false;
    roster_=roster;retail_=retail;epochs_=epochs;next_tick_=first;registry_=registry;
    input_epoch_=input_epoch;
    owner_thread_=GetCurrentThreadId();retail_admission_=admission;configured_=true;return true;
}
bool OwnedMovementFrames::Bind(uint8_t peer,uint64_t native_peer,PPCContext& ctx,uint8_t* base){
    Environment environment;
    if(!configured_ || owner_thread_!=GetCurrentThreadId() || faulted_ || delivered_ || !peer || peer>kOnlineRoomCapacity)return false;
    const unsigned index=peer-1;
    if(!(roster_&(1u<<index)) || (retail_&(1u<<index)) || bindings_[index].native_peer)return false;
    OwnedActorInput actor;if(!OwnedActorInputFor(native_peer,ctx,base,actor) || actor.registry!=registry_ || !actor.enabled)return false;
    for(const auto& binding:bindings_)if(binding.native_peer==native_peer || binding.entity==actor.entity)return false;
    bindings_[index]={native_peer,actor.generation,actor.entity,actor.controller,actor.action_user};return true;
}
bool OwnedMovementFrames::BindButtons(uint8_t peer,const NativeTriggerBinding& definitions,PPCContext& ctx,uint8_t* base){
    Environment environment;
    if(!configured_ || owner_thread_!=GetCurrentThreadId() || faulted_ || delivered_ || !peer || peer>kOnlineRoomCapacity)return false;
    auto& binding=bindings_[peer-1];OwnedActorInput actor;
    if(!binding.native_peer || binding.buttons || definitions.parent.registry!=registry_ ||
        definitions.parent.logical_user!=binding.action_user || !HashNativeButtonSchema(definitions) ||
        !ValidateNativeButtonDefinitionSnapshot(definitions,base) || !OwnedActorInputFor(binding.native_peer,ctx,base,actor) ||
        actor.generation!=binding.generation || actor.entity!=binding.entity || !actor.enabled)return false;
    binding.buttons=definitions;return true;
}
OwnedFrameResult OwnedMovementFrames::Preflight(const RoomFrame& frame,PPCContext& ctx,uint8_t* base)const{
    Environment environment;
    if(!configured_)return OwnedFrameResult::NotConfigured;
    if(owner_thread_!=GetCurrentThreadId())return OwnedFrameResult::WrongThread;
    if(faulted_)return OwnedFrameResult::Faulted;
    if(delivered_)return OwnedFrameResult::IncompleteTick;
    if(exhausted_ || frame.tick!=next_tick_)return OwnedFrameResult::UnexpectedTick;
    if(frame.players_mask!=roster_)return OwnedFrameResult::InvalidRoster;
    if(!SameWorld(registry_,base))return OwnedFrameResult::StaleActor;
    for(unsigned i=0;i<kOnlineRoomCapacity;++i){
        const auto& movement=frame.players[i];
        if(!(roster_&(1u<<i))){if(movement!=RoomMovement{})return OwnedFrameResult::InvalidRoster;continue;}
        if(movement.actor_epoch!=epochs_[i] || movement.input_epoch!=input_epoch_)return OwnedFrameResult::EpochMismatch;
        if(!ValidRoomMovement(movement))return OwnedFrameResult::InvalidMovement;
        if(movement.source==RoomMovementSource::NativeGameOverV4 || movement.source==RoomMovementSource::NativeCutsceneV5 || movement.source==RoomMovementSource::NativeCutsceneUiV6)return OwnedFrameResult::UnsupportedIntent;
        if(retail_&(1u<<i)){
            // Interpreted edges require a live native retail receiver, not a
            // boolean promise that some other code will consume them later.
            if(movement.source!=RoomMovementSource::NativeActionV1 && !retail_admission_)return OwnedFrameResult::UnsupportedIntent;
            continue;
        }
        const bool buttons=movement.source==RoomMovementSource::NativeActionV3;
        if(buttons && !bindings_[i].buttons)return OwnedFrameResult::UnsupportedIntent;
        const uint16_t allowed=movement.source==RoomMovementSource::NativeActionV2?movement.jump_button_mask:0;
        if(!buttons && ((movement.pad.buttons&~allowed) || movement.pad.left_trigger || movement.pad.right_trigger))return OwnedFrameResult::UnsupportedIntent;
        auto stream=jumps_[i];if(!AdvanceRoomJumpStream(stream,movement))return OwnedFrameResult::InvalidMovement;
        const auto& binding=bindings_[i];if(!binding.native_peer)return OwnedFrameResult::UnboundActor;
        OwnedActorInput actor;
        if(!OwnedActorInputFor(binding.native_peer,ctx,base,actor) || actor.registry!=registry_ ||
           actor.generation!=binding.generation || actor.entity!=binding.entity || actor.controller!=binding.controller || actor.action_user!=binding.action_user)return OwnedFrameResult::StaleActor;
        const auto result=ValidateOwnedActorMovement(binding.native_peer,Action(movement,binding.action_user),ctx,base);
        if(result!=ActorInputResult::Applied)return OwnedFrameResult::NativeNotReady;
        auto commands=JumpCommands(binding.generation,frame.tick,movement,binding.action_user);
        if(buttons){std::vector<NativeActorAction> actions;
            if(!ButtonCommands(*binding.buttons,movement,binding.action_user,actions,commands,base))return OwnedFrameResult::InvalidMovement;
        }
        if(ValidateOwnedActorJumpBatch(binding.native_peer,std::span<const NativeActorJump>(commands.data(),movement.jump_count),ctx,base)!=ActorInputResult::Applied)
            return OwnedFrameResult::NativeNotReady;
    }
    if(retail_ && retail_admission_){
        RoomFrame retail;retail.tick=frame.tick;retail.players_mask=retail_;
        for(unsigned i=0;i<2;++i)if(retail_&(1u<<i))retail.players[i]=frame.players[i];
        if(retail_admission_(retail,ctx,base)!=ActorInputResult::Applied)return OwnedFrameResult::NativeNotReady;
    }
    return OwnedFrameResult::Ready;
}
OwnedFrameResult OwnedMovementFrames::Deliver(const RoomFrame& frame,PPCContext& ctx,uint8_t* base){
    Environment environment;const auto preflight=Preflight(frame,ctx,base);if(preflight!=OwnedFrameResult::Ready)return preflight;
    const auto extras=uint16_t(roster_&~retail_);
    if(extras && !BeginOwnedActorBasisTick(registry_,frame.tick,ctx,base)){faulted_=true;return OwnedFrameResult::NativeNotReady;}
    // Stage every actor before the first native action, so an ordinary validation
    // failure cannot deliver half a frame. Native callbacks can still fail or
    // mutate state; that is terminal, never a claim of transactional rollback.
    for(unsigned i=0;i<kOnlineRoomCapacity;++i)if(extras&(1u<<i)){
        const auto& binding=bindings_[i];
        if(PrepareOwnedActorBasis(binding.native_peer,Basis(binding.native_peer,binding.generation,frame.tick,frame.players[i]),ctx,base)!=ActorInputResult::Applied){
            faulted_=true;return OwnedFrameResult::NativeNotReady;
        }
    }
    try{
        for(unsigned i=0;i<kOnlineRoomCapacity;++i)if(extras&(1u<<i))
            if(ReplayOwnedActorMovement(bindings_[i].native_peer,Action(frame.players[i],bindings_[i].action_user),frame.tick,ctx,base)!=ActorInputResult::Applied){
                faulted_=true;return OwnedFrameResult::NativeFailure;
            }
        // Movement claims for ALL peers precede the first edge. The complete
        // edge batches were checked before any native delivery; an exception
        // after entering native code is terminal, never retried as a rollback.
        for(unsigned i=0;i<kOnlineRoomCapacity;++i)if(extras&(1u<<i)){
            const auto& binding=bindings_[i];const auto& movement=frame.players[i];
            auto commands=JumpCommands(binding.generation,frame.tick,movement,binding.action_user);
            if(movement.source!=RoomMovementSource::NativeActionV3){
                for(unsigned n=0;n<movement.jump_count;++n)if(ReplayOwnedActorJump(binding.native_peer,commands[n],ctx,base)!=ActorInputResult::Applied){faulted_=true;return OwnedFrameResult::NativeFailure;}
            }else{
                std::vector<NativeActorAction> actions;
                if(!ButtonCommands(*binding.buttons,movement,binding.action_user,actions,commands,base)){faulted_=true;return OwnedFrameResult::NativeNotReady;}
                unsigned jump=0;
                for(const auto& action:actions){
                    if(!SameWorld(registry_,base) || !ValidateNativeButtonDefinitionSnapshot(*binding.buttons,base)){faulted_=true;return OwnedFrameResult::StaleActor;}
                    ActorInputResult result;
                    const auto uid=PPC_LOAD_U32(action.action);
                    if(uid==PPC_LOAD_U32(0x833DAEBC+binding.action_user*12) || uid==PPC_LOAD_U32(0x833DAEC0+binding.action_user*12)){
                        result=ReplayOwnedActorJump(binding.native_peer,commands[jump++],ctx,base);
                    }else{
                        const ButtonPermit permit{binding.native_peer,binding.generation,frame.tick,action};
                        struct Scope {explicit Scope(const ButtonPermit& p){button_permit=&p;}~Scope(){button_permit=nullptr;}}scope(permit);
                        uint32_t native_result=0;
                        result=detail::DispatchOwnedActorButton(binding.native_peer,binding.generation,frame.tick,action,ctx,base,native_result);
                        if(REXCVAR_GET(em2_private_profile)){
                            Environment log_environment;
                            const auto definition=std::find_if(binding.buttons->actions.begin(),binding.buttons->actions.end(),[&](const auto& d){return d.action==action.action && d.event==action.event;});
                            if(definition!=binding.buttons->actions.end())REXLOG_INFO("Native Room owned button callback: tick {} peer {} profile {} action '{}' result {} native return {}; original derived handler",frame.tick,i+1,binding.action_user,definition->name,int(result),native_result);
                            if(definition!=binding.buttons->actions.end() && REXCVAR_GET(em2_probe_owned_button_tasks) && definition->name.find("Electric Stream")!=std::string::npos){
                                OwnedActorInput actor;
                                if(OwnedActorInputFor(binding.native_peer,ctx,base,actor))LogButtonTasks("extra",i+1,frame.tick,actor.listener,base);
                                LogButtonTasks("retail",i+1,frame.tick,binding.buttons->parent.listener,base);
                            }
                        }
                    }
                    if(result!=ActorInputResult::Applied){faulted_=true;return OwnedFrameResult::NativeFailure;}
                }
            }
        }
    }catch(...){faulted_=true;throw;}
    for(unsigned i=0;i<kOnlineRoomCapacity;++i)if(extras&(1u<<i))AdvanceRoomJumpStream(jumps_[i],frame.players[i]);
    delivered_=frame;return OwnedFrameResult::Ready;
}
OwnedFrameResult OwnedMovementFrames::Finish(uint64_t tick,uint8_t* base){
    Environment environment;
    if(!configured_)return OwnedFrameResult::NotConfigured;
    if(owner_thread_!=GetCurrentThreadId())return OwnedFrameResult::WrongThread;
    if(faulted_)return OwnedFrameResult::Faulted;
    if(!delivered_ || delivered_->tick!=tick || tick!=next_tick_)return OwnedFrameResult::IncompleteTick;
    if(!SameWorld(registry_,base)){faulted_=true;return OwnedFrameResult::StaleActor;}
    for(unsigned i=0;i<kOnlineRoomCapacity;++i)if((roster_&~retail_)&(1u<<i)){
        const auto& binding=bindings_[i];
        if(!OwnedActorBasisConsumed(binding.native_peer,binding.generation,tick))return OwnedFrameResult::IncompleteTick;
    }
    delivered_.reset();if(next_tick_==UINT64_MAX)exhausted_=true;else ++next_tick_;return OwnedFrameResult::Ready;
}
OwnedFrameResult OwnedMovementFrames::FinishMovieEntry(uint64_t tick,PPCContext& ctx,uint8_t* base,uint16_t& consumed_mask){
    Environment environment;uint64_t agreed=0;
    if(!configured_)return OwnedFrameResult::NotConfigured;
    if(owner_thread_!=GetCurrentThreadId())return OwnedFrameResult::WrongThread;
    if(faulted_)return OwnedFrameResult::Faulted;
    if(!delivered_ || delivered_->tick!=tick || tick!=next_tick_ || tick==UINT64_MAX ||
       !em2::CurrentAgreedSimulationTick(agreed) || agreed!=tick)return OwnedFrameResult::IncompleteTick;
    if(!SameWorld(registry_,base))return OwnedFrameResult::StaleActor;
    NativeGameOverState control;uint32_t mode=0;
    if(!ReadNativeControlState(ctx,base,control,mode) || mode!=2 || control.registry!=registry_ ||
       !ReadNativeCutsceneContext(ctx,base,registry_))return OwnedFrameResult::NativeNotReady;
    uint16_t mask=0;
    for(unsigned i=0;i<kOnlineRoomCapacity;++i)if((roster_&~retail_)&(1u<<i)){
        const auto& binding=bindings_[i];OwnedActorInput actor;bool consumed=false;
        if(!OwnedActorInputFor(binding.native_peer,ctx,base,actor) || !actor.enabled || actor.generation!=binding.generation ||
           actor.entity!=binding.entity || actor.controller!=binding.controller)return OwnedFrameResult::StaleActor;
        if(!OwnedActorBasisDeliveryState(binding.native_peer,binding.generation,tick,consumed))return OwnedFrameResult::IncompleteTick;
        if(consumed)mask|=uint16_t(1u<<i);
    }
    // Retire the coordinator transaction, retaining the original per-actor
    // consumption flags and quarantine. The next owned basis tick clears old
    // staging; no omitted Motion work is replayed or labelled consumed.
    delivered_.reset();++next_tick_;consumed_mask=mask;return OwnedFrameResult::Ready;
}
bool TestOwnedMovementFrames(PPCContext& ctx,uint8_t* base,uint64_t first_peer,uint64_t second_peer,OwnedFrameUpdate update){
    if(!update)return false;Environment environment;
    OwnedActorInput a,b;if(!OwnedActorInputFor(first_peer,ctx,base,a) || !OwnedActorInputFor(second_peer,ctx,base,b))return false;
    // Actual loopback room -> typed authoritative frame -> original native
    // action/controller path. The supplied sink stops before physical contacts.
    Identity identity;identity.build[0]=1;identity.content[0]=2;identity.checkpoint[0]=3;identity.session[0]=4;
    identity.first_tick=1000000;identity.input_delay=0;
    Room host,client;unsigned checks=0;auto check=[&](bool value,const char* why){++checks;if(!value)REXLOG_ERROR("Owned network frame: {}",why);return value;};
    if(!check(host.Host(identity,"127.0.0.1",0,2,"Host") && client.Join(identity,"127.0.0.1",host.port(),"Guest"),"real room connects"))return false;
    auto pump=[&](auto ready){const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        do{host.Poll();client.Poll();if(ready())return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(std::chrono::steady_clock::now()<until);
        REXLOG_ERROR("Owned room states {}/{} errors '{}'/'{}'",int(host.state()),int(client.state()),host.error(),client.error());return false;};
    if(!check(pump([&]{return host.lobby().players.size()==2 && client.lobby().players.size()==2;}) &&
        host.SetReady(true) && client.SetReady(true) && pump([&]{return !client.profile_pending() && host.lobby().players[1].ready;}) &&
        host.Start() && pump([&]{return host.state()==RoomState::Loading && client.state()==RoomState::Loading;}),"frozen native-room roster waits for native world admission"))return false;
    {
        const auto caller=ctx;const auto fp=_mm_getcsr();const auto error=GetLastError();uint32_t native_result=0xCDCDCDCD;
        if(!check(detail::DispatchOwnedActorButton(a.peer,a.generation,identity.first_tick,{0,0x833DAEB8,0,0},ctx,base,native_result)==ActorInputResult::InvalidContext &&
                  !native_result && !std::memcmp(&caller,&ctx,sizeof(ctx)) && _mm_getcsr()==fp && GetLastError()==error,
                  "direct generic button dispatch cannot obtain a coordinator permit or change caller state"))return false;
    }
    OwnedMovementFrames coordinator;std::array<uint64_t,kOnlineRoomCapacity> epochs{};epochs[0]=epochs[1]=1;
    if(!check(coordinator.Configure(3,0,epochs,identity.first_tick,a.registry) && coordinator.Bind(1,a.peer,ctx,base) &&
        coordinator.Bind(2,b.peer,ctx,base) && !coordinator.Bind(3,a.peer,ctx,base),"room IDs bind distinct native actors without retail slot fabrication"))return false;
    RoomFrame premature;
    if(!check(!host.TakeFrame(premature) && !client.TakeFrame(premature),"loading cannot expose a frame before native actors are admitted"))return false;
    // This controlled fixture already owns both admitted actors. Production
    // readiness must come from its actual load/admission transaction instead.
    if(!check(host.WorldReady(identity.checkpoint) && client.WorldReady(identity.checkpoint) &&
        pump([&]{return host.state()==RoomState::Running && client.state()==RoomState::Running;}),"matching native world readiness activates frame dispatch"))return false;
    for(unsigned n=0;n<3;++n){
        RoomMovement first,second;first.source=second.source=RoomMovementSource::NativeActionV1;first.actor_epoch=second.actor_epoch=1;
        first.x=n==2?0:.5f;first.y=n==2?0:-.25f;second.x=n==2?0:-.75f;second.y=n==2?0:.125f;
        second.forward_x=1;second.forward_z=0;
        const auto tick=identity.first_tick+n;
        RoomFrame authoritative,received;bool taken=false,got=false;
        if(!check(host.SubmitLocal(tick,first) && !host.TakeFrame(authoritative) && client.SubmitLocal(tick,second) &&
            pump([&]{if(!taken)taken=host.TakeFrame(authoritative);if(!got)got=client.PeekFrame(received);return taken && got;}) &&
            authoritative==received && authoritative.players_mask==3,"both peers agree on exact semantic commands and per-peer view bases"))return false;
        const auto incoming=ctx;std::array<std::array<uint8_t,800>,2> before;
        std::memcpy(before[0].data(),base+a.controller,800);std::memcpy(before[1].data(),base+b.controller,800);
        auto bad=received;bad.players_mask=1;
        if(!check(coordinator.Preflight(bad,ctx,base)==OwnedFrameResult::InvalidRoster,"missing peer cannot become implicit neutral"))return false;
        bad=received;bad.players[1].actor_epoch=2;
        if(!check(coordinator.Preflight(bad,ctx,base)==OwnedFrameResult::EpochMismatch,"old actor incarnation cannot receive a new frame"))return false;
        bad=received;bad.players[1].source=RoomMovementSource::UnconvertedPad;
        if(!check(coordinator.Preflight(bad,ctx,base)==OwnedFrameResult::InvalidMovement,"raw pad is not native interpreted locomotion"))return false;
        bad=received;bad.players[1].pad.buttons=0x1000;
        if(!check(coordinator.Preflight(bad,ctx,base)==OwnedFrameResult::UnsupportedIntent &&
            !std::memcmp(before[0].data(),base+a.controller,800) && !std::memcmp(before[1].data(),base+b.controller,800) &&
            !std::memcmp(&incoming,&ctx,sizeof(ctx)),"all-peer preflight rejects unsupported intent before native effects"))return false;
        // A real native load/pause gate can close after receiving a frame.
        // Admission must leave that same command available instead of turning
        // a temporary native wait into dropped input or implicit neutral.
        const auto manager=PPC_LOAD_U32(0x833DAB60);const auto ready=PPC_LOAD_U8(manager+357);
        PPC_STORE_U8(manager+357,0);
        const auto waiting=coordinator.Preflight(received,ctx,base);
        PPC_STORE_U8(manager+357,ready);
        RoomFrame retained;
        if(!check(waiting==OwnedFrameResult::NativeNotReady && client.PeekFrame(retained) && retained==received &&
            client.next_tick()==tick && coordinator.next_tick()==tick,"native admission wait retains the exact transport frame and both timelines"))return false;
        RoomFrame committed;
        if(!check(coordinator.Preflight(retained,ctx,base)==OwnedFrameResult::Ready && client.TakeFrame(committed) && committed==retained &&
            client.next_tick()==tick+1,"transport consumption follows successful all-actor preflight"))return false;
        if(!check(coordinator.Deliver(committed,ctx,base)==OwnedFrameResult::Ready &&
            coordinator.Deliver(received,ctx,base)==OwnedFrameResult::IncompleteTick,"one authoritative frame delivers once before world update"))return false;
        if(!check(coordinator.Finish(tick,base)==OwnedFrameResult::IncompleteTick && coordinator.next_tick()==tick,
            "delivery without native controller consumption cannot complete or advance the network tick"))return false;
        {auto call=ctx;call.r3.u64=a.controller+24;update(call,base);}
        if(!check(coordinator.Finish(tick,base)==OwnedFrameResult::IncompleteTick && coordinator.next_tick()==tick,
            "one processed extra cannot acknowledge a tick while another actor still has pending native motion"))return false;
        {auto call=ctx;call.r3.u64=b.controller+24;update(call,base);}
        if(!check(OwnedActorBasisConsumed(a.peer,a.generation,tick) && OwnedActorBasisConsumed(b.peer,b.generation,tick) &&
            !OwnedActorBasisConsumed(a.peer,a.generation+1,tick) && !OwnedActorBasisConsumed(a.peer,a.generation,tick+1),
            "completed native controller updates expose only their exact actor generation and staged tick consumption"))return false;
        auto sample=[&](uint32_t address){return std::bit_cast<float>(PPC_LOAD_U32(address));};
        if(sample(a.listener+168)!=first.y || sample(a.listener+172)!=first.x || sample(b.listener+168)!=second.y || sample(b.listener+172)!=second.x)
            REXLOG_ERROR("Owned movement profile diagnostic: tick {}, users {}/{}, first cache {}/{}, expected {}/{}, second cache {}/{}, expected {}/{}",tick,a.action_user,b.action_user,sample(a.listener+168),sample(a.listener+172),first.y,first.x,sample(b.listener+168),sample(b.listener+172),second.y,second.x);
        if(!check(sample(a.listener+168)==first.y && sample(a.listener+172)==first.x &&
            sample(b.listener+168)==second.y && sample(b.listener+172)==second.x,"network movement reaches only its owned native action cache, including neutral release"))return false;
        auto motion_for=[&](const OwnedActorInput& actor){
            const auto types=PPC_LOAD_U32(actor.entity+88),list=PPC_LOAD_U32(actor.entity+36);
            const auto slot=PPC_LOAD_U8(types+PPC_LOAD_U8(0x833DAF54));return PPC_LOAD_U32(list+unsigned(slot)*4)-8;
        };
        const auto ma=motion_for(a),mb=motion_for(b);
        if(!check(sample(ma+176)==first.forward_x && sample(ma+180)==0 && sample(ma+184)==first.forward_z &&
            sample(mb+176)==second.forward_x && sample(mb+180)==0 && sample(mb+184)==second.forward_z,
            "real native motion uses the view received for each network peer, not the shared reference camera"))return false;
        if(!check(coordinator.Finish(tick,base)==OwnedFrameResult::Ready && coordinator.next_tick()==tick+1 &&
            coordinator.Finish(tick,base)==OwnedFrameResult::IncompleteTick,"native tick completion advances exactly once"))return false;
        std::array<uint8_t,32> fixture_digest{};fixture_digest[0]=0xA1;fixture_digest[1]=uint8_t(n+1);
        if(!check(host.CompleteGameplay(tick,fixture_digest) && client.CompleteGameplay(tick,fixture_digest) &&
            pump([&]{return !host.gameplay_completion_pending() && !client.gameplay_completion_pending() &&
                host.verified_gameplay_tick()==tick && client.verified_gameplay_tick()==tick;}),
            "fixture native callback processing is explicitly acknowledged before another transport frame"))return false;
    }
    REXLOG_INFO("Owned network frame passed {} checks: actual TCP room/authoritative frames into original native action/controller callbacks, distinct peers/views, preflight failure isolation, explicit neutral, epochs and tick ownership; fixture motion sink excludes contacts/full multiplayer",checks);
    return true;
}
bool TestOwnedJumpMovementFrames(PPCContext& ctx,uint8_t* base,uint64_t first_peer,uint64_t second_peer,OwnedFrameUpdate update){
    if(!update)return false;
    Environment environment;OwnedActorInput a,b;
    if(!OwnedActorInputFor(first_peer,ctx,base,a) || !OwnedActorInputFor(second_peer,ctx,base,b))return false;
    Identity identity;identity.build[0]=11;identity.content[0]=12;identity.checkpoint[0]=13;identity.session[0]=14;
    identity.first_tick=3000000;identity.input_delay=0;
    Room host,client;unsigned checks=0;auto check=[&](bool value,const char* why){++checks;if(!value)REXLOG_ERROR("Owned Jump network frame: {}",why);return value;};
    auto pump=[&](auto ready){const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        do{host.Poll();client.Poll();if(ready())return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(std::chrono::steady_clock::now()<until);return false;};
    if(!check(host.Host(identity,"127.0.0.1",0,2,"JumpHost") && client.Join(identity,"127.0.0.1",host.port(),"JumpGuest") &&
        pump([&]{return host.lobby().players.size()==2 && client.lobby().players.size()==2;}) && host.SetReady(true) && client.SetReady(true) &&
        pump([&]{return !client.profile_pending() && host.lobby().players[1].ready;}) && host.Start() &&
        pump([&]{return host.state()==RoomState::Loading && client.state()==RoomState::Loading;}) &&
        host.WorldReady(identity.checkpoint) && client.WorldReady(identity.checkpoint) &&
        pump([&]{return host.state()==RoomState::Running && client.state()==RoomState::Running;}),"actual TCP room admits matching native fixture world"))return false;
    OwnedMovementFrames coordinator;std::array<uint64_t,kOnlineRoomCapacity> epochs{};epochs[0]=epochs[1]=1;
    if(!coordinator.Configure(3,0,epochs,identity.first_tick,a.registry) || !coordinator.Bind(1,a.peer,ctx,base) || !coordinator.Bind(2,b.peer,ctx,base))return false;
    for(unsigned n=0;n<4;++n){
        RoomMovement first,second;first.source=second.source=RoomMovementSource::NativeActionV2;
        first.actor_epoch=second.actor_epoch=1;first.jump_button_mask=second.jump_button_mask=0x1000;
        first.x=.5f;second.x=-.5f;second.forward_x=1;second.forward_z=0;
        if(n==0){first.jump_count=1;first.jump_events[0]={1,RoomJumpEdge::Pressed};}
        if(n==1){second.jump_count=1;second.jump_events[0]={1,RoomJumpEdge::Pressed};}
        if(n==2){first.jump_count=2;first.jump_events[0]={2,RoomJumpEdge::Released};first.jump_events[1]={3,RoomJumpEdge::Pressed};
            second.jump_count=1;second.jump_events[0]={2,RoomJumpEdge::Released};}
        if(n==3){first.jump_count=1;first.jump_events[0]={4,RoomJumpEdge::Released};}
        first.pad.buttons=n<3?0x1000:0;second.pad.buttons=n==1?0x1000:0;
        const auto tick=identity.first_tick+n;RoomFrame authoritative,received;bool taken=false,got=false;
        if(!check(host.SubmitLocal(tick,first) && client.SubmitLocal(tick,second) &&
            pump([&]{if(!taken)taken=host.TakeFrame(authoritative);if(!got)got=client.PeekFrame(received);return taken && got;}) && authoritative==received,
            "explicit native edges and held no-event ticks survive actual TCP authoritative echo"))return false;
        std::array<std::array<uint8_t,800>,2> before;std::memcpy(before[0].data(),base+a.controller,800);std::memcpy(before[1].data(),base+b.controller,800);
        auto unchanged=[&]{return !std::memcmp(before[0].data(),base+a.controller,800) && !std::memcmp(before[1].data(),base+b.controller,800);};
        auto bad=received;bad.players[1].pad.buttons|=0x2000;
        if(n==0){auto buttons=received;buttons.players[1].source=RoomMovementSource::NativeActionV3;buttons.players[1].button_actions.schema[0]=1;
            if(!check(coordinator.Deliver(buttons,ctx,base)==OwnedFrameResult::UnsupportedIntent && unchanged(),"V3 button decisions never get discarded by the Movement/Jump-only extra adapter"))return false;}
        if(!check(coordinator.Deliver(bad,ctx,base)==OwnedFrameResult::UnsupportedIntent && unchanged(),"unsupported extra controls reject before either actor receives Movement"))return false;
        if(n==0){
            OwnedMovementFrames retail_adapter;
            if(!check(retail_adapter.Configure(3,1,epochs,tick,a.registry) && retail_adapter.Bind(2,b.peer,ctx,base) &&
                retail_adapter.Preflight(received,ctx,base)==OwnedFrameResult::UnsupportedIntent && unchanged(),
                "retail raw-pad adapter cannot silently discard V2 interpreted edges"))return false;
            bad=received;bad.players[1].jump_count=2;bad.players[1].jump_events[0]={1,RoomJumpEdge::Pressed};bad.players[1].jump_events[1]={3,RoomJumpEdge::Released};
            if(!check(coordinator.Deliver(bad,ctx,base)==OwnedFrameResult::InvalidMovement && unchanged(),"invalid later edge rejects whole frame without first-peer native effects"))return false;
            bad=received;bad.players[1].actor_epoch=8;
            if(!check(coordinator.Deliver(bad,ctx,base)==OwnedFrameResult::EpochMismatch && unchanged(),"wire actor incarnation cannot retarget current native generation"))return false;
        }
        if(n==1){
            const auto kinds=PPC_LOAD_U32(b.entity+88),list=PPC_LOAD_U32(b.entity+36);
            const auto slot=PPC_LOAD_U8(kinds+PPC_LOAD_U8(0x833DDF94));const auto body=PPC_LOAD_U32(list+unsigned(slot)*4)-8;
            const auto owner=PPC_LOAD_U32(body+48);PPC_STORE_U32(body+48,a.entity-8);
            const auto rejected=coordinator.Deliver(received,ctx,base);PPC_STORE_U32(body+48,owner);
            if(!check(rejected==OwnedFrameResult::NativeNotReady && unchanged(),"stale physics ownership preflight rejects all peers before delivery"))return false;
        }
        RoomFrame committed;
        if(!check(coordinator.Preflight(received,ctx,base)==OwnedFrameResult::Ready && client.TakeFrame(committed) && committed==received &&
            coordinator.Deliver(committed,ctx,base)==OwnedFrameResult::Ready && coordinator.Deliver(committed,ctx,base)==OwnedFrameResult::IncompleteTick,
            "one agreed frame delivers Movement for all actors before ordered original Jump callbacks"))return false;
        if(!check(coordinator.Finish(tick,base)==OwnedFrameResult::IncompleteTick && coordinator.next_tick()==tick,
            "Jump stimulus delivery alone does not prove the native motion stage ran"))return false;
        {uint16_t receipt=0xA55A;bool consumed=true;
            if(!check(OwnedActorBasisDeliveryState(a.peer,a.generation,tick,consumed) && !consumed &&
                coordinator.FinishMovieEntry(tick,ctx,base,receipt)!=OwnedFrameResult::Ready && receipt==0xA55A && coordinator.next_tick()==tick,
                "delivered but unconsumed extra input cannot close without an agreed native movie transition"))return false;}
        {auto call=ctx;call.r3.u64=a.controller+24;update(call,base);}
        if(!check(coordinator.Finish(tick,base)==OwnedFrameResult::IncompleteTick,
            "Jump frame waits for both native actor updates"))return false;
        {auto call=ctx;call.r3.u64=b.controller+24;update(call,base);}
        if(!check(coordinator.Finish(tick,base)==OwnedFrameResult::Ready && coordinator.next_tick()==tick+1,
            "Jump frame completes only after every native movement stage consumed its exact tick"))return false;
        std::array<uint8_t,32> fixture_digest{};fixture_digest[0]=0xA2;fixture_digest[1]=uint8_t(n+1);
        if(!check(host.CompleteGameplay(tick,fixture_digest) && client.CompleteGameplay(tick,fixture_digest) &&
            pump([&]{return !host.gameplay_completion_pending() && !client.gameplay_completion_pending() &&
                host.verified_gameplay_tick()==tick && client.verified_gameplay_tick()==tick;}),
            "fixture original Jump-task processing carries explicit completion proof before next frame"))return false;
    }
    REXLOG_INFO("Owned Jump network frame passed {} actual TCP/native delivery checks; native fixture observes Jump stimuli separately, no contact trajectory claim",checks);
    return true;
}

}

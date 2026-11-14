#include "online_owned_probe.h"
#include "online_actor_input.h"
#include "online_actor_jump.h"
#include "online_runtime.h"
#include "native_character_physics.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(std::string,em2_user_data_directory);
REXCVAR_DEFINE_BOOL(em2_probe_owned_native_player_input,false,"Debug","Private fixed-tick owned actor Movement/Jump program and native trajectory observation; no network or hardware input");
namespace em2::net {
namespace {
struct Environment{unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(csr);SetLastError(error);}};
struct State {
    OwnedActorInput actor;
    uint64_t clock=0;
    unsigned index=0,warmup=0,samples=0,movement_bases=0,forward_bases=0,back_bases=0;
    uint32_t primary_entity=0;
    unsigned jump_stimuli=0;
    bool active=false,failed=false,completed=false,pending=false,jump_sent=false,released=false,delivered=false;
    std::array<float,3> origin{};
    std::array<float,3> movement_start{},forward_end{},back_end{};
    float jump_origin=0;
    float max_horizontal=0,max_height=0,max_up_speed=0,last_horizontal_speed=0;
    bool airborne=false;
};
thread_local State state;
bool Fail(const char* why,int result=-1){
    state.failed=true;state.pending=false;
    SetOwnedActorInputEnabled(state.actor.peer,false);
    REXLOG_ERROR("Owned actor live input failed: peer {} tick {} sample {} reason '{}' result {}",state.actor.peer,state.clock,state.index,why,result);
    return false;
}
NativeActorJump Jump(uint64_t sequence,OwnedJumpEdge edge){return {state.actor.generation,state.clock,sequence,edge,
    {0,(edge==OwnedJumpEdge::Pressed?0x833DAEBCu:0x833DAEC0u)+state.actor.action_user*12,1,0}};}
}
bool ArmOwnedActorProbe(uint64_t peer,uint32_t primary_entity,PPCContext& ctx,uint8_t* base){
    Environment preserve;
    if(!REXCVAR_GET(em2_probe_owned_native_player_input) || !REXCVAR_GET(em2_private_profile) ||
       REXCVAR_GET(em2_user_data_directory).empty() || SynchronizedSessionActive() || state.active || state.actor.peer)return false;
    OwnedActorInput actor;if(!OwnedActorInputFor(peer,ctx,base,actor) || !actor.enabled)return false;
    state={};state.actor=actor;state.primary_entity=primary_entity;state.active=true;
    REXLOG_INFO("Owned actor live input armed: peer {} generation {} action user {} entity {:08X}; fixed-tick canonical actions only",peer,actor.generation,actor.action_user,actor.entity);
    return true;
}
bool BeforeOwnedActorProbeTick(PPCContext& ctx,uint8_t* base){
    if(!state.active)return true;
    Environment preserve;
    if(state.failed)return false;
    if(state.pending)return Fail("previous native update has not completed");
    if(SynchronizedSessionActive())return Fail("private input program cannot enter a synchronized session");
    OwnedActorInput actor;
    if(!OwnedActorInputFor(state.actor.peer,ctx,base,actor) || actor.generation!=state.actor.generation ||
       actor.entity!=state.actor.entity || actor.action_user!=state.actor.action_user)return Fail("actor incarnation changed");
    ++state.clock;
    NativeActorAction movement{0,0x833DAEB8+actor.action_user*12,0,0};
    if(state.index>=30 && state.index<60)movement.y=.15f;
    if(state.index>=60 && state.index<90)movement.y=-.15f;
    const auto admission=ValidateOwnedActorMovement(actor.peer,movement,ctx,base);
    const auto jump_admission=state.index==0?ValidateOwnedActorJump(actor.peer,Jump(1,OwnedJumpEdge::Pressed),ctx,base):ActorInputResult::Applied;
    if(admission!=ActorInputResult::Applied || jump_admission!=ActorInputResult::Applied){
        if(state.index==0 && ++state.warmup<=120){
            if(state.warmup==1 || state.warmup%30==0)REXLOG_INFO("Owned actor live input waiting: Movement {} Jump {} native updates {}",int(admission),int(jump_admission),state.warmup);
            return true;
        }
        return Fail("native input targets not ready",int(admission!=ActorInputResult::Applied?admission:jump_admission));
    }
    if(!BeginOwnedActorBasisTick(actor.registry,state.clock,ctx,base))return Fail("could not begin owned input tick");
    ActorLookBasis basis{actor.peer,actor.generation,state.clock};
    auto result=PrepareOwnedActorBasis(actor.peer,basis,ctx,base);
    if(result!=ActorInputResult::Applied)return Fail("native movement reference not ready",int(result));
    result=ReplayOwnedActorMovement(actor.peer,movement,state.clock,ctx,base);
    if(result!=ActorInputResult::Applied)return Fail("native Movement delivery rejected",int(result));
    if(!state.delivered){state.delivered=true;REXLOG_INFO("Owned actor live input delivery started: peer {} tick {}",actor.peer,state.clock);}
    if(state.index==90 || state.index==94){
        const auto command=Jump(state.index==90?1:2,state.index==90?OwnedJumpEdge::Pressed:OwnedJumpEdge::Released);
        result=ReplayOwnedActorJump(actor.peer,command,ctx,base);
        if(result!=ActorInputResult::Applied)return Fail("native Jump delivery rejected",int(result));
        if(state.index==90)state.jump_sent=true;else state.released=true;
    }
    state.pending=true;return true;
}
void AfterOwnedActorProbeTick(PPCContext& ctx,uint8_t* base){
    if(!state.active || !state.pending || state.failed)return;
    Environment preserve;state.pending=false;
    OwnedActorInput actor;GameplayObservation::CharacterPhysics sample;
    if(!OwnedActorInputFor(state.actor.peer,ctx,base,actor) || actor.generation!=state.actor.generation ||
       !em2::ReadNativeActorPhysics(base,actor.entity,sample)){Fail("native character physics observation unavailable");return;}
    if(!sample.active){if(state.index==0 && ++state.warmup<=120)return;Fail("owned physics proxy is inactive");return;}
    std::array<float,3> position{},velocity{};
    for(unsigned i=0;i<3;++i){position[i]=std::bit_cast<float>(sample.position[i]);velocity[i]=std::bit_cast<float>(sample.velocity[i]);}
    if(!state.samples)state.origin=position;
    ++state.samples;
    const bool consumed=OwnedActorBasisConsumed(actor.peer,actor.generation,state.clock);
    if(state.index>=30 && state.index<90 && consumed)++state.movement_bases;
    if(state.index>=30 && state.index<60 && consumed)++state.forward_bases;
    if(state.index>=60 && state.index<90 && consumed)++state.back_bases;
    if(state.index==29)state.movement_start=position;
    if(state.index==59)state.forward_end=position;
    if(state.index==89){state.back_end=position;state.jump_origin=position[1];}
    const auto x=position[0]-state.origin[0],z=position[2]-state.origin[2];
    state.max_horizontal=std::max(state.max_horizontal,std::sqrt(x*x+z*z));
    state.last_horizontal_speed=std::sqrt(velocity[0]*velocity[0]+velocity[2]*velocity[2]);
    if(state.index>=90){state.max_height=std::max(state.max_height,position[1]-state.jump_origin);
        if(state.index<105)state.max_up_speed=std::max(state.max_up_speed,velocity[1]);state.airborne|=sample.state!=0;}
    if(state.index<180 && (state.index%15==0 || state.index==94 || state.index==179))
        REXLOG_INFO("Owned actor live trajectory: peer {} sample {} tick {} position {}/{}/{} velocity {}/{}/{} native state {} staged basis consumed {}",actor.peer,state.index,state.clock,position[0],position[1],position[2],velocity[0],velocity[1],velocity[2],sample.state,consumed);
    if(state.index==179){
        const float fx=state.forward_end[0]-state.movement_start[0],fz=state.forward_end[2]-state.movement_start[2];
        const float bx=state.back_end[0]-state.forward_end[0],bz=state.back_end[2]-state.forward_end[2];
        const float forward=std::sqrt(fx*fx+fz*fz),back=std::sqrt(bx*bx+bz*bz),direction_dot=fx*bx+fz*bz;
        REXLOG_INFO("Owned actor live phase proof: forward {} back {} direction dot {} forward bases {} back bases {} landing height {} native state {}",forward,back,direction_dot,state.forward_bases,state.back_bases,position[1]-state.jump_origin,sample.state);
        REXLOG_INFO("Owned actor live jump proof: physics stimuli {} height {} initial upward speed {} airborne {}",state.jump_stimuli,state.max_height,state.max_up_speed,state.airborne);
        state.completed=state.jump_stimuli && std::isfinite(forward) && std::isfinite(back) && std::isfinite(direction_dot) &&
            std::isfinite(state.max_height) && std::isfinite(state.max_up_speed) &&
            forward>.05f && back>.05f && direction_dot<0 && state.forward_bases && state.back_bases &&
            state.jump_sent && state.released && state.airborne && state.max_height>.15f && state.max_up_speed>1.f &&
            sample.state==0 && std::abs(position[1]-state.jump_origin)<.3f && state.last_horizontal_speed<.1f;
        if(!state.completed){Fail("native movement/jump/neutral trajectory thresholds not met");return;}
        REXLOG_INFO("Owned actor live input completed: success true peer {} action user {} samples {} movement bases {} horizontal {} jump height {} upward speed {} final horizontal speed {}; native controller/tasks/physics, no network or physical-device claim",actor.peer,actor.action_user,state.samples,state.movement_bases,state.max_horizontal,state.max_height,state.max_up_speed,state.last_horizontal_speed);
    }
    // Continue delivering native neutral until the lifecycle owner disables
    // input and removes the actor; never leave an old movement cache active.
    if(state.index<180)++state.index;
}
bool OwnedActorProbeFailed(uint64_t peer){return state.actor.peer==peer && state.failed;}
bool StopOwnedActorProbe(uint64_t peer){
    if(state.actor.peer!=peer)return false;
    state.active=false;return state.completed && !state.failed && !state.pending;
}
bool RetireOwnedActorProbe(uint64_t peer,PPCContext& ctx,uint8_t* base){
    Environment preserve;
    if(state.actor.peer!=peer || state.active)return false;
    const bool retired=!state.jump_sent || ForgetOwnedActorJump(peer,state.actor.generation,ctx,base);
    REXLOG_INFO("Owned actor live input retired: peer {} native Jump history removed {}",peer,retired);
    state={};return retired;
}
void ObserveOwnedActorProbeJumpTargets(uint64_t peer,uint32_t task,uint32_t motion,uint32_t physics,uint8_t* base){
    if(!state.active || state.actor.peer!=peer)return;
    Environment preserve;
    uint32_t own=UINT32_MAX,primary=UINT32_MAX;
    if(state.index==90){LogNativeActorTaskInventory(state.actor.entity,base);LogNativeActorTaskInventory(state.primary_entity,base);}
    const bool own_read=ReadNativeJumpTaskFlags(state.actor.entity,base,own),primary_read=ReadNativeJumpTaskFlags(state.primary_entity,base,primary);
    REXLOG_INFO("Owned actor live Jump targets: sample {} task {:08X} motion {:08X} physics {:08X} task flags {:08X} readable {} primary flags {:08X} readable {} motion state {} mode {} speed bits {:08X} task bytes {}/{}/{}",state.index,task,motion,physics,own,own_read,primary,primary_read,PPC_LOAD_U32(motion+384),PPC_LOAD_U32(motion+208),PPC_LOAD_U32(motion+876),PPC_LOAD_U8(task+16),PPC_LOAD_U8(task+17),PPC_LOAD_U8(task+18));
}
void ObserveOwnedActorProbeJumpStimulus(PPCContext& ctx,uint8_t* base){
    if(!state.active || state.failed || (uint32_t(ctx.lr)!=0x8287E7C0 && uint32_t(ctx.lr)!=0x8287E814))return;
    Environment preserve;em2::GuestReadBatch reads;
    const auto target=ctx.r3.u32;
    if(target<4096 || uint64_t(target)+52>0x100000000ull || !em2::GuestReadBatch::Readable(base+target,52) || PPC_LOAD_U32(target+48)!=state.actor.entity-8)return;
    ++state.jump_stimuli;
    REXLOG_INFO("Owned actor live Jump stimulus: sample {} tick {} target {:08X} native f1 {}",state.index,state.clock,target,ctx.f1.f64);
}
}

#include "online_actor_frames.h"
#include "online_room.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <windows.h>
#include <immintrin.h>
#include <cmath>
#include <bit>
#include <cstring>
#include <thread>
#include <rex/logging.h>
namespace em2::net {
namespace {
struct Environment {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(csr);SetLastError(error);}};
bool SameWorld(uint32_t registry,uint8_t* base){return base && GuestReadBatch::Readable(base+0x833DA7E4,4) && PPC_LOAD_U32(0x833DA7E4)==registry;}
ActorLookBasis Basis(uint64_t peer,uint64_t generation,uint64_t tick,const RoomMovement& movement){
    return {peer,generation,tick,{movement.forward_z,0,-movement.forward_x},{movement.forward_x,0,movement.forward_z}};
}
NativeActorAction Action(const RoomMovement& movement){return {0,0x833DAEB8,movement.x,movement.y};}
}
bool OwnedMovementFrames::Configure(uint16_t roster,uint16_t retail,const std::array<uint64_t,kOnlineRoomCapacity>& epochs,uint64_t first,uint32_t registry){
    if(configured_ || !registry || !(roster&1) || (retail&~uint16_t(3)) || (retail&~roster))return false;
    for(unsigned i=0;i<kOnlineRoomCapacity;++i)if(bool(roster&(1u<<i))!=(epochs[i]!=0))return false;
    roster_=roster;retail_=retail;epochs_=epochs;next_tick_=first;registry_=registry;
    owner_thread_=GetCurrentThreadId();configured_=true;return true;
}
bool OwnedMovementFrames::Bind(uint8_t peer,uint64_t native_peer,PPCContext& ctx,uint8_t* base){
    Environment environment;
    if(!configured_ || owner_thread_!=GetCurrentThreadId() || faulted_ || delivered_ || !peer || peer>kOnlineRoomCapacity)return false;
    const unsigned index=peer-1;
    if(!(roster_&(1u<<index)) || (retail_&(1u<<index)) || bindings_[index].native_peer)return false;
    OwnedActorInput actor;if(!OwnedActorInputFor(native_peer,ctx,base,actor) || actor.registry!=registry_ || !actor.enabled)return false;
    for(const auto& binding:bindings_)if(binding.native_peer==native_peer || binding.entity==actor.entity)return false;
    bindings_[index]={native_peer,actor.generation,actor.entity,actor.controller};return true;
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
        if(movement.actor_epoch!=epochs_[i])return OwnedFrameResult::EpochMismatch;
        if(!ValidRoomMovement(movement))return OwnedFrameResult::InvalidMovement;
        if(retail_&(1u<<i))continue;
        // Extra actors currently support proved native locomotion only.
        // Never silently discard jump/brush/tool intent and claim full controls.
        if(movement.pad.buttons || movement.pad.left_trigger || movement.pad.right_trigger)return OwnedFrameResult::UnsupportedIntent;
        const auto& binding=bindings_[i];if(!binding.native_peer)return OwnedFrameResult::UnboundActor;
        OwnedActorInput actor;
        if(!OwnedActorInputFor(binding.native_peer,ctx,base,actor) || actor.registry!=registry_ ||
           actor.generation!=binding.generation || actor.entity!=binding.entity || actor.controller!=binding.controller)return OwnedFrameResult::StaleActor;
        const auto result=ValidateOwnedActorMovement(binding.native_peer,Action(movement),ctx,base);
        if(result!=ActorInputResult::Applied)return OwnedFrameResult::NativeNotReady;
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
            if(ReplayOwnedActorMovement(bindings_[i].native_peer,Action(frame.players[i]),frame.tick,ctx,base)!=ActorInputResult::Applied){
                faulted_=true;return OwnedFrameResult::NativeFailure;
            }
    }catch(...){faulted_=true;throw;}
    delivered_=frame;return OwnedFrameResult::Ready;
}
OwnedFrameResult OwnedMovementFrames::Finish(uint64_t tick,uint8_t* base){
    Environment environment;
    if(!configured_)return OwnedFrameResult::NotConfigured;
    if(owner_thread_!=GetCurrentThreadId())return OwnedFrameResult::WrongThread;
    if(faulted_)return OwnedFrameResult::Faulted;
    if(!delivered_ || delivered_->tick!=tick || tick!=next_tick_)return OwnedFrameResult::IncompleteTick;
    if(!SameWorld(registry_,base)){faulted_=true;return OwnedFrameResult::StaleActor;}
    delivered_.reset();if(next_tick_==UINT64_MAX)exhausted_=true;else ++next_tick_;return OwnedFrameResult::Ready;
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
        for(const auto actor:{a,b}){auto call=ctx;call.r3.u64=actor.controller+24;update(call,base);}
        auto sample=[&](uint32_t address){return std::bit_cast<float>(PPC_LOAD_U32(address));};
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
    }
    REXLOG_INFO("Owned network frame passed {} checks: actual TCP room/authoritative frames into original native action/controller callbacks, distinct peers/views, preflight failure isolation, explicit neutral, epochs and tick ownership; fixture motion sink excludes contacts/full multiplayer",checks);
    return true;
}
}

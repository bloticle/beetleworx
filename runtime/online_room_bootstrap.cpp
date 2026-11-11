#include "online_room_bootstrap.h"
#include "online_runtime.h"
#include "controls.h"
#include "native_random.h"
#include "players.h"
#include "simulation.h"
#include "game_console.h"
#include <windows.h>
#include <immintrin.h>
#include <rex/logging.h>
#include <algorithm>
#include <cstring>
#include <thread>

namespace em2::net {
namespace {
std::atomic<bool> bootstrap_active{false};
std::atomic<bool> bootstrap_halted{false};
thread_local RoomBootstrapGate* bootstrap_on_thread=nullptr;
struct Environment {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(csr);SetLastError(error);}};
uint16_t Mask(const LobbySnapshot& lobby){uint16_t result=0;for(const auto& player:lobby.players){
    if(!player.peer || player.peer>kOnlineRoomCapacity)return 0;result|=uint16_t(1u<<(player.peer-1));}return result;}
bool CanonicalPads(const RoomBootstrapFrame& frame){
    for(unsigned i=0;i<frame.players.size();++i){Pad expected;
        if(i==1 && frame.players[i].buttons==0x10)expected.buttons=0x10;
        if(frame.players[i]!=expected)return false;
    }
    return true;
}
}
bool RoomBootstrapActive()noexcept{return bootstrap_active.load(std::memory_order_acquire);}
bool RoomBootstrapHalted()noexcept{return bootstrap_halted.load(std::memory_order_acquire);}
RoomBootstrapGate* CurrentRoomBootstrapGate()noexcept{return RoomBootstrapActive()?bootstrap_on_thread:nullptr;}
bool RoomBootstrapGate::OnOwnerThread()const{return thread_==GetCurrentThreadId();}
bool RoomBootstrapGate::Stop(std::string error){
    stopped_=true;bootstrap_halted.store(true,std::memory_order_release);error_=std::move(error);prepared_.reset();
    if(room_)room_->Disconnect();
    // The last native tick still owns its published frame until explicit
    // shutdown. Neither a disconnect nor failed admission releases controls.
    REXLOG_ERROR("Native room initialization stopped: {}",error_);return false;
}
bool RoomBootstrapGate::Attach(Room& room){
    Environment environment;
    if(active() || LegacyNativeSessionActive() || RoomBootstrapActive() ||
       room.state()!=RoomState::Bootstrapping || !room.native_bootstrap() ||
       room.identity().initialization_policy!=InitializationPolicy::SharedNativeV1 ||
       !room.lobby().locked || !room.local_peer() || room.local_peer()>kOnlineRoomCapacity)return false;
    const auto mask=Mask(room.lobby());if(!(mask&1) || !(mask&2) || !(mask&(1u<<(room.local_peer()-1))))return false;
    if(!em2::BeginSynchronizedControls(room.bootstrap_tick()))return false;
    room_=&room;identity_=room.identity();thread_=GetCurrentThreadId();roster_=mask;generation_=em2::SynchronizedControlsGeneration();
    if(!generation_){em2::EndSynchronizedControls();room_=nullptr;return false;}
    committed_tick_=0;completed_.reset();prepared_.reset();submitted_.reset();joining_=true;
    completion_pending_=stopped_=sampling_rebased_=false;readiness_=nullptr;error_.clear();
    bootstrap_on_thread=this;bootstrap_halted.store(false,std::memory_order_release);
    active_.store(true,std::memory_order_release);bootstrap_active.store(true,std::memory_order_release);
    em2::ResetOnlineRandomTraceHistory();em2::ResetNativeSynchronizedJoinTiming();
    em2::BeginNativeOnlineRandom(identity_);em2::CancelQueuedGameLua();return true;
}
void RoomBootstrapGate::Detach(){
    Environment environment;if(!active())return;
    const bool controls_released=em2::EndSynchronizedControls(generation_);
    em2::EndNativeOnlineRandom();
    if(controls_released){em2::ResetNativeSynchronizedJoinTiming();em2::ResetOnlineRandomTraceHistory();}
    room_=nullptr;thread_=0;generation_=0;prepared_.reset();submitted_.reset();completed_.reset();
    completion_pending_=false;readiness_=nullptr;if(bootstrap_on_thread==this)bootstrap_on_thread=nullptr;
    active_.store(false,std::memory_order_release);bootstrap_active.store(false,std::memory_order_release);bootstrap_halted.store(false,std::memory_order_release);
}
bool RoomBootstrapGate::SetJoining(bool joining){
    if(!active() || !OnOwnerThread() || stopped_ || submitted_ || prepared_ || completion_pending_)return false;
    joining_=joining;return true;
}
bool RoomBootstrapGate::SetReadinessProbe(ReadinessProbe probe){
    if(!active() || !OnOwnerThread() || stopped_ || submitted_ || prepared_ || completion_pending_)return false;
    readiness_=probe;return true;
}
bool RoomBootstrapGate::CheckGameplayReadiness(PPCContext& ctx,uint8_t* base){
    Environment environment;
    if(!active() || !OnOwnerThread() || stopped_ || !completion_pending_ || !readiness_ || !base || !em2::NativeCoopReady(base))return false;
    struct Restore {PPCContext& ctx;std::array<uint8_t,sizeof(PPCContext)> saved;
        explicit Restore(PPCContext& value):ctx(value){std::memcpy(saved.data(),&ctx,sizeof(ctx));}
        ~Restore(){std::memcpy(&ctx,saved.data(),sizeof(ctx));}} restore(ctx);
    try{return readiness_(ctx,base);}catch(...){Stop("The gameplay adapter failed its completed-tick readiness check");return false;}
}
void RoomBootstrapGate::ObservationFailed(){
    Environment environment;
    if(active() && OnOwnerThread() && !stopped_)Stop("Native initialization observation unavailable; world resynchronization required");
}
BootstrapAdmission RoomBootstrapGate::Prepare(RoomBootstrapFrame& output){
    Environment environment;
    if(!active())return BootstrapAdmission::Inactive;
    if(!OnOwnerThread())return BootstrapAdmission::WrongThread;
    if(stopped_)return BootstrapAdmission::Stopped;
    room_->Poll();
    if(em2::SynchronizedControlsGeneration()!=generation_){Stop("The native controls owner was replaced during initialization");return BootstrapAdmission::Stopped;}
    if(room_->identity()!=identity_ || room_->lobby().locked==false || Mask(room_->lobby())!=roster_){Stop("The initialization identity or frozen roster changed");return BootstrapAdmission::Stopped;}
    if(room_->state()==RoomState::Running){
        if(completion_pending_ || prepared_ || !completed_ || *completed_==UINT64_MAX ||
            (!sampling_rebased_ && room_->next_tick()!=*completed_+1)){
            Stop("Gameplay activation did not follow the completed native tick");return BootstrapAdmission::Stopped;
        }
        if(!sampling_rebased_){
            if(!em2::BeginSynchronizedGameplaySampling(generation_,*completed_)){Stop("Could not advance unused local sampling to the agreed gameplay boundary");return BootstrapAdmission::Stopped;}
            sampling_rebased_=true;
        }
        return BootstrapAdmission::Gameplay;
    }
    if(room_->state()!=RoomState::Bootstrapping){Stop(room_->error().empty()?"The initialization room disconnected":room_->error());return BootstrapAdmission::Stopped;}
    if(completion_pending_)return BootstrapAdmission::Waiting;
    if(prepared_){output=*prepared_;return BootstrapAdmission::Ready;}
    const auto tick=room_->bootstrap_tick();
    if(completed_){
        if(tick==*completed_)return BootstrapAdmission::Waiting;
        if(*completed_==UINT64_MAX || tick!=*completed_+1){Stop("Unexpected initialization round");return BootstrapAdmission::Stopped;}
    }else if(tick!=identity_.first_tick){Stop("Initialization did not begin at the negotiated tick");return BootstrapAdmission::Stopped;}
    if(!submitted_){
        Pad pad;if(joining_ && room_->local_peer()==2)pad.buttons=0x10;
        if(!room_->SubmitBootstrapPad(tick,pad)){Stop("Could not submit canonical native join input");return BootstrapAdmission::Stopped;}
        submitted_=tick;
    }
    if(*submitted_!=tick){Stop("An initialization sample crossed a round boundary");return BootstrapAdmission::Stopped;}
    RoomBootstrapFrame frame;if(!room_->PeekBootstrapFrame(frame))return BootstrapAdmission::Waiting;
    if(frame.tick!=tick || frame.players_mask!=roster_ || !CanonicalPads(frame)){
        Stop("Initialization requires exact neutral input, with Start allowed only for native player two");return BootstrapAdmission::Stopped;
    }
    prepared_=frame;output=frame;return BootstrapAdmission::Ready;
}
bool RoomBootstrapGate::Commit(const RoomBootstrapFrame& frame){
    Environment environment;
    if(!active() || !OnOwnerThread() || stopped_ || completion_pending_ || !prepared_ || frame!=*prepared_)return false;
    RoomBootstrapFrame peek;
    if(!room_->PeekBootstrapFrame(peek) || peek!=frame || !room_->TakeBootstrapFrame(peek) || peek!=frame)return Stop("The agreed initialization frame changed before commit");
    Frame retail;retail.tick=frame.tick;retail.players[0]=frame.players[0];retail.players[1]=frame.players[1];
    uint64_t actual_generation=0;Frame actual;
    if(em2::SynchronizedControlsGeneration()!=generation_ || !em2::PublishSynchronizedControls(retail) ||
       !em2::ReadPublishedSynchronizedControls(actual_generation,actual) || actual_generation!=generation_ || actual!=retail)
        return Stop("Could not publish the agreed native retail lanes");
    committed_tick_=frame.tick;completion_pending_=true;prepared_.reset();return true;
}
bool RoomBootstrapGate::PublishedRetailFrame(uint64_t& generation,Frame& frame)const{
    if(!active() || !OnOwnerThread() || stopped_ || !completion_pending_)return false;
    uint64_t actual=0;Frame published;
    if(!em2::ReadPublishedSynchronizedControls(actual,published) || actual!=generation_ || published.tick!=committed_tick_)return false;
    generation=actual;frame=published;return true;
}
bool RoomBootstrapGate::Complete(bool ready,const std::array<uint8_t,32>& digest){
    Environment environment;
    if(!active() || !OnOwnerThread() || stopped_ || !completion_pending_)return false;
    if(!room_->CompleteBootstrap(committed_tick_,ready,digest))return Stop("Could not acknowledge the completed native initialization tick");
    completion_pending_=false;completed_=committed_tick_;submitted_.reset();return true;
}
}

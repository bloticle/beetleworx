#include "online_room_gameplay.h"
#include "online_room_bootstrap.h"
#include "online_local_source.h"
#include "online_local_jump.h"
#include "online_retail_input.h"
#include "online_retail_jump.h"
#include "allocation_trace.h"
#include "controls.h"
#include "players.h"
#include "simulation.h"
#include "guest_read_batch.h"
#include <rex/logging.h>
#include <rex/input/input_system.h>
#include <windows.h>
#include <immintrin.h>
#include <cstring>
#include <optional>
#include <exception>

namespace em2::net {
namespace {
thread_local NativeRoomGameplay* current_gameplay=nullptr;
thread_local uint64_t issued_world=0;
std::atomic<bool> gameplay_halted{false},gameplay_waiting{false};
struct Environment {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(csr);SetLastError(error);}};
struct Context {
    PPCContext& ctx;std::array<uint8_t,sizeof(PPCContext)> bytes{};
    explicit Context(PPCContext& value):ctx(value){std::memcpy(bytes.data(),&ctx,sizeof(ctx));}
    ~Context(){std::memcpy(&ctx,bytes.data(),bytes.size());}
};
bool Readable(uint32_t address,size_t size,uint8_t* base){
    return base && address>=4096 && uint64_t(address)+size<=0x100000000ull && GuestReadBatch::Readable(base+address,size);
}
bool RetailRoster(const LobbySnapshot& roster){
    return roster.locked && roster.capacity==2 && roster.players.size()==2 &&
        roster.players[0].peer==1 && roster.players[1].peer==2 &&
        roster.players[0].character==LobbyCharacter::Mickey && roster.players[1].character==LobbyCharacter::Oswald;
}
}
struct NativeRoomGameplay::Impl {
    Room* room=nullptr;RoomBootstrapGate* bootstrap=nullptr;rex::input::InputSystem* input=nullptr;
    DWORD thread=0;unsigned physical=0,slot=0;
    Identity identity;LobbySnapshot roster;
    uint64_t world=0,load=0,controls=0,committed=0;
    uint32_t registry=0;
    NativeLocalMovementBinding movement;NativeLocalJumpBinding jump;
    std::array<NativeActorAction,2> dispatch{};
    std::optional<uint64_t> submitted;
    std::optional<RoomFrame> prepared,consumed;
    bool stopped=false,pending=false,delivered=false,started=false,ready_logged=false;
    std::string error,note;
    bool Owner()const{return thread && thread==GetCurrentThreadId();}
    bool Configured()const{return room!=nullptr;}
    bool Canonical()const{return room && room->identity()==identity && room->lobby()==roster && room->local_peer()==slot+1;}
    bool World(uint8_t* base)const{
        return registry && load && load!=UINT64_MAX && em2::CurrentNativeLoadGeneration()==load &&
            Readable(0x833DA7E4,4,base) && PPC_LOAD_U32(0x833DA7E4)==registry && em2::NativeCoopReady(base);
    }
    NativeActorAction Action(unsigned slot,const RoomMovement& command)const{
        auto action=dispatch[slot];action.x=command.x;action.y=command.y;return action;
    }
    bool Actions(const RoomFrame& frame,PPCContext& ctx,uint8_t* base)const{
        for(unsigned slot=0;slot<2;++slot)if(ValidateRetailMovementAction(slot,Action(slot,frame.players[slot]),ctx,base)!=ActorInputResult::Applied)return false;
        return true;
    }
    bool Stop(std::string why){
        const bool report=!stopped;
        stopped=true;gameplay_halted.store(true,std::memory_order_release);gameplay_waiting.store(false,std::memory_order_release);
        if(room)room->Disconnect();
        if(report){error=std::move(why);REXLOG_ERROR("Native Room gameplay stopped: {}",error);}
        // Retain controls and exact retail suppression until explicit shutdown.
        return false;
    }
    struct FaultOnThrow {
        Impl& state;int exceptions=std::uncaught_exceptions();
        ~FaultOnThrow()noexcept{
            if(std::uncaught_exceptions()>exceptions && state.Owner()){
                Environment preserve;
                try{state.Stop("A native gameplay transaction threw; partial delivery cannot be retried");}
                catch(...){/* Stop sets the terminal state before diagnostics. */}
            }
        }
    };
    bool Retry(std::string why){if(note!=why){note=std::move(why);REXLOG_INFO("Native Room gameplay admission waiting: {}",note);}return false;}
};
NativeRoomGameplay::NativeRoomGameplay():impl_(std::make_unique<Impl>()){}
NativeRoomGameplay::~NativeRoomGameplay(){Detach();}
bool NativeRoomGameplay::Configure(Room& room,RoomBootstrapGate& bootstrap,rex::input::InputSystem& input,unsigned physical){
    Environment environment;auto& s=*impl_;
    if(s.Configured() || current_gameplay || physical>=2 || !RetailRoster(room.lobby()) ||
        !room.native_bootstrap() || room.identity().initialization_policy!=InitializationPolicy::SharedNativeV1 ||
        (room.state()!=RoomState::Loading && room.state()!=RoomState::Bootstrapping) || room.local_peer()<1 || room.local_peer()>2 ||
        issued_world==UINT64_MAX)return false;
    s.room=&room;s.bootstrap=&bootstrap;s.input=&input;s.physical=physical;s.slot=unsigned(room.local_peer()-1);
    s.thread=GetCurrentThreadId();s.identity=room.identity();s.roster=room.lobby();s.world=++issued_world;
    current_gameplay=this;gameplay_halted.store(false,std::memory_order_release);gameplay_waiting.store(false,std::memory_order_release);
    return true;
}
void NativeRoomGameplay::Detach(){
    Environment environment;auto& s=*impl_;if(!s.Configured())return;
    // Native owner, or shutdown after all guest threads have stopped.
    DisarmRetailJump();DisarmRetailInput();DisarmNativeLocalSource();DisarmNativeLocalJump();DisarmNativeLocalMovement();
    if(current_gameplay==this)current_gameplay=nullptr;
    s=Impl{};gameplay_halted.store(false,std::memory_order_release);gameplay_waiting.store(false,std::memory_order_release);
}
bool NativeRoomGameplay::Readiness(PPCContext& ctx,uint8_t* base){
    Environment environment;Context context(ctx);auto& s=*impl_;Impl::FaultOnThrow fault{s};
    if(!s.Owner() || s.stopped || !s.Canonical() || s.started || s.room->state()!=RoomState::Bootstrapping ||
        !s.bootstrap->Owns(*s.room) || !s.bootstrap->completion_pending() || !em2::NativeCoopReady(base))return false;
    uint64_t generation=0;Frame frame;
    if(!s.bootstrap->PublishedRetailFrame(generation,frame) || frame.tick==UINT64_MAX ||
        !Readable(0x833DA7E4,4,base))return s.Retry("No completed native controls/world boundary");
    const auto registry=PPC_LOAD_U32(0x833DA7E4);
    const auto load=em2::CurrentNativeLoadGeneration();
    if(!registry || !load || load==UINT64_MAX)return s.Retry("Native world incarnation is unavailable");
    if(s.registry && (s.registry!=registry || s.load!=load || s.controls!=generation))return s.Stop("The native world or controls changed during input admission");
    if(!s.registry){s.registry=registry;s.load=load;s.controls=generation;}
    if(!ArmRetailInput(s.world,registry,generation,3,{1,1},frame.tick+1,ctx,base) ||
        !RebaseRetailInputFirstTick(frame.tick+1,ctx,base))return s.Retry("Retail controllers, motion or retained camera references are not admitted");
    // The owner publishes BOTH complete canonical retail lanes before replay.
    // Ordinary native Look therefore reads the agreed right stick; movement
    // direction still uses the sender's explicitly interpreted reference.
    if(!SetRetailCameraInputAllowed(true))return s.Retry("Canonical native camera input is not admitted");
    if(!ArmRetailJump(ctx,base))return s.Retry("Native retail Jump tasks are not admitted");
    if(!s.movement.world_epoch){
        // Discover both real event/action descriptors. Discover the local slot
        // last: only its producer lease is retained for physical sampling.
        for(unsigned pass=0;pass<2;++pass){
            const auto slot=pass?s.slot:1-s.slot;NativeLocalMovementBinding binding;bool found=false;
            for(unsigned native_user=0;native_user<4;++native_user){
                DisarmNativeLocalMovement();
                if(ArmNativeLocalMovement(s.world,registry,base,native_user,slot) &&
                    DiscoverNativeLocalMovementBinding(ctx,base,binding)){found=true;break;}
            }
            if(!found){s.movement={};return s.Retry("A retail native Movement event/action is not admitted");}
            s.dispatch[slot]={binding.event,binding.action,0,0};
            if(slot==s.slot)s.movement=binding;
        }
    }
    if(!DiscoverNativeLocalJumpBinding(s.movement,ctx,base,s.jump))return s.Retry("Local native Jump bindings are not admitted");
    if(!ArmNativeLocalSource(*s.room,s.physical,s.movement,ctx,base))return s.Retry("Local hardware source route is not admitted");
    // Only the final unanimous raw round determines the first gameplay tick.
    // No physical poll or manufactured button edge belongs in readiness.
    DisarmNativeLocalJump();
    if(!ArmNativeLocalJump(s.jump,1,frame.tick+1,ctx,base))return s.Retry("Native Jump source baseline is not neutral");
    s.registry=registry;s.load=load;s.controls=generation;s.note.clear();
    if(!s.ready_logged){
        REXLOG_INFO("Native Room gameplay sources and receivers ready: peer {}, physical device {}, native device {}, load incarnation {}, candidate first tick {}; original Movement/Jump and canonical native Look",
            s.slot+1,s.physical,s.movement.user,s.load,frame.tick+1);
        s.ready_logged=true;
    }
    return true;
}
RoomGameplayAdmission NativeRoomGameplay::Prepare(PPCContext& ctx,uint8_t* base,RoomFrame& output){
    Environment environment;Context context(ctx);output={};auto& s=*impl_;Impl::FaultOnThrow fault{s};
    if(!s.Configured())return RoomGameplayAdmission::Inactive;
    if(!s.Owner())return RoomGameplayAdmission::WrongThread;
    if(s.stopped)return RoomGameplayAdmission::Stopped;
    s.room->Poll();
    if(!s.Canonical() || s.room->state()!=RoomState::Running || !s.bootstrap->Owns(*s.room) ||
        em2::SynchronizedControlsGeneration()!=s.controls || !s.World(base)){
        s.Stop("Gameplay lost its frozen Room, world, human actors or controls lease");return RoomGameplayAdmission::Stopped;
    }
    gameplay_waiting.store(true,std::memory_order_release);
    if(s.pending || s.room->gameplay_completion_pending())return RoomGameplayAdmission::Waiting;
    if(s.prepared){output=*s.prepared;return RoomGameplayAdmission::Ready;}
    const auto tick=s.room->next_tick();
    if(!s.submitted){
        if(tick>UINT64_MAX-s.identity.input_delay || s.room->next_submit_tick()!=tick+s.identity.input_delay){
            s.Stop("Gameplay source/delayed transport timelines do not match");return RoomGameplayAdmission::Stopped;
        }
        NativeRoutedPadSample sample;RoomMovement command;NativeLocalJumpEvents edges;
        if(!SampleNativeLocalSource(*s.room,*s.input,tick,ctx,base,sample) ||
            !ProduceNativeLocalRoomMovement(*s.room,s.movement,sample,1,ctx,base,command) ||
            !ProduceNativeLocalJump(*s.room,s.jump,sample,1,ctx,base,edges)){
            s.Stop("The live local source could not produce original native Movement/Jump");return RoomGameplayAdmission::Stopped;
        }
        command.source=RoomMovementSource::NativeActionV2;command.jump_button_mask=edges.button_mask;
        command.jump_count=edges.count;command.jump_events=edges.events;
        if(ValidateRetailMovement(s.slot,command,ctx,base)!=ActorInputResult::Applied ||
            !s.room->SubmitLocal(s.room->next_submit_tick(),command)){
            s.Stop("Local intent is unsupported or transport rejected its native command");return RoomGameplayAdmission::Stopped;
        }
        s.submitted=tick;
    }
    if(*s.submitted!=tick){s.Stop("A cached physical sample crossed native ticks");return RoomGameplayAdmission::Stopped;}
    RoomFrame frame;if(!s.room->PeekFrame(frame))return RoomGameplayAdmission::Waiting;
    if(frame.tick!=tick || frame.players_mask!=3 ||
        ValidateRetailMovementFrame(frame,ctx,base)!=ActorInputResult::Applied ||
        ValidateRetailJumpFrame(frame,ctx,base)!=ActorInputResult::Applied || !s.Actions(frame,ctx,base)){
        s.Stop("Received retail frame failed complete native preflight");return RoomGameplayAdmission::Stopped;
    }
    s.prepared=frame;output=frame;gameplay_waiting.store(false,std::memory_order_release);return RoomGameplayAdmission::Ready;
}
bool NativeRoomGameplay::Commit(const RoomFrame& frame,PPCContext& ctx,uint8_t* base){
    Environment environment;Context context(ctx);auto& s=*impl_;Impl::FaultOnThrow fault{s};
    if(!s.Owner() || s.stopped || s.pending || !s.prepared || frame!=*s.prepared)return false;
    if(!s.Canonical() || !s.World(base) || ValidateRetailMovementFrame(frame,ctx,base)!=ActorInputResult::Applied ||
        ValidateRetailJumpFrame(frame,ctx,base)!=ActorInputResult::Applied || !s.Actions(frame,ctx,base))return s.Stop("Native retail admission changed before frame consumption");
    RoomFrame actual;
    if(!s.room->PeekFrame(actual) || actual!=frame || !s.room->TakeFrame(actual) || actual!=frame)return s.Stop("The authoritative gameplay frame changed before consumption");
    Frame retail;retail.tick=frame.tick;retail.players[0]=frame.players[0].pad;retail.players[1]=frame.players[1].pad;
    uint64_t generation=0;Frame published;
    if(em2::SynchronizedControlsGeneration()!=s.controls || !em2::PublishSynchronizedControls(retail) ||
        !em2::ReadPublishedSynchronizedControls(generation,published) || generation!=s.controls || published!=retail)
        return s.Stop("Could not publish the authoritative retail gameplay controls");
    if(!s.started)REXLOG_INFO("Native Room gameplay first frame consumed: tick {}, delay {}, physical source {}, native slot {}; native update and world verification still required",
        frame.tick,s.identity.input_delay,s.physical,s.slot);
    s.committed=frame.tick;s.consumed=frame;s.pending=true;s.delivered=false;s.started=true;s.prepared.reset();s.submitted.reset();return true;
}
bool NativeRoomGameplay::Deliver(PPCContext& ctx,uint8_t* base){
    Environment environment;Context context(ctx);auto& s=*impl_;Impl::FaultOnThrow fault{s};uint64_t tick=0;
    if(!s.Owner() || s.stopped || !s.pending || s.delivered || !s.consumed ||
        !em2::CurrentAgreedSimulationTick(tick) || tick!=s.committed)return false;
    if(!s.World(base) || !s.Actions(*s.consumed,ctx,base) || !BeginRetailBasisTick(s.registry,tick,ctx,base))return s.Stop("Could not begin the admitted native retail input tick");
    for(unsigned slot=0;slot<2;++slot)if(PrepareRetailMovement(slot,s.consumed->players[slot],tick,ctx,base)!=ActorInputResult::Applied)
        return s.Stop("Could not stage each retail actor's transmitted movement reference");
    for(unsigned slot=0;slot<2;++slot){
        const auto action=s.Action(slot,s.consumed->players[slot]);
        if(ReplayRetailMovement(slot,action,tick,ctx,base)!=ActorInputResult::Applied)return s.Stop("Original native retail Movement delivery failed");
    }
    for(unsigned slot=0;slot<2;++slot)if(ReplayRetailJumpBatch(slot,s.consumed->players[slot],tick,ctx,base)!=ActorInputResult::Applied)
        return s.Stop("Original native retail Jump delivery failed");
    s.delivered=true;return true;
}
bool NativeRoomGameplay::Complete(const std::array<uint8_t,32>& digest,PPCContext& ctx,uint8_t* base){
    Environment environment;Context context(ctx);auto& s=*impl_;Impl::FaultOnThrow fault{s};uint64_t tick=0;
    if(!s.Owner() || s.stopped || !s.pending || !s.delivered || !s.consumed ||
        !em2::CurrentAgreedSimulationTick(tick) || tick!=s.committed)return false;
    if(!s.World(base) || FinishRetailJumpTick(tick,ctx,base)!=ActorInputResult::Applied ||
        !FinishRetailInputTick(tick,ctx,base) || !s.room->CompleteGameplay(tick,digest))
        return s.Stop("Native gameplay completion, receiver lifecycle or world verification failed");
    // A completed 30 Hz update does not itself mean presentation is stalled.
    // Freeze interpolation only when Prepare actually cannot admit a due tick.
    s.pending=false;s.consumed.reset();return true;
}
void NativeRoomGameplay::ObservationFailed(){auto& s=*impl_;if(s.Owner() && !s.stopped)s.Stop("Completed native gameplay observation is unavailable");}
bool NativeRoomGameplay::active()const{return impl_->Configured();}
bool NativeRoomGameplay::halted()const{return impl_->stopped;}
bool NativeRoomGameplay::waiting()const{return gameplay_waiting.load(std::memory_order_acquire);}
bool NativeRoomGameplay::completion_pending()const{return impl_->pending;}
uint64_t NativeRoomGameplay::committed_tick()const{return impl_->committed;}
const std::string& NativeRoomGameplay::error()const{return impl_->error;}
NativeRoomGameplay* CurrentNativeRoomGameplay()noexcept{return current_gameplay;}
bool NativeRoomGameplayReadiness(PPCContext& ctx,uint8_t* base){return current_gameplay && current_gameplay->Readiness(ctx,base);}
bool NativeRoomGameplayHalted()noexcept{return gameplay_halted.load(std::memory_order_acquire);}
bool NativeRoomGameplayWaiting()noexcept{return gameplay_waiting.load(std::memory_order_acquire);}
}

#include "online_room_gameplay.h"
#include "online_gameover_input.h"
#include "online_cutscene_input.h"
#include "online_cutscene_skip.h"
#include "online_actor_jump.h"
#include "online_room_bootstrap.h"
#include "online_room_runtime.h"
#include "online_native_menu.h"
#include "online_room_leave.h"
#include "online_local_source.h"
#include "online_local_jump.h"
#include "online_native_triggers.h"
#include "online_button_actions.h"
#include "online_retail_input.h"
#include "online_retail_jump.h"
#include "online_retail_buttons.h"
#include "online_actor_frames.h"
#include "online_native_roster.h"
#include "online_room_probe.h"
#include "online_viewport.h"
#include "online_nametags.h"
#include "allocation_trace.h"
#include "controls.h"
#include "players.h"
#include "simulation.h"
#include "guest_read_batch.h"
#include "world_observation.h"
#include <rex/logging.h>
#include <rex/input/input_system.h>
#include <windows.h>
#include <immintrin.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <thread>
#include <optional>
#include <exception>
#include <vector>
#include <rex/cvar.h>
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(bool,em2_probe_room_recovery_controller);
REXCVAR_DEFINE_BOOL(em2_probe_unfenced_cutscene_return,false,"Debug","Private native lobby QA only: reproduce old carried-button behavior for the held-return regression");
REXCVAR_DEFINE_UINT32(em2_probe_room_partial_delivery_tick,0xFFFFFFFFu,"Debug","Explicit private native lobby QA fault: stop after original input delivery but before native physics/post-update at this tick");
REXCVAR_DEFINE_BOOL(em2_probe_room_hud_properties,false,"Debug","Private native Room QA only: observe actual authored life-property callback ownership");
REXCVAR_DEFINE_BOOL(em2_room_native_buttons,false,"Debug","Enable experimental two-player native button production and delivery in retained Room gameplay");
REXCVAR_DEFINE_BOOL(em2_probe_room_triggers,false,"Debug","Private completed-boundary trigger discovery and neutral original-math preservation check; never dispatches actions");
REXCVAR_DEFINE_BOOL(em2_room_native_reload,true,"Debug","Experimental agreed native Room world unload and fresh bootstrap admission");
PPC_EXTERN_FUNC(__imp__sub_82A14BF8);
PPC_EXTERN_FUNC(__imp__sub_82A39B48);
PPC_EXTERN_FUNC(__imp__sub_8276B878);

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
    return roster.locked && roster.capacity>=2 && roster.capacity<=16 && roster.players.size()>=2 && roster.players.size()<=roster.capacity &&
        roster.players[0].peer==1 && roster.players[1].peer==2 &&
        roster.players[0].character==LobbyCharacter::Mickey && roster.players[1].character==LobbyCharacter::Oswald;
}
bool ProbeTriggerMath(unsigned slot,const NativeLocalMovementBinding& parent,uint64_t tick,PPCContext& ctx,uint8_t* base){
    Environment environment;Context context(ctx);
    try{
        NativeTriggerBinding binding;
        if(!DiscoverNativeTriggerBinding(parent,ctx,base,binding,NativeButtonScope::DigitalAndTriggers)){
            REXLOG_WARN("Private native trigger probe P{}: no supported binding; no input or action dispatched",slot);return false;
        }
        struct Snapshot{uint32_t address;std::vector<uint8_t> bytes;};
        std::vector<Snapshot> snapshots;
        auto save=[&](uint32_t address,size_t size){
            if(!Readable(address,size,base))return false;
            snapshots.push_back({address,std::vector<uint8_t>(base+address,base+address+size)});return true;
        };
        if(!save(parent.manager,176) || !save(parent.input,88) || !save(parent.device,96) ||
            !save(binding.buttons,binding.button_count*20))return false;
        for(const auto& action:binding.actions)if(!save(action.action,436))return false;
        NativeTriggerSource source;NativeTriggerBatch batch;
        if(const auto schema=HashNativeButtonSchema(binding)){
            std::string hex;for(auto byte:*schema)hex+=fmt::format("{:02x}",byte);
            REXLOG_INFO("Private native definition schema P{}: {}; metadata only, no dispatch",slot,hex);
        }
        const uint64_t diagnostic_epoch=0x5452494747455200ull+slot+1;
        // Synthetic NEUTRAL private math only: no hardware poll, Room submit,
        // task callback, gameplay-ready vote or source-provenance claim.
        const bool armed=ArmNativeTriggers(source,binding,diagnostic_epoch,tick,ctx,base);
        const bool evaluated=armed &&
            ProduceNativeTriggers(source,binding,{},parent.user,diagnostic_epoch,tick,ctx,base,batch);
        bool unchanged=true;for(const auto& snapshot:snapshots)
            unchanged&=std::memcmp(base+snapshot.address,snapshot.bytes.data(),snapshot.bytes.size())==0;
        bool neutral=evaluated && batch.count==binding.actions.size();
        if(evaluated)for(unsigned i=0;i<batch.count;++i)neutral&=!batch.actions[i].dispatch;
        RoomButtonActions packet;std::vector<NativeActorAction> mapped;
        const bool metadata=evaluated && EncodeNativeButtonActions(binding,batch,packet) && MatchNativeButtonActions(binding,packet,mapped);
        if(metadata){std::string hex;for(auto byte:packet.schema)hex+=fmt::format("{:02x}",byte);
            REXLOG_INFO("Private native button schema P{}: {}, source tick {}, mapped {}; metadata only, no dispatch",slot,hex,packet.source_tick,mapped.size());}
        REXLOG_INFO("Private native trigger probe P{}: supported {}, excluded {}, native user {}, armed {}, evaluated {}, neutral math {}, live bytes unchanged {}; no actions dispatched",
            slot,binding.actions.size(),binding.excluded.size(),parent.user,armed,evaluated,neutral,unchanged);
        if(!armed)for(unsigned i=0;i<binding.button_count;++i)
            if(PPC_LOAD_U8(binding.buttons+i*20+1) || PPC_LOAD_U8(binding.buttons+i*20+3))
                REXLOG_INFO("Private native button baseline P{}: index {}, current {}, previous {}",slot,i,
                    PPC_LOAD_U8(binding.buttons+i*20+1),PPC_LOAD_U8(binding.buttons+i*20+3));
        if(evaluated && !neutral)for(unsigned i=0;i<batch.count;++i)if(batch.actions[i].dispatch)
            REXLOG_INFO("Private native neutral math output P{}: descriptor {} name '{}'",slot,i,binding.actions[i].name);
        for(const auto& action:binding.actions)REXLOG_INFO("Private native trigger descriptor P{}: context {} ordinal {} kind {} key {:08X} name '{}'",
            slot,action.context,action.ordinal,action.kind,action.key,action.name);
        for(const auto& action:binding.excluded)REXLOG_INFO("Private native trigger exclusion P{}: ordinal {} kind {} condition {} curve {} reason '{}'",
            slot,action.ordinal,action.kind,action.condition,action.curve,action.reason);
        return neutral && unchanged && metadata;
    }catch(const std::exception& error){REXLOG_WARN("Private native trigger probe P{} failed: {}; no actions dispatched",slot,error.what());}
    catch(...){REXLOG_WARN("Private native trigger probe P{} failed; no actions dispatched",slot);}
    return false;
}
}
struct NativeRoomGameplay::Impl {
    Room* room=nullptr;RoomBootstrapGate* bootstrap=nullptr;rex::input::InputSystem* input=nullptr;
    rex::input::InputSystem* original_input=nullptr;
    std::unique_ptr<rex::input::InputSystem> scripted_input;
    DWORD thread=0;unsigned physical=0,slot=0;
    uint64_t peer=0;uint16_t roster_mask=0;
    std::unique_ptr<NativeRoomActors> actors=std::make_unique<NativeRoomActors>();
    Identity identity;LobbySnapshot roster;
    uint64_t world=0,load=0,controls=0,committed=0;
    uint32_t registry=0;
    NativeLocalMovementBinding movement;NativeLocalJumpBinding jump;
    std::array<uint32_t,2> label_entities{},label_handles{};
    std::array<NativeTriggerBinding,2> button_definitions;
    NativeTriggerBinding button_binding;std::unique_ptr<NativeTriggerSource> button_source=std::make_unique<NativeTriggerSource>();
    std::array<NativeActorAction,2> dispatch{};
    std::array<std::unique_ptr<RetailButtonReceiver>,2> button_receivers;
    std::array<std::array<uint8_t,32>,2> button_schemas{};
    bool button_receiver_probe_done=false;
    bool history_probe_done=false;
    bool native_buttons=false,button_delivery_enabled=false;
    std::optional<uint64_t> native_completed;
    uint64_t input_epoch=1;
    std::optional<uint32_t> normal_control_mode;
    std::string normal_context_name;
    bool return_pending=false;
    std::optional<uint64_t> return_wait_start;
    std::optional<uint64_t> cutscene_entry,cutscene_resume;
    std::unique_ptr<NativeCutsceneSkipSource> private_skip_source;
    NativeCutsceneUiGate skip_gate;
    std::optional<uint16_t> departing_cutscene_buttons;
    bool skip_target_available=false;
    uint32_t skip_target_mode=0;
    std::unique_ptr<NativeMovieAcceptReceiver> movie_receiver;
    uint32_t movie_sequence_target=0;
    uint32_t movie_control_mode=0;
    std::optional<uint64_t> skip_target_entry;
    std::unique_ptr<NativeCutsceneSkipReceiver> skip_receiver;
    bool private_skip_armed=false,private_skip_failed=false;
    void ProbeSkip(const Pad& pad,uint64_t tick,PPCContext& ctx,uint8_t* base){
        if(!REXCVAR_GET(em2_private_profile) || return_pending || private_skip_failed)return;
        try{
            if(!private_skip_source)private_skip_source=std::make_unique<NativeCutsceneSkipSource>();
            if(!private_skip_armed){
                private_skip_armed=private_skip_source->Arm(registry,movement.user,input_epoch,tick,ctx,base);
                if(!private_skip_armed)return;
                REXLOG_INFO("Private native cutscene skip source armed: tick {} input epoch {} native device {}; private original math only, no dispatch or submit",tick,input_epoch,movement.user);
            }
            NativeCutsceneSkipSample sample;
            if(!private_skip_source->Produce(pad,movement.user,input_epoch,tick,ctx,base,sample)){
                private_skip_failed=true;REXLOG_WARN("Private native cutscene skip source rejected sample: tick {}; no callbacks dispatched",tick);return;
            }
            if(std::any_of(sample.dispatch.begin(),sample.dispatch.end(),[](bool fired){return fired;}))
                REXLOG_INFO("Private native cutscene skip result: tick {} epoch {} prompt {} hold {} release {}; original math, no callback dispatch or wire submission",tick,input_epoch,sample.dispatch[0],sample.dispatch[1],sample.dispatch[2]);
        }catch(...){private_skip_failed=true;REXLOG_WARN("Private native cutscene skip source failed; no callbacks dispatched");}
    }
    std::array<std::array<uint8_t,32>,kOnlineRoomCapacity> cutscene_schemas{};
    bool prepared_passive=false,consumed_passive=false;
    bool CutsceneFrame(const RoomFrame& frame,PPCContext& ctx,uint8_t* base)const{
        if(cutscene_entry){
            bool phase=ReadNativeCutsceneContext(ctx,base,registry,movie_sequence_target);
            if(!phase && return_pending && normal_control_mode){NativeGameOverState state;uint32_t mode=0;
                phase=ReadNativeControlState(ctx,base,state,mode) && state.registry==registry && mode==*normal_control_mode;}
            return phase && ValidNativeCutsceneFrame(frame,roster_mask,*cutscene_entry,identity.input_delay,input_epoch,cutscene_schemas,native_buttons);
        }
        if(cutscene_resume && frame.tick>*cutscene_resume && *cutscene_resume<=UINT64_MAX-identity.input_delay &&
           frame.tick<=*cutscene_resume+identity.input_delay){
            NativeGameOverState state;uint32_t mode=0;
            return normal_control_mode && ReadNativeControlState(ctx,base,state,mode) && state.registry==registry &&
                mode==*normal_control_mode && NativeCutsceneReturnPrefix(frame,roster_mask,input_epoch-1);
        }return false;
    }
    // Frozen for the rest of this native world. Only its original load and a
    // new bootstrap may re-arm gameplay sources after terminal GameOver.
    std::optional<uint64_t> game_over_entry;
    std::array<std::array<uint8_t,32>,kOnlineRoomCapacity> game_over_schemas{};
    bool GameOver(PPCContext& ctx,uint8_t* base)const{
        NativeGameOverState state;
        return game_over_entry && ReadNativeGameOverState(ctx,base,state) &&
            state.registry==registry && state.dead<=roster.players.size();
    }
    bool GameOverFrame(const RoomFrame& frame,PPCContext& ctx,uint8_t* base)const{
        return GameOver(ctx,base) && ValidNativeGameOverFrame(frame,roster_mask,
            *game_over_entry,identity.input_delay,game_over_schemas,native_buttons,input_epoch);
    }
    bool native_unwind=false;
    std::optional<uint64_t> first_game_tick;
    std::optional<uint64_t> camera_tick;
    std::unique_ptr<OwnedMovementFrames> actor_frames;
    uint64_t button_frames=0;
    std::optional<uint64_t> submitted;
    std::optional<RoomFrame> prepared,consumed;
    bool stopped=false,pending=false,delivered=false,started=false,ready_logged=false;
    uint64_t load_sequence=0;unsigned load_depth=0;
    bool transition_pending=false,transition_reentry=false;
    std::optional<LoadPoint> last_load;
    unsigned trigger_probe_mask=0;
    std::array<unsigned,2> trigger_probe_attempts{};
    std::array<uint64_t,2> trigger_probe_tick{UINT64_MAX,UINT64_MAX};
    std::string error,note;
    bool Owner()const{return thread && thread==GetCurrentThreadId();}
    bool Configured()const{return room!=nullptr;}
    bool Canonical()const{return room && room->identity()==identity && room->lobby()==roster && room->local_peer()==peer;}
    const char* WorldFailure(uint8_t* base)const{
        if(!registry || !load || load==UINT64_MAX)return "native world bindings were not established";
        if(em2::CurrentNativeLoadGeneration()!=load)return "native load generation changed";
        if(!Readable(0x833DA7E4,4,base) || PPC_LOAD_U32(0x833DA7E4)!=registry)return "native entity registry changed or unloaded";
        if(!em2::NativeCoopRuntimeReady(base)){const auto reason=em2::NativeCoopReadinessFailure(base);return reason?reason:"native co-op runtime camera ownership is invalid";}
        return nullptr;
    }
    bool World(uint8_t* base)const{return !WorldFailure(base);}
    NativeActorAction Action(unsigned slot,const RoomMovement& command)const{
        auto action=dispatch[slot];action.x=command.x;action.y=command.y;return action;
    }
    bool Actions(const RoomFrame& frame,PPCContext& ctx,uint8_t* base)const{
        for(unsigned slot=0;slot<2;++slot)if(ValidateRetailMovementAction(slot,Action(slot,frame.players[slot]),ctx,base)!=ActorInputResult::Applied)return false;
        return true;
    }
    RoomFrame RetailFrame(const RoomFrame& full)const{RoomFrame frame;frame.tick=full.tick;frame.players_mask=3;frame.players[0]=full.players[0];frame.players[1]=full.players[1];return frame;}
    bool ButtonFrame(const RoomFrame& full,PPCContext& ctx,uint8_t* base)const{
        const auto frame=RetailFrame(full);
        if(!native_buttons)return ValidateRetailJumpFrame(frame,ctx,base)==ActorInputResult::Applied;
        if(!first_game_tick)return false;
        const auto phase=ClassifyRetailButtonFrame(frame,*first_game_tick,identity.input_delay);
        if(phase==RetailButtonPhase::StartupNeutral){
            return ValidateRetailJumpFrame(frame,ctx,base)==ActorInputResult::Applied;
        }
        return phase==RetailButtonPhase::NativeButtons && ValidateRetailButtonFrame(frame,ctx,base)==ActorInputResult::Applied;
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
                state.native_unwind=true;
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
        (room.state()!=RoomState::Loading && room.state()!=RoomState::Bootstrapping) || room.local_peer()<1 || room.local_peer()>16 ||
        issued_world==UINT64_MAX)return false;
    const auto local=std::find_if(room.lobby().players.begin(),room.lobby().players.end(),[&](const auto& player){return player.peer==room.local_peer();});
    if(local==room.lobby().players.end() || 
        !s.actors->Configure(room.lobby()))return false;
    s.room=&room;s.bootstrap=&bootstrap;s.input=&input;s.original_input=&input;s.physical=physical;s.peer=room.local_peer();
    s.slot=local->character==LobbyCharacter::Mickey?0u:1u;s.roster_mask=uint16_t(s.actors->mask()|3);
    s.thread=GetCurrentThreadId();s.identity=room.identity();s.roster=room.lobby();s.world=++issued_world;
    s.native_buttons=REXCVAR_GET(em2_room_native_buttons);
    s.scripted_input=MakePrivateRoomInput(room);if(s.scripted_input)s.input=s.scripted_input.get();
    current_gameplay=this;gameplay_halted.store(false,std::memory_order_release);gameplay_waiting.store(false,std::memory_order_release);
    return true;
}
void NativeRoomGameplay::Detach(){
    ClearOnlineNameTags();
    Environment environment;auto& s=*impl_;if(!s.Configured())return;
    // Native owner, or shutdown after all guest threads have stopped.
    DisarmRetailButtonDelivery();DisarmRetailJump();DisarmRetailInput();DisarmNativeLocalSource();DisarmNativeLocalJump();DisarmNativeLocalMovement();
    ResetNativeTriggers(*s.button_source);
    if(current_gameplay==this)current_gameplay=nullptr;
    s=Impl{};gameplay_halted.store(false,std::memory_order_release);gameplay_waiting.store(false,std::memory_order_release);
}
bool NativeRoomGameplay::InitializeActors(PPCContext& ctx,uint8_t* base){
    Environment environment;Context context(ctx);auto& s=*impl_;Impl::FaultOnThrow fault{s};
    if(!s.Owner() || s.stopped || !s.Canonical() || s.started || s.room->state()!=RoomState::Bootstrapping ||
        !s.bootstrap->Owns(*s.room) || !s.bootstrap->completion_pending())return false;
    const auto result=s.actors->Prepare(ctx,base);
    if(result==NativeRosterStep::Failed)return s.Stop(s.actors->error().empty()?"Native human roster admission failed":s.actors->error());
    return result==NativeRosterStep::Ready;
}
uint16_t NativeRoomGameplay::extra_actor_mask()const{return impl_->actors->ready()?impl_->actors->mask():0;}
uint32_t NativeRoomGameplay::NativeDeathParticipantCount(PPCContext& ctx,uint8_t* base)const{
    Environment environment;Context preserve(ctx);const auto& s=*impl_;uint64_t tick=0;
    if(!s.Owner() || s.stopped || !s.started || !s.pending || !s.delivered || !s.Canonical() ||
       s.room->state()!=RoomState::Running || !s.World(base) || !s.actors->ready() ||
       !em2::CurrentAgreedSimulationTick(tick) || tick!=s.committed)return 0;
    const auto count=s.roster.players.size();
    if(count<2 || count>kOnlineRoomCapacity || s.actors->actors().size()!=count-2)return 0;
    for(const auto& entry:s.actors->actors()){
        OwnedActorInput actor;if(!OwnedActorForPeer(entry.room_peer,ctx,base,actor))return 0;
    }
    return uint32_t(count);
}
bool NativeRoomGameplay::OwnedActorForPeer(uint64_t peer,PPCContext& ctx,uint8_t* base,OwnedActorInput& out)const{
    Environment environment;Context preserve(ctx);out={};const auto& s=*impl_;
    if(!s.Owner() || s.stopped || !s.started || !s.Canonical() || !s.World(base) || !s.actors->ready())return false;
    for(const auto& entry:s.actors->actors())if(entry.room_peer==peer){
        OwnedActorInput current;
        if(!OwnedActorInputFor(entry.actor.peer,ctx,base,current) || !current.enabled ||
           current.generation!=entry.actor.generation || current.registry!=entry.actor.registry ||
           current.entity!=entry.actor.entity || current.controller!=entry.actor.controller ||
           current.listener!=entry.actor.listener || current.handle!=entry.actor.handle || current.action_user!=entry.actor.action_user)return false;
        out=current;return true;
    }
    return false;
}
uint32_t NativeRoomGameplay::LocalCameraController()const{const auto& s=*impl_;return s.Owner() && s.peer>2 && s.load==em2::CurrentNativeLoadGeneration()?s.actors->CameraController(s.peer):0;}
uint32_t NativeRoomGameplay::LocalHudActor(unsigned profile,PPCContext& ctx,uint8_t* base)const{
    Environment environment;Context preserve(ctx);const auto& s=*impl_;
    if(profile>1 || !s.Owner() || s.stopped || !s.started || s.peer<=2 || !s.World(base) || !s.actors->ready())return 0;
    for(const auto& entry:s.actors->actors())if(entry.room_peer==s.peer && entry.actor.action_user==profile){
        OwnedActorInput current;
        if(OwnedActorInputFor(entry.actor.peer,ctx,base,current) && current.enabled &&
            current.generation==entry.actor.generation && current.entity==entry.actor.entity)return current.entity;
    }
    return 0;
}
bool NativeRoomGameplay::OwnsCameraMode(uint32_t mode,uint8_t* base)const{return impl_->Owner() && impl_->actors->OwnsCameraMode(mode,base);}
bool NativeRoomGameplay::RenderBindingsAvailable(uint8_t* base)const{
    const auto& s=*impl_;
    // Startup still renders its ordinary menus/loading world. After gameplay
    // admission, old online pose/camera-target bindings cannot be reused.
    return !s.Configured() || (s.Owner() && !s.stopped && (s.started?s.World(base):(!s.transition_reentry || (s.actors->ready() && em2::NativeCoopReady(base)))));
}
bool NativeRoomGameplay::NativeLoadPoint(std::string_view destination,LoadPhase phase,bool success,PPCContext& ctx,uint8_t* base){
    Environment environment;Context context(ctx);auto& s=*impl_;Impl::FaultOnThrow fault{s};
    if(!s.Configured() || !s.started || !REXCVAR_GET(em2_room_native_reload))return true;
    uint64_t tick=0;
    if(!s.Owner() || s.stopped || !s.pending || !s.delivered || !em2::CurrentAgreedSimulationTick(tick) ||
       tick!=s.committed || destination.empty() || destination.size()>=96 || s.load_sequence==UINT64_MAX)
        return s.Stop("Native loader entered outside its consumed Room tick");
    LoadPoint point;point.sequence=s.load_sequence;point.tick=tick;point.phase=phase;point.success=success;
    std::copy(destination.begin(),destination.end(),point.destination.begin());
    const auto wait=[&](auto ready){
        const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(120);
        do{
            if(NativeRoomLoadsCancelled())return false;
            UpdateNativeRoomSession(base,false);
            if(s.room->state()!=RoomState::Running)return false;
            if(ready())return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }while(std::chrono::steady_clock::now()<until);return false;
    };
    if(phase==LoadPhase::Begin && !s.load_depth){
        if(!s.World(base) || !wait([&]{return (s.first_game_tick==tick && !s.room->verified_gameplay_tick()) ||
             (tick && s.room->verified_gameplay_tick()==tick-1);}))return s.Stop("Could not drain old-world proof before native load");
    }
    if(!s.room->SubmitLoadPoint(point) || !wait([&]{return s.room->LoadPointAgreed(point);}))return s.Stop("Native Room loader rendezvous failed");
    ++s.load_sequence;s.last_load=point;
    if(phase==LoadPhase::Begin){
        if(!s.load_depth){
            if(!s.actors->PrepareWorldUnload(ctx,base))return s.Stop("Could not release old-world actor bindings before native unload");
            ClearOnlineNameTags();
        }
        ++s.load_depth;
    }else{
        if(!s.load_depth)return s.Stop("Native Room loader End has no matching Begin");
        --s.load_depth;if(!success)return s.Stop("The original native level loader failed");
        if(!s.load_depth)s.transition_pending=true;
    }
    REXLOG_INFO("Native Room loader agreed: tick {}, sequence {}, phase {}, destination '{}'",tick,point.sequence,unsigned(phase),destination);
    return true;
}
bool NativeRoomGameplay::transition_pending()const{return impl_->transition_pending;}
bool NativeRoomGameplay::CompleteWorldTransition(PPCContext& ctx,uint8_t* base){
    Environment environment;Context context(ctx);auto& s=*impl_;Impl::FaultOnThrow fault{s};uint64_t tick=0;
    if(!s.Owner() || s.stopped || !s.transition_pending || s.load_depth || !s.last_load || !s.pending ||
       !s.delivered || !em2::CurrentAgreedSimulationTick(tick) || tick!=s.committed ||
       em2::CurrentNativeLoadGeneration()==s.load || !Readable(0x833DA7E4,4,base) || !PPC_LOAD_U32(0x833DA7E4))return s.Stop("Native reload has no completed changed-world boundary");
    const auto level=em2::CurrentNativeLoadLevel();
    if(std::string_view(level.data(),strnlen_s(level.data(),level.size()))!=s.last_load->destination.data())return s.Stop("Native reload destination differs from agreed loader");
    em2::PrepareSimulationWorldUnload(ctx,base);
    if(!s.actors->FinishWorldUnload(ctx,base))return s.Stop("Could not finalize old cameras at the completed native reload boundary");
    const auto world=NativeRoomLoadedWorldToken();
    if(!world || !s.room->RebootstrapLoadedWorld(*s.last_load,*world))return s.Stop("Could not assert the completed native reload world");
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(120);
    while(s.room->state()==RoomState::Running && std::chrono::steady_clock::now()<until && !NativeRoomLoadsCancelled()){
        UpdateNativeRoomSession(base,false);std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if(s.room->state()!=RoomState::Bootstrapping || NativeRoomLoadsCancelled())return s.Stop("Reloaded Room world agreement failed");
    auto* room=s.room;auto* gate=s.bootstrap;auto* input=s.original_input;const auto physical=s.physical;
    const auto first=room->bootstrap_tick();
    Detach();
    if(!gate->ReenterLoadedWorld() || !gate->SetJoining(!em2::NativeCoopReady(base)) || !Configure(*room,*gate,*input,physical))return impl_->Stop("Could not rebuild native gameplay bindings after world agreement");
    impl_->transition_reentry=true;
    REXLOG_INFO("Native Room reload entered fresh admission: raw first {}, new controls generation {}",first,em2::SynchronizedControlsGeneration());
    return true;
}
bool NativeRoomGameplay::NameTagActors(std::vector<OnlineNameActor>& out,uint64_t& local,uint8_t* base,std::array<uint8_t,16>* session)const{
    auto& s=*impl_;if(!s.Owner() || s.stopped || !s.started || !s.World(base) || !s.actors->ready() || s.room->state()!=RoomState::Running)return false;
    const auto manager=Readable(0x833DAB60,4,base)?PPC_LOAD_U32(0x833DAB60):0;
    if(!Readable(manager,640,base))return false;
    for(const auto& player:s.roster.players){
        uint32_t entity=0;
        if(player.peer<=2){
            const auto slot=unsigned(player.peer-1);
            if(PPC_LOAD_U32(manager+slot*320+20)!=s.label_handles[slot])return false;
            entity=s.label_entities[slot];
        }else for(const auto& entry:s.actors->actors())if(entry.room_peer==player.peer){entity=entry.actor.entity;break;}
        if(!Readable(entity,96,base) || player.color>=kLobbyColors.size())return false;
        out.push_back({player.peer,entity,kLobbyColors[player.color].rgba,player.name,unsigned(player.character)});
    }
    local=s.peer;if(session)*session=s.identity.session;return out.size()==s.roster.players.size();
}
void NativeRoomGameplay::UpdateExtraCameras(PPCContext& ctx,uint8_t* base){
    Environment environment;Context context(ctx);auto& s=*impl_;Impl::FaultOnThrow fault{s};uint64_t tick=0;
    if(!s.Owner() || s.stopped || !s.pending || !s.delivered || !s.actors->mask() || !s.World(base) ||
        !em2::CurrentAgreedSimulationTick(tick) || tick!=s.committed || !Readable(0x833DAB60,4,base))return;
    NativeGameOverState control;uint32_t mode=0;
    if(ReadNativeControlState(ctx,base,control,mode) && NativeMovieControlMode(mode) && control.registry==s.registry &&
       ReadNativeCutsceneContext(ctx,base,s.registry,s.movie_sequence_target)){
        if(tick%60==0)REXLOG_INFO("Native Room movie camera retained: tick {}; original movie view, no independent gameplay look or local view replacement",tick);
        return;
    }
    const auto manager=PPC_LOAD_U32(0x833DAB60);
    if(!Readable(manager,352,base) || ctx.r3.u32!=PPC_LOAD_U32(manager+348) || s.camera_tick==tick)return;
    s.camera_tick=tick;
    if(!s.consumed || !s.actors->UpdateCameras(*s.consumed,ctx,base) || !BindNativeRoomLocalView(ctx,base))s.Stop("Independent native camera update or local view binding failed");
}
bool NativeRoomGameplay::ReadMovieClock(uint32_t manager,PPCContext& ctx,uint8_t* base,uint64_t& tick)const{
    Environment environment;Context context(ctx);const auto& s=*impl_;
    if(!s.Owner() || s.stopped || !s.started || !s.native_completed || !s.World(base) || !s.Canonical() ||
       s.room->state()!=RoomState::Running || !Readable(0x833DCCB8,4,base) ||
       PPC_LOAD_U32(0x833DCCB8)!=manager || !Readable(manager,132,base))return false;
    NativeGameOverState control;uint32_t mode=0;
    if(!ReadNativeControlState(ctx,base,control,mode) || !NativeMovieControlMode(mode) || control.registry!=s.registry ||
       !ReadNativeCutsceneContext(ctx,base,s.registry,s.movie_sequence_target))return false;
    uint64_t current=0;
    if(em2::CurrentAgreedSimulationTick(current)){
        if(!s.pending || !s.delivered || current!=s.committed)return false;
    }else current=*s.native_completed;
    tick=current;return true;
}
bool NativeRoomGameplay::Readiness(PPCContext& ctx,uint8_t* base){
    Environment environment;Context context(ctx);auto& s=*impl_;Impl::FaultOnThrow fault{s};
    if(!s.Owner() || s.stopped || !s.Canonical() || s.started || s.room->state()!=RoomState::Bootstrapping ||
        !s.bootstrap->Owns(*s.room) || !s.bootstrap->completion_pending() || !s.actors->ready() || !em2::NativeCoopReady(base))return false;
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
            if(s.native_buttons || (REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_probe_room_triggers))){
                NativeTriggerBinding definitions;
                if(DiscoverNativeTriggerBinding(binding,ctx,base,definitions,NativeButtonScope::DigitalAndTriggers)){
                    if(!s.button_receivers[slot])s.button_receivers[slot]=std::make_unique<RetailButtonReceiver>();
                    if(ArmRetailButtons(*s.button_receivers[slot],slot,definitions,ctx,base)){
                        if(const auto schema=HashNativeButtonSchema(definitions))s.button_schemas[slot]=*schema;
                        s.button_definitions[slot]=definitions;
                        if(s.native_buttons && slot==s.slot)s.button_binding=definitions;
                    }else REXLOG_WARN("Private native button receiver P{}: ownership/definition capture rejected; no dispatch",slot);
                }
                if(s.native_buttons && (!s.button_receivers[slot] || s.button_schemas[slot]==std::array<uint8_t,32>{})){
                    s.movement={};return s.Retry("Native button definitions are not admitted for both retail actors");
                }
            }
            if(REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_probe_room_triggers) && !(s.trigger_probe_mask&(1u<<slot))){
                ++s.trigger_probe_attempts[slot];
                s.trigger_probe_tick[slot]=frame.tick+1;
                if(ProbeTriggerMath(slot,binding,frame.tick+1,ctx,base))s.trigger_probe_mask|=1u<<slot;
            }
            s.dispatch[slot]={binding.event,binding.action,0,0};
            s.label_entities[slot]=binding.entity;s.label_handles[slot]=binding.handle;
            if(slot==s.slot)s.movement=binding;
        }
    }
    if(s.normal_context_name.empty() && !ReadNativeInputContextName(base,s.normal_context_name))return s.Retry("Original gameplay input-context name is not available");
    if(!DiscoverNativeLocalJumpBinding(s.movement,ctx,base,s.jump))return s.Retry("Local native Jump bindings are not admitted");
    if(!ArmNativeLocalSource(*s.room,s.physical,s.movement,ctx,base))return s.Retry("Local hardware source route is not admitted");
    // Only the final unanimous raw round determines the first gameplay tick.
    // No physical poll or manufactured button edge belongs in readiness.
    DisarmNativeLocalJump();
    if(!ArmNativeLocalJump(s.jump,1,frame.tick+1,ctx,base))return s.Retry("Native Jump source baseline is not neutral");
    if(s.native_buttons){
        if(!ResetNativeTriggers(*s.button_source) || !ArmNativeTriggers(*s.button_source,s.button_binding,1,frame.tick+1,ctx,base))
            return s.Retry("Native button source baseline is not neutral");
        if(!s.button_delivery_enabled){
            if(!EnableRetailButtonDelivery({s.button_receivers[0].get(),s.button_receivers[1].get()},ctx,base))
                return s.Retry("Complete two-slot native button delivery is not admitted");
            s.button_delivery_enabled=true;
        }
    }
    s.registry=registry;s.load=load;s.controls=generation;s.note.clear();
    if(!s.ready_logged){
        REXLOG_INFO("Native Room gameplay sources and receivers ready: peer {}, physical device {}, native device {}, load incarnation {}, candidate first tick {}; original Movement/Jump and canonical native Look",
            s.peer,s.physical,s.movement.user,s.load,frame.tick+1);
        s.ready_logged=true;
    }
    return true;
}
bool NativeRoomGameplay::RearmInputContext(uint64_t tick,PPCContext& ctx,uint8_t* base){
    auto& s=*impl_;const auto old_epoch=s.input_epoch-1;
    const char* step="owner boundary";bool finished=false;
    struct Diagnostic {const char*& step;bool& finished;uint64_t tick;~Diagnostic(){if(!finished)REXLOG_ERROR("Native cutscene rearm rejected: source tick {} stage '{}'",tick,step);}} diagnostic{step,finished,tick};
    if(!s.Owner() || !s.cutscene_entry || !s.native_completed || *s.native_completed!=tick-1 ||
       s.input_epoch==UINT64_MAX || !RefreshOwnedActorTaskPermissions(ctx,base))return false;
    const auto next_epoch=s.input_epoch+1;
    // No native update or physical poll can interleave this owner-thread
    // transition. Quarantine stays armed while old capabilities are retired.
    DisarmRetailButtonDelivery();DisarmRetailJump();DisarmNativeLocalSource();DisarmNativeLocalJump();DisarmNativeLocalMovement();
    step="retail context generation";if(!BeginRetailContextRearm(old_epoch,next_epoch,tick,ctx,base))return false;
    s.button_delivery_enabled=false;s.movement={};s.jump={};s.button_binding={};
    s.button_source=std::make_unique<NativeTriggerSource>();
    for(auto& receiver:s.button_receivers)receiver.reset();
    for(unsigned pass=0;pass<2;++pass){
        const auto slot=pass?s.slot:1-s.slot;NativeLocalMovementBinding binding;bool found=false;
        for(unsigned user=0;user<4;++user){DisarmNativeLocalMovement();
            if(ArmNativeLocalMovement(s.world,s.registry,base,user,slot) && DiscoverNativeLocalMovementBinding(ctx,base,binding)){found=true;break;}}
        step=slot?"Oswald movement discovery":"Mickey movement discovery";
        if(!found || binding.entity!=s.label_entities[slot] || binding.handle!=s.label_handles[slot])return false;
        s.dispatch[slot]={binding.event,binding.action,0,0};
        if(s.native_buttons){
            NativeTriggerBinding definitions;step=slot?"Oswald button definitions":"Mickey button definitions";
            if(!DiscoverNativeTriggerBinding(binding,ctx,base,definitions,NativeButtonScope::DigitalAndTriggers))return false;
            auto receiver=std::make_unique<RetailButtonReceiver>();
            if(!ArmRetailButtons(*receiver,slot,definitions,ctx,base))return false;
            const auto schema=HashNativeButtonSchema(definitions);if(!schema)return false;
            s.button_schemas[slot]=*schema;s.button_definitions[slot]=definitions;s.button_receivers[slot]=std::move(receiver);
            if(slot==s.slot)s.button_binding=definitions;
        }
        if(slot==s.slot)s.movement=binding;
    }
    step="retail Jump receiver";if(!ArmRetailJump(ctx,base))return false;
    step="local Jump definitions";if(!DiscoverNativeLocalJumpBinding(s.movement,ctx,base,s.jump))return false;
    step="local source route";if(!ArmNativeLocalSource(*s.room,s.physical,s.movement,ctx,base))return false;
    step="departing cutscene button fence";
    if(!s.departing_cutscene_buttons)return false;
    const bool unfenced=REXCVAR_GET(em2_private_profile) && NativeOnlineMenuQaActive() && REXCVAR_GET(em2_probe_unfenced_cutscene_return);
    if(!unfenced && !FenceNativeLocalSourceButtons(*s.room,*s.departing_cutscene_buttons,ctx,base))return false;
    if(unfenced)REXLOG_INFO("Private native cutscene return fence bypassed: source tick {} held {:04X}; old behavior reproduction only",tick,*s.departing_cutscene_buttons);
    step="local Jump source baseline";if(!ArmNativeLocalJump(s.jump,next_epoch,tick,ctx,base))return false;
    if(s.native_buttons){
        step="local button source and delivery";
        if(!ArmNativeTriggers(*s.button_source,s.button_binding,next_epoch,tick,ctx,base) ||
           !EnableRetailButtonDelivery({s.button_receivers[0].get(),s.button_receivers[1].get()},ctx,base))return false;
        s.button_delivery_enabled=true;
    }
    step="finish retail context";if(!FinishRetailContextRearm(s.native_buttons,ctx,base) || tick>UINT64_MAX-s.identity.input_delay)return false;
    const auto first=tick+s.identity.input_delay;
    step="owned Jump stream epochs";for(const auto& actor:s.actors->actors())if(!RearmOwnedActorJumpInput(actor.actor.peer,actor.actor.generation,old_epoch,next_epoch,first,ctx,base))return false;
    auto frames=std::make_unique<OwnedMovementFrames>();std::array<uint64_t,kOnlineRoomCapacity> epochs{};
    for(const auto& player:s.roster.players)epochs[player.peer-1]=1;
    step="owned frame coordinator";if(!frames->Configure(s.roster_mask,3,epochs,first,s.registry,ValidateRetailMovementFrame,next_epoch) || !s.actors->BindFrames(*frames,ctx,base))return false;
    if(s.native_buttons)for(const auto& actor:s.actors->actors())
        if(!frames->BindButtons(actor.room_peer,s.button_definitions[actor.actor.action_user],ctx,base))return false;
    finished=true;s.actor_frames=std::move(frames);s.input_epoch=next_epoch;s.cutscene_resume=*s.native_completed;s.cutscene_entry.reset();s.movie_sequence_target=0;s.movie_control_mode=0;s.return_pending=false;s.return_wait_start.reset();
    s.skip_receiver.reset();s.movie_receiver.reset();s.skip_target_available=false;s.skip_target_mode=0;s.skip_target_entry.reset();
    REXLOG_INFO("Native Room cutscene bindings re-admitted: source tick {}, first delivery {}, input epoch {}; actor/world/controls incarnation retained, fresh producer/receiver leases",tick,first,next_epoch);
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
    if(s.pending || s.room->gameplay_frame_blocked())return RoomGameplayAdmission::Waiting;
    NativeGameOverState terminal;
    if(!s.game_over_entry && s.started && s.native_completed && ReadNativeGameOverState(ctx,base,terminal)){
        const auto count=s.roster.players.size();
        bool owned=count>=2 && count<=kOnlineRoomCapacity && terminal.dead==count &&
            terminal.registry==s.registry && s.actors->ready() && s.actors->actors().size()==count-2 &&
            *s.native_completed!=UINT64_MAX && s.room->next_tick()==*s.native_completed+1;
        for(const auto& actor:s.actors->actors()){OwnedActorInput current;owned=OwnedActorForPeer(actor.room_peer,ctx,base,current) && owned;}
        if(!owned){s.Stop("Native GameOver lost its admitted all-human death ownership");return RoomGameplayAdmission::Stopped;}
        for(const auto& player:s.roster.players)s.game_over_schemas[player.peer-1]=s.button_schemas[player.character==LobbyCharacter::Mickey?0:1];
        s.game_over_entry=*s.native_completed;
        REXLOG_INFO("Native Room terminal GameOver admitted: entry tick {}, participants {}; original state 7, all humans dead, gameplay callbacks frozen until original load",*s.game_over_entry,count);
    }
    if(s.game_over_entry){
        if(!s.GameOver(ctx,base)){s.Stop("Terminal native GameOver changed without an original world transition");return RoomGameplayAdmission::Stopped;}
        if(s.prepared){output=*s.prepared;return RoomGameplayAdmission::Ready;}
        const auto tick=s.room->next_tick();
        if(!s.submitted){
            if(tick>UINT64_MAX-s.identity.input_delay || s.room->next_submit_tick()!=tick+s.identity.input_delay ||
               !s.room->SubmitLocal(s.room->next_submit_tick(),NativeGameOverCommand(s.input_epoch))){
                s.Stop("Native GameOver no-action command submission failed");return RoomGameplayAdmission::Stopped;}
            s.submitted=tick;
        }
        if(*s.submitted!=tick){s.Stop("Native GameOver source crossed native ticks");return RoomGameplayAdmission::Stopped;}
        RoomFrame frame;if(!s.room->PeekFrame(frame))return RoomGameplayAdmission::Waiting;
        if(frame.tick!=tick || !s.GameOverFrame(frame,ctx,base)){
            s.Stop("Native GameOver authoritative frame failed phase admission");return RoomGameplayAdmission::Stopped;}
        s.prepared=frame;output=frame;gameplay_waiting.store(false,std::memory_order_release);return RoomGameplayAdmission::Ready;
    }
    NativeGameOverState control;uint32_t mode=0;
    if(!ReadNativeControlState(ctx,base,control,mode) || control.registry!=s.registry){s.Stop("Native input control-state query lost world ownership");return RoomGameplayAdmission::Stopped;}
    if(NativeCutsceneControlMode(mode) && !s.cutscene_entry){
        if(!s.started || !s.native_completed || !s.normal_control_mode || s.input_epoch==UINT64_MAX ||
           !(s.input_epoch&1) || !ReadNativeCutsceneContext(ctx,base,s.registry)){
            s.Stop("Native cutscene entry lacks a completed owned input epoch");return RoomGameplayAdmission::Stopped;}
        for(const auto& player:s.roster.players)s.cutscene_schemas[player.peer-1]=s.button_schemas[player.character==LobbyCharacter::Mickey?0:1];
        s.cutscene_entry=*s.native_completed;s.cutscene_resume.reset();++s.input_epoch;
        s.movie_sequence_target=0;s.movie_control_mode=0;
        s.private_skip_source.reset();s.private_skip_armed=false;s.private_skip_failed=false;s.skip_gate={};s.skip_receiver.reset();s.movie_receiver.reset();s.skip_target_available=false;s.skip_target_mode=0;s.skip_target_entry.reset();s.departing_cutscene_buttons.reset();
        REXLOG_INFO("Native Room cutscene input suspended: entry tick {}, input epoch {}; original state {} and native cutscene ownership, actor incarnations unchanged",*s.cutscene_entry,s.input_epoch,mode);
    }
    if(s.cutscene_entry && !NativeCutsceneControlMode(mode)){
        if(!s.normal_control_mode || mode!=*s.normal_control_mode){
            s.Stop("Native cutscene returned to an unowned control mode");return RoomGameplayAdmission::Stopped;}
        std::string selected;
        if(!ReadNativeInputContextName(base,selected) || selected!=s.normal_context_name){
            if(!s.return_wait_start){s.return_wait_start=s.room->next_tick();
                REXLOG_INFO("Native Room cutscene return awaiting selector: source tick {}, expected context '{}'; native mode restored, gameplay source remains retired",*s.return_wait_start,s.normal_context_name);}
            if(s.room->next_tick()-*s.return_wait_start>=120){s.Stop("Native cutscene gameplay selector did not settle within 120 real native updates");return RoomGameplayAdmission::Stopped;}
            s.return_pending=true;
        }else if(!RearmInputContext(s.room->next_tick(),ctx,base)){
            s.Stop("Native cutscene return could not re-admit fresh input bindings");return RoomGameplayAdmission::Stopped;}
    }else if(s.cutscene_entry){s.return_pending=false;s.return_wait_start.reset();}
    if(s.cutscene_entry){
        if(!NativeMovieControlMode(mode) || (s.movie_sequence_target && s.movie_control_mode!=mode)){s.movie_sequence_target=0;s.movie_control_mode=0;}
        if((!ReadNativeCutsceneContext(ctx,base,s.registry,s.movie_sequence_target) && !s.return_pending) || !RefreshOwnedActorTaskPermissions(ctx,base)){
            s.Stop("Native cutscene context or authored permissions changed before suspension frame");return RoomGameplayAdmission::Stopped;}
        if(NativeMovieControlMode(mode) && !s.movie_sequence_target){uint32_t profile=0,object=0,handler=0;
            if(!ReadNativeControlTarget(ctx,base,s.registry,mode,profile,object,handler) || handler!=NativeMovieInputHandler(mode)){
                s.Stop("Native movie sequence lost its validated original target");return RoomGameplayAdmission::Stopped;}
            s.movie_sequence_target=object;s.movie_control_mode=mode;
            REXLOG_INFO("Native Room movie sequence target retained: tick {} object {:08X}; native movie state {}, no gameplay callbacks",s.room->next_tick(),object,mode);
        }
        TracePrivateNativeCutsceneUi(s.input_epoch,ctx,base,s.registry);
        const bool target_available=!s.return_pending && ((mode==6 && NativeCutsceneSkipTargetAvailable(s.registry,ctx,base)) ||
            (mode==2 && NativeMovieAcceptTargetAvailable(s.registry,ctx,base)));
        if(target_available!=s.skip_target_available || (target_available && s.skip_target_mode!=mode)){
            s.skip_receiver.reset();s.movie_receiver.reset();s.skip_gate={};s.skip_target_available=target_available;s.skip_target_mode=target_available?mode:0;
            s.skip_target_entry=target_available?s.native_completed:std::nullopt;
        }
        if(s.prepared){output=*s.prepared;return RoomGameplayAdmission::Ready;}
        const auto tick=s.room->next_tick();
        if(!s.submitted){Pad sampled;
            if(tick>UINT64_MAX-s.identity.input_delay || s.room->next_submit_tick()!=tick+s.identity.input_delay ||
               !em2::SampleLocalControls(*s.input,s.physical,tick,sampled)){
                s.Stop("Native cutscene source could not poll its owned device");return RoomGameplayAdmission::Stopped;}
            const bool down=s.skip_gate.Sample(bool(sampled.buttons&0x1000));
            const bool available=s.skip_target_available;
            const auto command=available?NativeCutsceneUiCommand(s.input_epoch,down):NativeCutsceneCommand(s.input_epoch);
            if(!s.room->SubmitLocal(s.room->next_submit_tick(),command)){
                s.Stop("Native cutscene source could not submit typed UI intent");return RoomGameplayAdmission::Stopped;}
            s.departing_cutscene_buttons=sampled.buttons;
            s.ProbeSkip(sampled,tick,ctx,base);
            s.submitted=tick;
        }
        if(*s.submitted!=tick){s.Stop("Native cutscene source crossed native ticks");return RoomGameplayAdmission::Stopped;}
        RoomFrame frame;if(!s.room->PeekFrame(frame))return RoomGameplayAdmission::Waiting;
        if(frame.tick!=tick || !s.CutsceneFrame(frame,ctx,base)){
            s.Stop("Native cutscene authoritative frame failed input-epoch admission");return RoomGameplayAdmission::Stopped;}
        s.prepared=frame;s.prepared_passive=true;output=frame;gameplay_waiting.store(false,std::memory_order_release);return RoomGameplayAdmission::Ready;
    }
    s.normal_control_mode=mode;
    if(!RefreshOwnedActorTaskPermissions(ctx,base)){s.Stop("Could not refresh authored extra-human ability policy");return RoomGameplayAdmission::Stopped;}
    if(s.prepared){output=*s.prepared;return RoomGameplayAdmission::Ready;}
    const auto tick=s.room->next_tick();
    if(s.scripted_input && s.first_game_tick && !s.history_probe_done){
        const auto verified=s.room->verified_gameplay_tick();
        if(verified && *verified>=*s.first_game_tick && *verified-*s.first_game_tick>=511){
            const auto scope=s.room->gameplay_history_scope();const auto first=*s.first_game_tick+288,last=*s.first_game_tick+511;std::string fingerprint;
            if(!scope || s.room->FingerprintGameplayHistory(*scope,first,last,fingerprint)!=RoomHistoryResult::Ready){s.Stop("Private native consumed-input history verification failed");return RoomGameplayAdmission::Stopped;}
            s.history_probe_done=true;
            REXLOG_INFO("Native Room verified input history: local peer {} first {} last {} frames 224 mask {:04X} sha256 {}; exact consumed/completed/verified commands, no native replay",s.peer,first,last,s.roster_mask,fingerprint);
        }
    }
    if(!s.first_game_tick){
        s.first_game_tick=tick;
        auto frames=std::make_unique<OwnedMovementFrames>();
        std::array<uint64_t,kOnlineRoomCapacity> epochs{};for(const auto& player:s.roster.players)epochs[player.peer-1]=1;
        if(!frames->Configure(s.roster_mask,3,epochs,tick,s.registry,ValidateRetailMovementFrame) || !s.actors->BindFrames(*frames,ctx,base)){
            s.Stop("Could not configure the native actor frame transaction");return RoomGameplayAdmission::Stopped;
        }
        if(s.native_buttons)for(const auto& actor:s.actors->actors()){
            if(!frames->BindButtons(actor.room_peer,s.button_definitions[actor.actor.action_user],ctx,base)){
                s.Stop("Could not bind native button definitions to each extra actor");return RoomGameplayAdmission::Stopped;
            }
        }
        s.actor_frames=std::move(frames);
    }
    if(REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_probe_room_triggers) && !s.button_receiver_probe_done){
        for(unsigned slot=0;slot<2;++slot){
            RoomMovement neutral;neutral.actor_epoch=1;neutral.source=RoomMovementSource::NativeActionV3;neutral.jump_button_mask=0x1000;
            neutral.button_actions.schema=s.button_schemas[slot];neutral.button_actions.source_tick=tick;
            std::vector<NativeActorAction> mapped;
            const bool valid=s.button_receivers[slot] && PrepareRetailButtons(*s.button_receivers[slot],neutral,ctx,base,mapped) && mapped.empty();
            REXLOG_INFO("Private native button receiver P{}: independent owner preflight {}, current local producer P{}; synthetic neutral metadata only, no dispatch/submit/vote",slot,valid,s.slot);
        }
        s.button_receiver_probe_done=true;
    }
    // A co-op join can leave the native Start history down at readiness. A
    // bounded local retry observes later natural settling; it never votes ready,
    // polls a device, changes a history or dispatches an action.
    if(REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_probe_room_triggers) &&
       !(s.trigger_probe_mask&(1u<<s.slot)) && s.trigger_probe_attempts[s.slot]<4 && s.trigger_probe_tick[s.slot]!=tick){
        ++s.trigger_probe_attempts[s.slot];
        s.trigger_probe_tick[s.slot]=tick;
        if(ProbeTriggerMath(s.slot,s.movement,tick,ctx,base))s.trigger_probe_mask|=1u<<s.slot;
    }
    if(!s.submitted){
        if(tick>UINT64_MAX-s.identity.input_delay || s.room->next_submit_tick()!=tick+s.identity.input_delay){
            s.Stop("Gameplay source/delayed transport timelines do not match");return RoomGameplayAdmission::Stopped;
        }
        NativeRoutedPadSample sample;RoomMovement command;NativeLocalJumpEvents edges;
        if(!SampleNativeLocalSource(*s.room,*s.input,tick,ctx,base,sample) ||
            !ProduceNativeLocalRoomMovement(*s.room,s.movement,sample,1,ctx,base,command) ||
            !ProduceNativeLocalJump(*s.room,s.jump,sample,s.input_epoch,ctx,base,edges)){
            s.Stop("The live local source could not produce original native Movement/Jump");return RoomGameplayAdmission::Stopped;
        }
        command.input_epoch=s.input_epoch;command.source=RoomMovementSource::NativeActionV2;command.jump_button_mask=edges.button_mask;
        command.jump_count=edges.count;command.jump_events=edges.events;
        if(s.peer>2 && !s.actors->ViewBasis(s.peer,ctx,base,command.forward_x,command.forward_z)){
            s.Stop("Local extra-human native camera reference is unavailable");return RoomGameplayAdmission::Stopped;
        }
        bool local_valid=false;
        if(s.native_buttons){NativeTriggerBatch buttons;std::vector<NativeActorAction> mapped;
            if(!ProduceNativeTriggers(*s.room,*s.button_source,s.button_binding,sample,s.input_epoch,ctx,base,buttons) ||
               !EncodeNativeButtonActions(s.button_binding,buttons,command.button_actions)){
                s.Stop("The sealed local sample could not produce native button decisions");return RoomGameplayAdmission::Stopped;}
            command.source=RoomMovementSource::NativeActionV3;
            // The outgoing source is ahead of received input by input_delay;
            // receiver history applies only to the authoritative echo below.
            local_valid=PrepareRetailButtons(*s.button_receivers[s.slot],command,ctx,base,mapped);
        }else local_valid=ValidateRetailMovement(s.slot,command,ctx,base)==ActorInputResult::Applied;
        if(!local_valid ||
            !s.room->SubmitLocal(s.room->next_submit_tick(),command)){
            s.Stop("Local intent is unsupported or transport rejected its native command");return RoomGameplayAdmission::Stopped;
        }
        s.submitted=tick;
    }
    if(*s.submitted!=tick){s.Stop("A cached physical sample crossed native ticks");return RoomGameplayAdmission::Stopped;}
    RoomFrame frame;if(!s.room->PeekFrame(frame))return RoomGameplayAdmission::Waiting;
    if(s.cutscene_resume && tick<=*s.cutscene_resume+s.identity.input_delay){
        if(frame.tick!=tick || !s.CutsceneFrame(frame,ctx,base)){
            s.Stop("Native cutscene return delayed prefix changed before gameplay re-admission");return RoomGameplayAdmission::Stopped;}
        s.prepared=frame;s.prepared_passive=true;output=frame;gameplay_waiting.store(false,std::memory_order_release);return RoomGameplayAdmission::Ready;
    }
    if(frame.tick!=tick || frame.players_mask!=s.roster_mask ||
        !s.actor_frames || s.actor_frames->Preflight(frame,ctx,base)!=OwnedFrameResult::Ready ||
        !s.ButtonFrame(frame,ctx,base) || !s.Actions(frame,ctx,base)){
        s.Stop("Received retail frame failed complete native preflight");return RoomGameplayAdmission::Stopped;
    }
    s.prepared=frame;output=frame;gameplay_waiting.store(false,std::memory_order_release);return RoomGameplayAdmission::Ready;
}
bool NativeRoomGameplay::Commit(const RoomFrame& frame,PPCContext& ctx,uint8_t* base){
    Environment environment;Context context(ctx);auto& s=*impl_;Impl::FaultOnThrow fault{s};
    if(!s.Owner() || s.stopped || s.pending || !s.prepared || frame!=*s.prepared)return false;
    if(!s.Canonical() || !s.World(base) || (s.game_over_entry?!s.GameOverFrame(frame,ctx,base):s.prepared_passive?!s.CutsceneFrame(frame,ctx,base):
        (!s.actor_frames || s.actor_frames->Preflight(frame,ctx,base)!=OwnedFrameResult::Ready ||
        !s.ButtonFrame(frame,ctx,base) || !s.Actions(frame,ctx,base))))return s.Stop("Native retail admission changed before frame consumption");
    RoomFrame actual;
    if(!s.room->PeekFrame(actual) || actual!=frame || !s.room->TakeFrame(actual) || actual!=frame)return s.Stop("The authoritative gameplay frame changed before consumption");
    Frame retail;retail.tick=frame.tick;
    // Keep delayed commands in history without routing them to dead actors.
    if(!s.game_over_entry && !s.prepared_passive){retail.players[0]=frame.players[0].pad;retail.players[1]=frame.players[1].pad;}
    uint64_t generation=0;Frame published;
    if(em2::SynchronizedControlsGeneration()!=s.controls || !em2::PublishSynchronizedControls(retail) ||
        !em2::ReadPublishedSynchronizedControls(generation,published) || generation!=s.controls || published!=retail)
        return s.Stop("Could not publish the authoritative retail gameplay controls");
    if(!s.started)REXLOG_INFO("Native Room gameplay first frame consumed: tick {}, delay {}, physical source {}, native slot {}; native update and world verification still required",
        frame.tick,s.identity.input_delay,s.physical,s.slot);
    s.consumed_passive=s.prepared_passive;s.prepared_passive=false;
    s.committed=frame.tick;s.consumed=frame;s.pending=true;s.delivered=false;s.started=true;s.prepared.reset();s.submitted.reset();return true;
}
bool NativeRoomGameplay::Deliver(PPCContext& ctx,uint8_t* base){
    Environment environment;Context context(ctx);auto& s=*impl_;Impl::FaultOnThrow fault{s};uint64_t tick=0;
    if(!s.Owner() || s.stopped || !s.pending || s.delivered || !s.consumed ||
        !em2::CurrentAgreedSimulationTick(tick) || tick!=s.committed)return false;
    if(const auto reason=s.WorldFailure(base))return s.Stop(std::string("Native world changed before input delivery: ")+reason);
    if(s.game_over_entry || s.consumed_passive){
        if(s.game_over_entry?!s.GameOverFrame(*s.consumed,ctx,base):!s.CutsceneFrame(*s.consumed,ctx,base))return s.Stop("Native no-gameplay input phase changed before native update");
        if(!BeginRetailPassiveTick(s.registry,tick,ctx,base))return s.Stop("Could not retain retail quarantine for native GameOver");
        if(!s.game_over_entry && s.cutscene_entry && !s.return_pending && s.skip_target_available && s.skip_target_entry && tick>*s.skip_target_entry+s.identity.input_delay){
            const bool ui=std::any_of(s.consumed->players.begin(),s.consumed->players.end(),[](const auto& c){return c.source==RoomMovementSource::NativeCutsceneUiV6;});
            if(ui || s.skip_receiver || s.movie_receiver){
                if(s.skip_target_mode==2){
                    if(!s.movie_receiver)s.movie_receiver=std::make_unique<NativeMovieAcceptReceiver>();
                    if(!s.movie_receiver->Consume(*s.consumed,s.registry,s.input_epoch,ctx,base))return s.Stop("Native movie Accept could not deliver its agreed frame to the original handler");
                }else{
                    if(!s.skip_receiver)s.skip_receiver=std::make_unique<NativeCutsceneSkipReceiver>();
                    if(!s.skip_receiver->Consume(*s.consumed,s.registry,s.input_epoch,ctx,base))return s.Stop("Native cutscene UI could not deliver its agreed frame to the original handler");
                }
            }
        }
        s.delivered=true;return true; // Character gameplay callbacks remain quarantined.
    }
    if(!s.Actions(*s.consumed,ctx,base))return s.Stop("Native retail action bindings changed before delivery");
    if(!s.ButtonFrame(*s.consumed,ctx,base))return s.Stop("Native retail button frame failed delivery admission");
    if(!s.actor_frames)return s.Stop("Native owned actor frame coordinator is missing at delivery");
    const auto owned=s.actor_frames->Deliver(*s.consumed,ctx,base);
    if(owned!=OwnedFrameResult::Ready)return s.Stop("Native owned actor frame delivery failed (code "+std::to_string(unsigned(owned))+")");
    if(!BeginRetailBasisTick(s.registry,tick,ctx,base))return s.Stop("Native retail input clock or ownership changed before delivery");
    for(unsigned slot=0;slot<2;++slot)if(PrepareRetailMovement(slot,s.consumed->players[slot],tick,ctx,base)!=ActorInputResult::Applied)
        return s.Stop("Could not stage each retail actor's transmitted movement reference");
    for(unsigned slot=0;slot<2;++slot){
        const auto action=s.Action(slot,s.consumed->players[slot]);
        if(ReplayRetailMovement(slot,action,tick,ctx,base)!=ActorInputResult::Applied)return s.Stop("Original native retail Movement delivery failed");
    }
    const bool buttons=s.consumed->players[0].source==RoomMovementSource::NativeActionV3;
    for(unsigned slot=0;slot<2;++slot){
        const auto result=buttons?ReplayRetailButtonBatch(slot,s.RetailFrame(*s.consumed),ctx,base):ReplayRetailJumpBatch(slot,s.consumed->players[slot],tick,ctx,base);
        if(result!=ActorInputResult::Applied)return s.Stop("Original native retail button/Jump delivery failed");
    }
    if(buttons && (++s.button_frames==1 || s.button_frames%256==0))
        REXLOG_INFO("Native Room V3 button frame delivered: tick {}, source ticks {}/{}, frames {}; original character callbacks, no second Jump replay",
            tick,s.consumed->players[0].button_actions.source_tick,s.consumed->players[1].button_actions.source_tick,s.button_frames);
    const auto injected_tick=REXCVAR_GET(em2_probe_room_partial_delivery_tick);
    if(injected_tick!=0xFFFFFFFFu && tick==injected_tick && REXCVAR_GET(em2_private_profile) && NativeOnlineMenuQaActive()){
        REXLOG_INFO("Private native Room partial delivery injected: tick {}; original actor/retail inputs delivered, native update not entered",tick);
        return s.Stop("Private native Room partial-delivery fault injection");
    }
    s.delivered=true;return true;
}
bool NativeRoomGameplay::Complete(const std::array<uint8_t,32>& digest,PPCContext& ctx,uint8_t* base){
    Environment environment;Context context(ctx);auto& s=*impl_;Impl::FaultOnThrow fault{s};uint64_t tick=0;
    if(!s.Owner() || s.stopped || !s.pending || !s.delivered || !s.consumed ||
        !em2::CurrentAgreedSimulationTick(tick) || tick!=s.committed)return false;
    if(const auto failure=s.WorldFailure(base))return s.Stop(std::string("Native gameplay completion rejected: ")+failure);
    if(s.game_over_entry || s.consumed_passive){
        if(!FinishRetailPassiveTick(tick,ctx,base))return s.Stop("Native GameOver passive quarantine completion failed");
    }else{
        const auto finished=s.consumed->players[0].source==RoomMovementSource::NativeActionV3?
            FinishRetailButtonTick(tick,ctx,base):FinishRetailJumpTick(tick,ctx,base);
        if(finished!=ActorInputResult::Applied)return s.Stop("Native retail button/jump completion rejected (result "+std::to_string(int(finished))+")");
        if(!s.actor_frames)return s.Stop("Native owned actor frame coordinator is missing at completion");
        NativeGameOverState control;uint32_t mode=0;
        const bool movie_entry=!s.cutscene_entry && ReadNativeControlState(ctx,base,control,mode) && NativeMovieControlMode(mode) &&
            control.registry==s.registry && ReadNativeCutsceneContext(ctx,base,s.registry);
        if(movie_entry){uint8_t retail_consumed=0;uint16_t extra_consumed=0;
            if(!FinishRetailMovieEntryTick(tick,ctx,base,retail_consumed))return s.Stop("Native movie entry could not close its delivered retail input tick");
            const auto owned_finished=s.actor_frames->FinishMovieEntry(tick,ctx,base,extra_consumed);
            if(owned_finished!=OwnedFrameResult::Ready)return s.Stop("Native movie entry could not close its owned actor tick (result "+std::to_string(int(owned_finished))+")");
            REXLOG_INFO("Native Room movie entry interrupted update: tick {} actual retail Motion mask {:02X} extra mask {:04X}; original world pause, omitted work not replayed or marked consumed",tick,retail_consumed,extra_consumed);
        }else{
            if(!FinishRetailInputTick(tick,ctx,base))return s.Stop("Native retail movement tick was not completed by its current receiver");
            const auto owned_finished=s.actor_frames->Finish(tick,base);
            if(owned_finished!=OwnedFrameResult::Ready)return s.Stop("Native owned actor completion rejected (result "+std::to_string(int(owned_finished))+")");
        }
    }
    if(s.game_over_entry && (tick==*s.game_over_entry+1 || tick%30==0))
        REXLOG_INFO("Native Room GameOver native update completed: tick {}; original world update and real digest, no gameplay callbacks dispatched",tick);
    if(s.consumed_passive && tick%60==0)REXLOG_INFO("Native Room cutscene passive update completed: tick {}, input epoch {}; real native update/digest and retained quarantine, no gameplay input dispatch",tick,s.input_epoch);
    ObservePrivateRoomActors(tick,s.actors->actors(),base);
    ObservePrivateExtraDamage(tick,ctx,base);
    // The original post-update and phase-specific completion succeeded.
    // GameOver promises no gameplay callbacks. Proof consensus may still fail.
    // This is only a physical update boundary, never authoritative agreement.
    s.native_completed=tick;
    if(!s.room->CompleteGameplay(tick,digest))return s.Stop("Native gameplay world verification failed");
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
bool NativeRoomGameplay::SampleRecoveryButtons(uint16_t& buttons)const{
    Environment environment;const auto& s=*impl_;buttons=0;
    if(!s.Configured() || !s.Owner() || (s.room->state()!=RoomState::Error && s.room->state()!=RoomState::Disconnected) ||
       em2::InAgreedSimulationTick())return false;
    auto* input=s.original_input;
    if(REXCVAR_GET(em2_probe_room_recovery_controller) && REXCVAR_GET(em2_private_profile) && NativeOnlineMenuQaActive() && s.scripted_input)
        input=s.scripted_input.get();
    return input && em2::SampleLocalRecoveryButtons(*input,s.physical,buttons);
}
const char* NativeRoomGameplay::LeaveBoundaryBlocker(uint8_t* base)const{
    Environment environment;const auto& s=*impl_;
    if(!s.Configured() || !s.Owner())return "native gameplay owner unavailable";
    if(em2::InAgreedSimulationTick())return "native update is still executing";
    if(s.native_unwind)return "native transaction unwound";
    if(!s.started || !s.native_completed || *s.native_completed!=s.committed)return "native input or post-update completion is unavailable";
    if(s.transition_pending || s.load_depth)return "native world transition is unfinished";
    if(!s.Canonical())return "native room identity or roster changed";
    return s.WorldFailure(base);
}
std::unique_ptr<NativeRoomActors> NativeRoomGameplay::PrepareTitleUnload(PPCContext& ctx,uint8_t* base){
    Environment environment;Context context(ctx);auto& s=*impl_;Impl::FaultOnThrow fault{s};
    if(LeaveBoundaryBlocker(base) || !s.actors || !s.actors->PrepareWorldUnload(ctx,base))return {};
    return std::move(s.actors);
}
std::optional<uint64_t> NativeRoomGameplay::last_native_completed_tick()const{
    return impl_->Owner()?impl_->native_completed:std::nullopt;
}
const std::string& NativeRoomGameplay::error()const{return impl_->error;}
NativeRoomGameplay* CurrentNativeRoomGameplay()noexcept{return current_gameplay;}
bool NativeRoomRenderBindingsAvailable(uint8_t* base){if(NativeRoomTitleLeaveRenderRetired())return false;const auto* game=CurrentNativeRoomGameplay();return !game || game->RenderBindingsAvailable(base);}
bool NativeRoomGameplayReadiness(PPCContext& ctx,uint8_t* base){return current_gameplay && current_gameplay->Readiness(ctx,base);}
bool NativeRoomGameplayHalted()noexcept{return gameplay_halted.load(std::memory_order_acquire);}
bool NativeRoomGameplayWaiting()noexcept{return gameplay_waiting.load(std::memory_order_acquire);}
uint32_t NativeRoomLocalCameraController(){auto* gameplay=CurrentNativeRoomGameplay();return gameplay?gameplay->LocalCameraController():0;}
bool SuppressNativeRoomCameraLook(PPCContext& ctx,uint8_t* base){auto* gameplay=CurrentNativeRoomGameplay();
    if(!gameplay || !gameplay->OwnsCameraMode(ctx.r3.u32,base))return false;ctx.r3.u64=0;return true;}
bool NativeRoomOwnsCameraMode(uint32_t mode,uint8_t* base){auto* gameplay=CurrentNativeRoomGameplay();return gameplay && gameplay->OwnsCameraMode(mode,base);}
}
PPC_FUNC_IMPL(sub_82A14BF8){
    const auto arguments=ctx;__imp__sub_82A14BF8(ctx,base);
    if(auto* gameplay=em2::net::CurrentNativeRoomGameplay()){
        struct Restore{PPCContext& ctx;PPCContext saved;unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Restore(){ctx=saved;_mm_setcsr(csr);SetLastError(error);}}restore{ctx,ctx};
        ctx=arguments;gameplay->UpdateExtraCameras(ctx,base);
    }
}

PPC_FUNC_IMPL(sub_82A39B48){
    // The ordinary camera's near-target visibility update can run between
    // native unload and fresh admission. Its hide helper dereferences the
    // target's NiPointer without a null check (82A37D28); that model may
    // already be gone even though the retained character entity still exists.
    // Defer the whole camera-originated visibility transaction, including its
    // cached flag at +664. Skipping only the hide helper would mark that flag
    // complete without changing the model, preventing a retry after admission.
    // The rest of the original camera update still runs, allowing the new
    // entrance to activate its normal controller.
    const auto saved_error=GetLastError();const auto saved_csr=_mm_getcsr();
    const bool defer=ctx.lr==0x82A14F44 && !em2::net::NativeRoomRenderBindingsAvailable(base);
    SetLastError(saved_error);_mm_setcsr(saved_csr);
    if(!defer)__imp__sub_82A39B48(ctx,base);
}

// The native current-health HUD helper resolves a ScriptEntity here before
// querying its authored Health component. Replace only that read-only lookup,
// for the local extra human's matching character widget. No PlayerManager
// record, shared ammo pool, health value or simulation identity is changed.
PPC_EXTERN_FUNC(__imp__sub_82A2F6B8);
PPC_EXTERN_FUNC(__imp__sub_82A2F708);
namespace {
void ObserveNativeRoomHudLife(unsigned profile,PPCContext& ctx,uint8_t* base){
    const auto error=GetLastError();const auto csr=_mm_getcsr();
    struct RestoreEnvironment{DWORD error;unsigned csr;~RestoreEnvironment(){SetLastError(error);_mm_setcsr(csr);}}restore{error,csr};
    if(!REXCVAR_GET(em2_probe_room_hud_properties) || !REXCVAR_GET(em2_private_profile) || !em2::net::NativeRoomActive())return;
    auto* gameplay=em2::net::CurrentNativeRoomGameplay();
    const auto target=gameplay?gameplay->LocalHudActor(profile,ctx,base):0;
    const auto load=em2::CurrentNativeLoadGeneration();
    const auto key=(load<<3)|(uint64_t(bool(gameplay))<<2)|(uint64_t(bool(target))<<1)|uint64_t(em2::NativeResourceObservationActive());
    static thread_local std::array<uint64_t,2> previous={UINT64_MAX,UINT64_MAX};
    if(previous[profile]!=key){previous[profile]=key;REXLOG_INFO("Native HUD life callback observed: profile {} load {} native owner {} local target available {} canonical read {}",profile,load,bool(gameplay),bool(target),em2::NativeResourceObservationActive());}
}
}
PPC_FUNC_IMPL(sub_82A2F6B8){__imp__sub_82A2F6B8(ctx,base);ObserveNativeRoomHudLife(0,ctx,base);}
PPC_FUNC_IMPL(sub_82A2F708){__imp__sub_82A2F708(ctx,base);ObserveNativeRoomHudLife(1,ctx,base);}
bool em2::net::ReadNativeRoomHudLife(unsigned profile,PPCContext& ctx,uint8_t* base,double& value){
    Environment environment;Context preserve(ctx);
    if(profile>1 || !base || ctx.r1.u32<1024 || em2::NativeResourceObservationActive())return false;
    auto* game=CurrentNativeRoomGameplay();
    if(!game || !game->LocalHudActor(profile,ctx,base))return false;
    // Invoke the authored numeric getter only because the actual GFx status
    // widget is requesting its current value. The existing read-only entity
    // lookup selects the admitted actor; no PlayerManager record is replaced.
    const auto stack=ctx.r1.u32-256,output=stack+64;
    PPC_STORE_U32(stack,ctx.r1.u32);ctx.r1.u64=stack;ctx.r3.u64=output;
    if(profile)sub_82A2F708(ctx,base);else sub_82A2F6B8(ctx,base);
    if(PPC_LOAD_U32(output)!=2)return false;
    const auto bits=PPC_LOAD_U64(output+8);std::memcpy(&value,&bits,sizeof(value));
    return std::isfinite(value) && value>=0;
}
PPC_FUNC_IMPL(sub_8276B878){
    const auto caller=ctx.lr;const auto manager=ctx.r3.u32;const auto profile=ctx.r4.u32;
    __imp__sub_8276B878(ctx,base);
    if(caller!=0x82A2EBF4 || profile>1 || em2::NativeResourceObservationActive() || manager!=PPC_LOAD_U32(0x833DAB60))return;
    if(auto* gameplay=em2::net::CurrentNativeRoomGameplay()){
        const auto retail=ctx.r3.u32;
        if(const auto entity=gameplay->LocalHudActor(profile,ctx,base)){
            ctx.r3.u64=entity;
            static thread_local uint32_t reported=0;
            if(reported!=entity){const auto error=GetLastError();const auto csr=_mm_getcsr();
                REXLOG_INFO("Native Room HUD health target: profile {} retail {:08X} local extra {:08X}; original current-life property query",profile,retail,entity);
                SetLastError(error);_mm_setcsr(csr);reported=entity;}
        }
    }
}

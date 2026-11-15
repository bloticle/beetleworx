#include "gpu_sync.h"
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
#include "native_character_physics.h"
#include "native_entity_probe.h"
#include "native_entity_lifetime.h"
#include "native_actor_health.h"
#include "native_entity_retirement.h"
#include "native_damage_event.h"
#include "native_component_schema.h"
#include "native_random.h"
#include "native_projectile_impact.h"
#include <rex/logging.h>
#include <rex/input/input_system.h>
#include <windows.h>
#include <immintrin.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <bit>
#include <thread>
#include <optional>
#include <exception>
#include <vector>
#include <set>
#include <rex/cvar.h>
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DEFINE_BOOL(em2_probe_room_authority_state,false,"Debug","Private native Room: publish real completed player positions/velocities over the host-state channel; does not apply corrections or disable proofs");
REXCVAR_DEFINE_BOOL(em2_probe_room_authority_drift,false,"Debug","Private correction-mode guest only: inject one player/world position error and one player health error, then require native host-state repair");
REXCVAR_DEFINE_BOOL(em2_probe_room_player_damage_events,false,"Debug","Private native Room: transmit causal root player Damage events and replay exact native payloads before guest updates");
REXCVAR_DEFINE_BOOL(em2_probe_room_world_damage_events,false,"Debug","Private native Room: extend native Damage event replication and local suppression to world entities; lifecycle support remains experimental");
REXCVAR_DEFINE_BOOL(em2_probe_room_ai_targets,false,"Debug","Private native Room: host owns non-human native brain target-selection decisions");
REXCVAR_DEFINE_BOOL(em2_probe_room_ai_target_drift,false,"Debug","Private AI authority fixture: only the host requests Mickey as an NPC target during ticks 120-179");
REXCVAR_DEFINE_BOOL(em2_probe_room_spawn_batches,false,"Debug","Private native Room: transport and compare complete native spawner batches; no spawn replay or suppression");
REXCVAR_DEFINE_BOOL(em2_probe_room_spawn_eligibility,false,"Debug","Private spawn authority: suppress guest-only operations at the audited null-safe projectile caller; requires batch capture");
REXCVAR_DEFINE_BOOL(em2_probe_room_spawn_replay,false,"Debug","Private spawn authority: replay missing Shooter preparation and pending-shot setup; requires batch capture and eligibility");
REXCVAR_DECLARE(std::string,em2_room_probe_role);
REXCVAR_DEFINE_BOOL(em2_probe_room_collection_drift,false,"Debug","Isolated native Room QA only: omit one matching Lua collection birth at tick 800 or later to require native host replay");
REXCVAR_DEFINE_BOOL(em2_probe_room_entity_retirements,false,"Debug","Private authority: transmit confirmed native NPC removals and replay original retirement/flush on the guest");
REXCVAR_DEFINE_BOOL(em2_probe_room_projectile_impacts,false,"Debug","Private authority: host-owned impact/retirement and native FX replay for captured Shooter projectiles");
REXCVAR_DEFINE_BOOL(em2_probe_room_shooter_launches,false,"Debug","Private authority: host-owned pending-shot launch timing, native velocity setup and launch FX");
PPC_EXTERN_FUNC(__imp__sub_828B92A8);
PPC_EXTERN_FUNC(sub_827B3C60);
PPC_EXTERN_FUNC(__imp__sub_832212E8);
PPC_EXTERN_FUNC(__imp__sub_827F2FA8);
REXCVAR_DECLARE(bool,em2_profile_world_memory);
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
uint32_t NamedComponent(uint8_t* base,uint32_t entity,const char* wanted){
    if(!Readable(entity,96,base) || !Readable(0x833DBAF0,1,base))return 0;
    const auto count=PPC_LOAD_U8(0x833DBAF0);const auto types=PPC_LOAD_U32(entity+88),items=PPC_LOAD_U32(entity+36);
    if(!count || !Readable(types,count,base))return 0;uint32_t found=0;
    for(unsigned kind=0;kind<count;++kind){const auto slot=PPC_LOAD_U8(types+kind);if(slot==255)continue;
        if(!Readable(items,size_t(slot)*4+4,base))return 0;const auto face=PPC_LOAD_U32(items+slot*4);std::string name;
        if(face<8 || !em2::ReadNativeComponentTypeName(base,face-8,name) || name!=wanted)continue;
        if(found && found!=face-8)return 0;found=face-8;
    }return found;
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
    bool startup_cutscene=false;
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
    std::optional<uint64_t> game_over_entry,host_game_over_entry;
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
    std::optional<RoomAuthorityState> pending_authority;
    std::vector<DamageEvent> damage_events;
    std::vector<RoomAiTargetDecision> ai_decisions;
    std::vector<SpawnBatch> spawn_batches;
    bool spawn_replaying=false,collection_drift_injected=false;
    std::vector<ProjectileImpact> projectile_impacts;
    std::set<SpawnId> owned_projectiles;
    std::set<SpawnId> authority_entities,retirements_applied;
    uint64_t projectile_load=0;
    bool impacts_applied=false;
    std::vector<ShooterLaunch> shooter_launches;
    size_t launch_cursor=0;
    size_t ai_cursor=0;
    bool stopped=false,pending=false,delivered=false,started=false,ready_logged=false;
    bool authority_drift_injected=false;
    bool authority_world_drift_injected=false;
    bool authority_health_drift_injected=false;
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
    uint8_t HumanPeer(uint32_t entity)const{
        uint64_t result=0;
        for(unsigned slot=0;slot<2;++slot)if(entity && entity==label_entities[slot])result=slot+1;
        if(!result)for(const auto& actor:actors->actors())if(actor.actor.entity==entity){result=actor.room_peer;break;}
        if(!result || result>16 || std::none_of(roster.players.begin(),roster.players.end(),[&](const auto& player){return player.peer==result;}))return 0;
        return uint8_t(result);
    }
    bool AnnouncedLoadCancellation(uint64_t tick)const{
        // The host is blocked inside the real loader before completing this
        // update, so no completed combat packet exists. Its authenticated,
        // tick-scoped notice admits the guest's original path to the same
        // loader rendezvous. It does not authorize completing this update.
        return Owner() && Configured() && !stopped && peer!=1 && pending && delivered &&
            tick==committed && !pending_authority && room->HostLoadAnnounced(tick);
    }
    bool AiScope(uint32_t brain,uint8_t* base,std::array<uint32_t,4>& id,uint64_t& tick){
        if(!REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_probe_room_ai_targets) || !Owner() || !Configured() || stopped ||
           !pending || !delivered || load_depth || transition_pending || !room->host_correction_mode() ||
           room->state()!=RoomState::Running || !em2::CurrentAgreedSimulationTick(tick) || tick!=committed || AnnouncedLoadCancellation(tick))return false;
        if(!Readable(brain,296,base))return Stop("Native AI target brain unavailable");
        const auto attached=PPC_LOAD_U32(brain+60);
        if(!attached || uint64_t(attached)+28>0x100000000ull || !Readable(attached+8,20,base) || PPC_LOAD_U32(attached+8)!=0x8208D908)
            return Stop("Native AI target owner layout unsupported");
        if(HumanPeer(attached+8))return false;
        for(unsigned i=0;i<4;++i)id[i]=PPC_LOAD_U32(attached+12+i*4);
        if(std::all_of(id.begin(),id.end(),[](auto word){return word==0;}))return Stop("Native AI target owner has no identity");
        return true;
    }
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
    if(ReadNativeControlState(ctx,base,control,mode) && NativeCutsceneControlMode(mode) && control.registry==s.registry &&
       ReadNativeCutsceneContext(ctx,base,s.registry,s.movie_sequence_target)){
        if(tick%60==0)REXLOG_INFO("Native Room authored camera retained: tick {}; original cutscene view, no independent gameplay look or local view replacement",tick);
        return;
    }
    const auto manager=PPC_LOAD_U32(0x833DAB60);
    if(!Readable(manager,352,base) || ctx.r3.u32!=PPC_LOAD_U32(manager+348) || s.camera_tick==tick)return;
    s.camera_tick=tick;
    if(!s.consumed || !s.actors->UpdateCameras(*s.consumed,ctx,base) || !BindNativeRoomLocalView(ctx,base))s.Stop("Independent native camera update or local view binding failed");
}
bool NativeRoomGameplay::ReadMovieClock(uint32_t manager,PPCContext& ctx,uint8_t* base,uint64_t& tick)const{
    Environment environment;Context context(ctx);const auto& s=*impl_;
    if(!s.Owner() || s.stopped || !s.started || !s.World(base) || !s.Canonical() ||
       s.room->state()!=RoomState::Running || !Readable(0x833DCCB8,4,base) ||
       PPC_LOAD_U32(0x833DCCB8)!=manager || !Readable(manager,132,base))return false;
    NativeGameOverState control;uint32_t mode=0;
    if(!ReadNativeControlState(ctx,base,control,mode) || !NativeMovieControlMode(mode) || control.registry!=s.registry ||
       !ReadNativeCutsceneContext(ctx,base,s.registry,s.movie_sequence_target))return false;
    uint64_t current=0;
    if(em2::CurrentAgreedSimulationTick(current)){
        if(!s.pending || !s.delivered || current!=s.committed)return false;
    }else{
        if(!s.native_completed)return false;
        current=*s.native_completed;
    }
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
    finished=true;s.startup_cutscene=false;s.actor_frames=std::move(frames);s.input_epoch=next_epoch;s.cutscene_resume=*s.native_completed;s.cutscene_entry.reset();s.movie_sequence_target=0;s.movie_control_mode=0;s.return_pending=false;s.return_wait_start.reset();
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
        s.game_over_entry=s.peer!=1 && s.host_game_over_entry?s.host_game_over_entry:s.native_completed;
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
            NativeGameOverState observed;uint32_t observed_mode=0;
            const bool observed_valid=ReadNativeControlState(ctx,base,observed,observed_mode);
            REXLOG_ERROR("Native GameOver phase failure: tick {} frame {} entry {} delay {} input epoch {}, native valid {} mode {} dead {} registry {:08X}/{:08X}",
                tick,frame.tick,*s.game_over_entry,s.identity.input_delay,s.input_epoch,observed_valid,observed_mode,observed.dead,observed.registry,s.registry);
            for(unsigned peer=0;peer<kOnlineRoomCapacity;++peer)if(frame.players_mask&(1u<<peer)){
                const auto& command=frame.players[peer];
                REXLOG_ERROR("Native GameOver rejected command: peer {} source {} epoch {} actor {} valid {} source tick {} schema-match {}",
                    peer+1,unsigned(command.source),command.input_epoch,command.actor_epoch,ValidRoomMovement(command),command.button_actions.source_tick,command.button_actions.schema==s.game_over_schemas[peer]);
            }
            s.Stop("Native GameOver authoritative frame failed phase admission");return RoomGameplayAdmission::Stopped;}
        s.prepared=frame;output=frame;gameplay_waiting.store(false,std::memory_order_release);return RoomGameplayAdmission::Ready;
    }
    NativeGameOverState control;uint32_t mode=0;
    if(!ReadNativeControlState(ctx,base,control,mode) || control.registry!=s.registry){s.Stop("Native input control-state query lost world ownership");return RoomGameplayAdmission::Stopped;}
    if(NativeCutsceneControlMode(mode) && !s.cutscene_entry){
        if(!BindNativeRoomLocalView(ctx,base,true)){s.Stop("Could not retain the original authored cutscene camera");return RoomGameplayAdmission::Stopped;}
        const auto first=s.room->next_tick(),raw=s.bootstrap->committed_tick();
        const bool startup=!s.started && !s.native_completed && !s.first_game_tick && s.ready_logged &&
            !s.bootstrap->completion_pending() && raw!=UINT64_MAX && first==raw+1 && s.input_epoch==1;
        if((!startup && (!s.started || !s.native_completed || !s.normal_control_mode)) || s.input_epoch==UINT64_MAX ||
           !(s.input_epoch&1) || !ReadNativeCutsceneContext(ctx,base,s.registry)){
            s.Stop("Native cutscene entry lacks a completed owned input epoch");return RoomGameplayAdmission::Stopped;}
        if(startup){s.startup_cutscene=true;s.first_game_tick=first;
            REXLOG_INFO("Native Room opening cutscene admitted from verified bootstrap tick {}; first gameplay tick {}; no prior gameplay frame fabricated",raw,first);}

        for(const auto& player:s.roster.players)s.cutscene_schemas[player.peer-1]=s.button_schemas[player.character==LobbyCharacter::Mickey?0:1];
        s.cutscene_entry=startup?raw:*s.native_completed;s.cutscene_resume.reset();++s.input_epoch;
        s.movie_sequence_target=0;s.movie_control_mode=0;
        s.private_skip_source.reset();s.private_skip_armed=false;s.private_skip_failed=false;s.skip_gate={};s.skip_receiver.reset();s.movie_receiver.reset();s.skip_target_available=false;s.skip_target_mode=0;s.skip_target_entry.reset();s.departing_cutscene_buttons.reset();
        REXLOG_INFO("Native Room cutscene input suspended: entry tick {}, input epoch {}; original state {} and native cutscene ownership, actor incarnations unchanged",*s.cutscene_entry,s.input_epoch,mode);
    }
    if(s.cutscene_entry && !NativeCutsceneControlMode(mode)){
        if(s.startup_cutscene){
            // The opening film has no prior gameplay control mode to restore.
            // Adopt its first native gameplay selector only after the same
            // registry, human actors and owned cameras pass normal validation.
            if(mode==7 || !em2::NativeCoopRuntimeReady(base)){s.Stop("Opening cutscene did not return to the owned human world");return RoomGameplayAdmission::Stopped;}
            if(!s.normal_control_mode)s.normal_control_mode=mode;
            std::string selected;
            if(ReadNativeInputContextName(base,selected) && selected!="CutScene" && !selected.empty())s.normal_context_name=selected;
            else if(s.normal_context_name=="CutScene")s.normal_context_name.clear();
        }

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
        const bool target_available=!s.return_pending && (((mode==6 || mode==1) && NativeCutsceneSkipTargetAvailable(s.registry,ctx,base)) ||
            (mode==2 && NativeMovieAcceptTargetAvailable(s.registry,ctx,base)));
        if(target_available!=s.skip_target_available || (target_available && s.skip_target_mode!=mode)){
            s.skip_receiver.reset();s.movie_receiver.reset();s.skip_gate={};s.skip_target_available=target_available;s.skip_target_mode=target_available?mode:0;
            s.skip_target_entry=target_available?(s.native_completed?s.native_completed:s.cutscene_entry):std::nullopt;
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
    if(!BindNativeRoomLocalView(ctx,base)){s.Stop("Could not restore the local gameplay camera");return RoomGameplayAdmission::Stopped;}
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
    s.committed=frame.tick;s.consumed=frame;s.pending=true;s.delivered=false;s.started=true;s.prepared.reset();s.submitted.reset();s.damage_events.clear();s.ai_decisions.clear();s.spawn_batches.clear();s.projectile_impacts.clear();s.shooter_launches.clear();s.launch_cursor=0;s.impacts_applied=false;s.ai_cursor=0;s.retirements_applied.clear();return true;
}
bool NativeRoomGameplay::Deliver(PPCContext& ctx,uint8_t* base){
    Environment environment;Context context(ctx);auto& s=*impl_;Impl::FaultOnThrow fault{s};uint64_t tick=0;
    if(!s.Owner() || s.stopped || !s.pending || s.delivered || !s.consumed ||
        !em2::CurrentAgreedSimulationTick(tick) || tick!=s.committed)return false;
    if(const auto reason=s.WorldFailure(base))return s.Stop(std::string("Native world changed before input delivery: ")+reason);
    const auto stage_authority=[&]{
        if(s.peer==1 || !s.room->host_correction_mode())return true;
        // Receive the host result before the guest native update. Lethal state
        // applied only after the update shifts death/reload by one simulation
        // step even when the original Damage listeners are used.
        const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(!s.pending_authority && s.room->state()==RoomState::Running &&
              std::chrono::steady_clock::now()<until && !NativeRoomLoadsCancelled()){
            UpdateNativeRoomSession(base,false);s.pending_authority=s.room->TakeAuthorityState();
            if(!s.pending_authority && s.room->HostLoadAnnounced(tick)){
                REXLOG_INFO("Native guest admitted host-announced load tick: {}; no completed combat packet, original update must enter the matching native loader",tick);
                return true;
            }
            if(!s.pending_authority)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if(!s.pending_authority || s.pending_authority->tick!=tick)return s.Stop("Host update result did not arrive before matching native guest update");
        if(!s.pending_authority->ai_decisions.empty() && (!REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_probe_room_ai_targets)))
            return s.Stop("Host AI target decisions require the matching native AI adapter");
        if(!s.pending_authority->spawn_batches.empty() && (!REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_probe_room_spawn_batches)))
            return s.Stop("Host spawn batches require the matching private native capture adapter");
        if(!s.pending_authority->projectile_impacts.empty() && (!REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_probe_room_projectile_impacts)))
            return s.Stop("Host projectile impacts require the matching private native impact adapter");
        if(!s.pending_authority->shooter_launches.empty() && (!REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_probe_room_shooter_launches)))
            return s.Stop("Host Shooter launches require the matching private native launch adapter");
        if(!s.pending_authority->retired_entities.empty() && (!REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_probe_room_entity_retirements)))
            return s.Stop("Host entity retirements require the matching private native adapter");
        if(REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_probe_room_entity_retirements)){
            for(const auto& actor:s.pending_authority->characters)s.authority_entities.insert(actor.id);
            for(const auto& command:s.pending_authority->ai_decisions)s.authority_entities.insert(command.owner);
            for(const auto& batch:s.pending_authority->spawn_batches)if(!batch.shooter && batch.collection)
                for(const auto& child:batch.children)s.authority_entities.insert(child.entity);
            if(s.authority_entities.size()>65536)return s.Stop("Native entity authority inventory exceeds bound");
        }
        if(!s.pending_authority->damages.empty()){
            const char* error=nullptr;
            std::vector<OnlineNameActor> players;uint64_t local_player=0;
            if(!NameTagActors(players,local_player,base))return s.Stop("Host Damage replay lost current native player bindings");
            std::vector<em2::NativeDamageActor> bindings;
            for(const auto& player:players){uint32_t handle=0;
                if(player.peer<=2)handle=s.label_handles[player.peer-1];
                else for(const auto& actor:s.actors->actors())if(actor.room_peer==player.peer){handle=actor.actor.handle;break;}
                bindings.push_back({uint8_t(player.peer),player.entity,handle});
            }
            if(!em2::ReplayNativeDamageEvents(ctx,base,s.pending_authority->damages,bindings,error))return s.Stop(std::string("Native host Damage replay failed: ")+(error?error:"unknown"));
            size_t shape_keys=0;for(const auto& damage:s.pending_authority->damages)shape_keys+=damage.shape_keys.size();
            REXLOG_INFO("Native host Damage batch replayed: tick {}, events {}, child shape keys {}; exact native reconstruction and original listeners before guest update",tick,s.pending_authority->damages.size(),shape_keys);
        }
        std::vector<OnlineNameActor> actors;uint64_t local=0;
        if(!NameTagActors(actors,local,base) || actors.size()!=s.pending_authority->actors.size())return s.Stop("Host pre-update death correction lost native roster ownership");
        for(const auto& sample:s.pending_authority->actors){
            if(!sample.health || *sample.health!=0)continue;
            const auto actor=std::find_if(actors.begin(),actors.end(),[&](const auto& value){return value.peer==sample.peer;});
            if(actor==actors.end())return s.Stop("Host pre-update death correction has no native actor");
            const auto health=em2::ReadNativeActorHealth(ctx,base,actor->entity);
            if(!health)return s.Stop("Host pre-update death correction could not inspect native health");
            if(*health>0){
                // Full event authority already delivered the host's actual
                // Damage operations above. A remaining zero can come from a
                // direct native path such as FallRescue; do not invent another
                // hit that native immunity may reject. Legacy health-only
                // correction retains its immediate Damage notification path.
                const bool full_damage=REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_probe_room_world_damage_events);
                // A direct host zero observed AFTER this update (FallRescue,
                // for example) must not be moved before the guest update.
                // That would advance Health death/countdown and respawn by
                // one native tick. Actual Damage events were replayed above;
                // remaining property drift is reconciled at completion.
                if(full_damage)continue;
                if(!em2::CorrectNativeActorHealth(ctx,base,actor->entity,0))return s.Stop("Host pre-update native death correction failed");
                REXLOG_INFO("Native host zero health reconciled before guest update: tick {}, peer {}; {}",tick,sample.peer,
                    full_damage?"original Health setter; death processing remains in the upcoming native update":"original Damage listeners");
            }
        }
        return true;
    };
    if(s.game_over_entry || s.consumed_passive){
        if(s.game_over_entry?!s.GameOverFrame(*s.consumed,ctx,base):!s.CutsceneFrame(*s.consumed,ctx,base))return s.Stop("Native no-gameplay input phase changed before native update");
        if(!BeginRetailPassiveTick(s.registry,tick,ctx,base))return s.Stop("Could not retain retail quarantine for native passive input phase");
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
        s.delivered=true;
        if(s.skip_target_mode==1 && s.skip_receiver && !ServiceNativeDirectMovieSkip(s.registry,ctx,base))
            return s.Stop("Native direct movie held-skip completion lost its agreed owner");
        return stage_authority(); // Character gameplay callbacks remain quarantined.
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
    s.delivered=true;return stage_authority();
}
bool NativeRoomGameplay::Complete(const std::array<uint8_t,32>& digest,PPCContext& ctx,uint8_t* base){
    Environment environment;Context context(ctx);auto& s=*impl_;Impl::FaultOnThrow fault{s};uint64_t tick=0;
    if(!s.Owner() || s.stopped || !s.pending || !s.delivered || !s.consumed ||
        !em2::CurrentAgreedSimulationTick(tick) || tick!=s.committed)return false;
    if(s.AnnouncedLoadCancellation(tick))return s.Stop("Host announced a native load, but guest completed without entering the matching loader");
    if(const auto failure=s.WorldFailure(base))return s.Stop(std::string("Native gameplay completion rejected: ")+failure);
    if(REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_probe_room_ai_targets) && s.peer!=1 &&
       (!s.pending_authority || s.ai_cursor!=s.pending_authority->ai_decisions.size()))return s.Stop("Native AI target completion did not consume the host decision batch");
    if(REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_probe_room_spawn_batches) && s.peer!=1){
        if(REXCVAR_GET(em2_probe_room_shooter_launches) && !ReplayShooterLaunches(ctx,base,false))return false;
        if(REXCVAR_GET(em2_probe_room_projectile_impacts) && !ReplayProjectileImpacts(ctx,base))return false;
        if(REXCVAR_GET(em2_probe_room_entity_retirements) && !ReplayEntityRetirements(ctx,base,false))return false;
        if(REXCVAR_GET(em2_probe_room_spawn_replay) && !ReplayMissingSpawns(ctx,base))return false;
        if(REXCVAR_GET(em2_probe_room_shooter_launches) && !ReplayShooterLaunches(ctx,base,true))return false;
        if(REXCVAR_GET(em2_probe_room_entity_retirements) && !ReplayEntityRetirements(ctx,base,true))return false;
        if(!s.pending_authority || s.spawn_batches!=s.pending_authority->spawn_batches)
            return s.Stop("Native spawn batch capture differs from host; spawn replay remains unimplemented");
        if(!s.spawn_batches.empty())REXLOG_INFO("Native host spawn batch capture matched: tick {}, batches {}; original births and bookkeeping compared, no replay",tick,s.spawn_batches.size());
    }
    if(s.game_over_entry || s.consumed_passive){
        if(!FinishRetailPassiveTick(tick,ctx,base))return s.Stop("Native passive input quarantine completion failed");
    }else{
        NativeGameOverState control;uint32_t mode=0;
        const bool movie_entry=!s.cutscene_entry && ReadNativeControlState(ctx,base,control,mode) && NativeMovieControlMode(mode) &&
            control.registry==s.registry && ReadNativeCutsceneContext(ctx,base,s.registry);
        const auto finished=s.consumed->players[0].source==RoomMovementSource::NativeActionV3?
            FinishRetailButtonTick(tick,ctx,base,movie_entry):FinishRetailJumpTick(tick,ctx,base,movie_entry);
        if(finished!=ActorInputResult::Applied)return s.Stop("Native retail button/jump completion rejected (result "+std::to_string(int(finished))+")");
        if(!s.actor_frames)return s.Stop("Native owned actor frame coordinator is missing at completion");
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
    if(REXCVAR_GET(em2_profile_world_memory) && tick%300==0)em2::ReportWorldGraphicsMemory("agreed-world",em2::CurrentNativeLoadGeneration());
    if(!s.room->CompleteGameplay(tick,digest))return s.Stop("Native gameplay world verification failed");
    const bool authority=s.room->host_correction_mode();
    if((authority || (REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_probe_room_authority_state))) && s.peer==1){
        // Read the native physics result, not interpolated presentation poses.
        // Existing typed/epoch-owned roster resolution covers retail and extra
        // players without sending a guest pointer or local handle over the wire.
        const auto scope=s.room->gameplay_history_scope();
        RoomAuthorityState state;state.tick=tick;state.completion=digest;
        std::vector<OnlineNameActor> actors;uint64_t local=0;
        std::vector<std::array<uint32_t,4>> player_ids;
        bool ready=scope && NameTagActors(actors,local,base);
        if(ready){state.world=scope->world;state.roster_revision=scope->roster_revision;state.players_mask=scope->players_mask;
            for(const auto& actor:actors){
                GameplayObservation::CharacterPhysics physics;
                if(!em2::ReadNativeActorPhysics(base,actor.entity,physics)){ready=false;break;}
                player_ids.push_back(physics.id);
                RoomActorState sample;sample.peer=actor.peer;sample.active=physics.active;
                if(authority){sample.health=em2::ReadNativeActorHealth(ctx,base,actor.entity);if(!sample.health){ready=false;break;}}
                for(unsigned i=0;i<3;++i){sample.position[i]=std::bit_cast<float>(physics.position[i]);sample.velocity[i]=std::bit_cast<float>(physics.velocity[i]);}
                state.actors.push_back(sample);
            }
        }
        if(authority && ready){
            std::vector<GameplayObservation::CharacterPhysics> world;
            ready=em2::ReadNativeCharacterPhysics(base,world);
            if(ready){
                em2::NativeEntitySnapshot entities;const char* error=nullptr;uint32_t address=0;
                ready=em2::ReadNativeEntitySnapshot(base,entities,error,address,false);
                world.erase(std::remove_if(world.begin(),world.end(),[&](const auto& character){return std::find(player_ids.begin(),player_ids.end(),character.id)!=player_ids.end();}),world.end());
                if(ready && REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_probe_room_entity_retirements)){
                    for(const auto& actor:world)s.authority_entities.insert(actor.id);
                    for(auto it=s.authority_entities.begin();it!=s.authority_entities.end();){
                        const auto found=std::lower_bound(entities.records.begin(),entities.records.end(),*it,[](const auto& row,const auto& id){return row.id<id;});
                        if(found!=entities.records.end() && found->id==*it){++it;continue;}
                        if(state.retired_entities.size()>=kAuthorityRetirementBatch){ready=false;break;}
                        state.retired_entities.push_back(*it);it=s.authority_entities.erase(it);
                    }
                    if(!state.retired_entities.empty())REXLOG_INFO("Native host entity retirements captured: tick {}, count {}; confirmed absent from full native registry",tick,state.retired_entities.size());
                }
                // Incremental batches keep even large inventories inside one
                // bounded packet without stalling input behind a bulk transfer.
                const auto count=std::min(world.size(),kAuthorityCharacterBatch);
                const auto first=world.empty()?0:size_t((tick%world.size())*kAuthorityCharacterBatch%world.size());
                for(size_t i=0;ready && i<count;++i){const auto& physics=world[(first+i)%world.size()];RoomCharacterState sample;
                    sample.id=physics.id;sample.active=physics.active;
                    const auto entity=std::lower_bound(entities.records.begin(),entities.records.end(),physics.id,[](const auto& record,const auto& id){return record.id<id;});
                    if(entity==entities.records.end() || entity->id!=physics.id){ready=false;break;}
                    sample.health=em2::ReadNativeActorHealth(ctx,base,entity->debug_address);
                    for(unsigned axis=0;axis<3;++axis){sample.position[axis]=std::bit_cast<float>(physics.position[axis]);sample.velocity[axis]=std::bit_cast<float>(physics.velocity[axis]);}
                    state.characters.push_back(sample);
                }
            }
        }
        if(authority){
            NativeGameOverState terminal;
            if(ReadNativeGameOverState(ctx,base,terminal) && terminal.registry==s.registry &&
               (s.game_over_entry || terminal.dead==s.roster.players.size()))
                state.game_over_entry=s.game_over_entry.value_or(tick);
            state.damages=std::move(s.damage_events);
            state.ai_decisions=std::move(s.ai_decisions);
            state.spawn_batches=std::move(s.spawn_batches);
            state.projectile_impacts=std::move(s.projectile_impacts);
            state.shooter_launches=std::move(s.shooter_launches);
            if(!ready)return s.Stop("Could not capture completed host actor state");
            const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(10);
            AuthorityPublishResult result;
            do{
                result=s.room->TryPublishAuthorityState(state);
                if(result!=AuthorityPublishResult::Backpressure)break;
                if(NativeRoomLoadsCancelled())return false;
                UpdateNativeRoomSession(base,false);
                if(s.room->state()!=RoomState::Running)break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }while(std::chrono::steady_clock::now()<until);
            if(result!=AuthorityPublishResult::Published)return s.Stop("Could not publish completed host actor state");
        }
        if(!authority && ready && !s.room->PublishAuthorityState(state))
            REXLOG_WARN("Private host-state publication deferred: tick {}; no correction or verification bypass",tick);
    }
    if(authority && s.peer!=1){
        // Do not apply a packet from an earlier/later update to the current
        // physics world. The host may pipeline later results within negotiated
        // credit; the client applies only this exact update's result.
        std::optional<RoomAuthorityState> state=std::move(s.pending_authority);s.pending_authority.reset();
        const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(s.room->state()==RoomState::Running && !state && std::chrono::steady_clock::now()<until && !NativeRoomLoadsCancelled()){
            UpdateNativeRoomSession(base,false);state=s.room->TakeAuthorityState();
            if(!state)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if(!state || state->tick!=tick)return s.Stop("Host correction did not arrive at the matching completed update");
        if(state->game_over_entry){
            if(s.host_game_over_entry && s.host_game_over_entry!=state->game_over_entry)
                return s.Stop("Host terminal phase changed its original world entry tick");
            s.host_game_over_entry=state->game_over_entry;
            if(s.game_over_entry && s.game_over_entry!=s.host_game_over_entry){
                REXLOG_INFO("Native GameOver phase aligned: local entry {} host entry {}; delayed commands retain original per-peer terminal stream",*s.game_over_entry,*s.host_game_over_entry);
                s.game_over_entry=s.host_game_over_entry;
            }
        }

        std::vector<OnlineNameActor> actors;uint64_t local=0;
        if(!NameTagActors(actors,local,base) || actors.size()!=state->actors.size())return s.Stop("Host correction roster has no current native bindings");
        bool discontinuity=false;unsigned corrected=0,health_corrected=0;
        auto health=[&](uint32_t entity,const std::optional<int32_t>& target,uint64_t player){
            if(!target)return true;
            auto current=em2::ReadNativeActorHealth(ctx,base,entity);if(!current)return false;
            if(player && REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_probe_room_authority_drift) && !s.authority_health_drift_injected && *current>1 && *current<1000){
                if(!em2::CorrectNativeActorHealth(ctx,base,entity,*current-1))return false;
                s.authority_health_drift_injected=true;current=em2::ReadNativeActorHealth(ctx,base,entity);
                if(!current)return false;
                REXLOG_INFO("Private native authority health drift injected: tick {}, peer {}, health {} (host {}) through original property setter",tick,player,*current,*target);
            }
            if(*current==*target)return true;
            const auto phase=REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_probe_room_world_damage_events)?
                em2::NativeHealthCorrectionPhase::CompletedPropertyOnly:em2::NativeHealthCorrectionPhase::CompletedUpdate;
            if(!em2::CorrectNativeActorHealth(ctx,base,entity,*target,phase))return false;
            if(*target==0 && *current>0 && phase==em2::NativeHealthCorrectionPhase::CompletedPropertyOnly)
                REXLOG_INFO("Native host zero health reconciled after guest update: tick {}, peer {}; original property setter, native death processing remains on the following tick",tick,player);
            ++health_corrected;
            if(player && s.authority_health_drift_injected && REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_probe_room_authority_drift))
                REXLOG_INFO("Private native authority health correction verified: tick {}, peer {}, {} -> {}; original property setter/readback, damage effects not replicated",tick,player,*current,*target);
            return true;
        };
        std::vector<std::array<uint32_t,4>> player_ids;
        for(const auto& sample:state->actors){
            const auto actor=std::find_if(actors.begin(),actors.end(),[&](const auto& entry){return entry.peer==sample.peer;});
            GameplayObservation::CharacterPhysics current;
            if(actor==actors.end() || !em2::ReadNativeActorPhysics(base,actor->entity,current) || current.active!=sample.active)
                return s.Stop("Host correction actor lifetime differs; native lifecycle synchronization required");
            player_ids.push_back(current.id);
            if(!health(actor->entity,sample.health,sample.peer))return s.Stop("Original native player health correction failed");
            if(!sample.active)continue;
            if(REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_probe_room_authority_drift) && !s.authority_drift_injected){
                std::array<float,3> moved{},velocity{};
                for(unsigned i=0;i<3;++i){moved[i]=std::bit_cast<float>(current.position[i]);velocity[i]=std::bit_cast<float>(current.velocity[i]);}
                moved[0]+=0.5f;
                if(!em2::CorrectNativeActorPhysics(ctx,base,actor->entity,moved,velocity) ||
                    !em2::ReadNativeActorPhysics(base,actor->entity,current))return s.Stop("Private native drift injection failed");
                s.authority_drift_injected=true;
                REXLOG_INFO("Private native authority drift injected: tick {}, peer {}, +0.5m via original physics setters",tick,sample.peer);
            }
            float distance=0,velocity_error=0;
            for(unsigned i=0;i<3;++i){const float d=sample.position[i]-std::bit_cast<float>(current.position[i]);distance+=d*d;
                const float v=sample.velocity[i]-std::bit_cast<float>(current.velocity[i]);velocity_error+=v*v;}
            if(distance<=0.000001f && velocity_error<=0.000025f)continue;
            if(!em2::CorrectNativeActorPhysics(ctx,base,actor->entity,sample.position,sample.velocity))
                return s.Stop("Original native physics correction failed");
            if(REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_probe_room_authority_drift) && corrected==0 && s.authority_drift_injected)
                REXLOG_INFO("Private native authority correction verified: tick {}, peer {}, squared position error {} before repair; native readback passed",tick,sample.peer,distance);
            discontinuity|=distance>4.0f;++corrected;
        }
        unsigned world_corrected=0,world_missing=0,world_inactive=0;
        if(!state->characters.empty()){
            em2::NativeEntitySnapshot entities;const char* error=nullptr;uint32_t address=0;
            if(!em2::ReadNativeEntitySnapshot(base,entities,error,address,false))return s.Stop("Current native entity registry unavailable for host world corrections");
            for(const auto& sample:state->characters){
                if(std::find(player_ids.begin(),player_ids.end(),sample.id)!=player_ids.end())return s.Stop("Host world correction overlaps player identity");
                const auto found=std::lower_bound(entities.records.begin(),entities.records.end(),sample.id,[](const auto& entity,const auto& id){return entity.id<id;});
                GameplayObservation::CharacterPhysics current;
                if(found==entities.records.end() || found->id!=sample.id || !em2::ReadNativeActorPhysics(base,found->debug_address,current)){++world_missing;continue;}
                if(current.active!=sample.active){++world_inactive;continue;}
                if(!health(found->debug_address,sample.health,0))return s.Stop("Original native world-character health correction failed");
                if(!sample.active)continue;
                if(REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_probe_room_authority_drift) && !s.authority_world_drift_injected){
                    std::array<float,3> moved{},velocity{};
                    for(unsigned i=0;i<3;++i){moved[i]=std::bit_cast<float>(current.position[i]);velocity[i]=std::bit_cast<float>(current.velocity[i]);}moved[0]+=0.5f;
                    if(!em2::CorrectNativeActorPhysics(ctx,base,found->debug_address,moved,velocity) || !em2::ReadNativeActorPhysics(base,found->debug_address,current))return s.Stop("Private native world-character drift injection failed");
                    s.authority_world_drift_injected=true;
                    REXLOG_INFO("Private native world-character drift injected: tick {}, GUID {:08X}/{:08X}/{:08X}/{:08X}, +0.5m via original physics setters",tick,sample.id[0],sample.id[1],sample.id[2],sample.id[3]);
                }
                float distance=0,velocity_error=0;
                for(unsigned i=0;i<3;++i){const float d=sample.position[i]-std::bit_cast<float>(current.position[i]);distance+=d*d;
                    const float v=sample.velocity[i]-std::bit_cast<float>(current.velocity[i]);velocity_error+=v*v;}
                if(distance<=0.000001f && velocity_error<=0.000025f)continue;
                if(!em2::CorrectNativeActorPhysics(ctx,base,found->debug_address,sample.position,sample.velocity))return s.Stop("Original native world-character correction failed");
                if(REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_probe_room_authority_drift) && !world_corrected && s.authority_world_drift_injected)
                    REXLOG_INFO("Private native world-character correction verified: tick {}, squared position error {} before repair; native GUID resolution/readback passed",tick,distance);
                discontinuity|=distance>4.0f;++world_corrected;
            }
        }
        if(discontinuity)em2::ResetNetworkPresentationHistory(ctx,base);
        if(!s.room->AcknowledgeAuthorityState(tick,state->completion))return s.Stop("Could not acknowledge applied native host state");
        if(tick%60==0)REXLOG_INFO("Native host correction applied: tick {}, actors {}, corrected {}; world characters {}, corrected {}, missing {}, active mismatch {}; health corrections {}; original native setters, no rollback",tick,actors.size(),corrected,state->characters.size(),world_corrected,world_missing,world_inactive,health_corrected);
    }
    // A completed 30 Hz update does not itself mean presentation is stalled.
    // Freeze interpolation only when Prepare actually cannot admit a due tick.
    s.pending=false;s.consumed.reset();return true;
}
void NativeRoomGameplay::ObservationFailed(){auto& s=*impl_;if(s.Owner() && !s.stopped)s.Stop("Completed native gameplay observation is unavailable");}
bool NativeRoomGameplay::active()const{return impl_->Configured();}
bool NativeRoomGameplay::halted()const{return impl_->stopped;}
bool NativeRoomGameplay::waiting()const{return gameplay_waiting.load(std::memory_order_acquire);}
bool NativeRoomGameplay::completion_pending()const{return impl_->pending;}
bool NativeRoomGameplay::SuppressLocalDamage(uint32_t target)const{
    const auto& s=*impl_;uint64_t tick=0;
    const bool owned=s.Owner() && s.Configured() && !s.stopped && s.peer!=1 && s.pending && !s.load_depth &&
        !s.transition_pending && s.room->state()==RoomState::Running && s.room->host_correction_mode() &&
        em2::CurrentAgreedSimulationTick(tick) && tick==s.committed;
    if(!owned || !target || s.AnnouncedLoadCancellation(tick))return false;
    if(REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_probe_room_world_damage_events))return true;
    return target==s.label_handles[0] || target==s.label_handles[1] ||
        std::any_of(s.actors->actors().begin(),s.actors->actors().end(),[&](const auto& entry){return entry.actor.handle==target;});
}
std::optional<size_t> NativeRoomGameplay::BeginAiTargetDecision(uint32_t brain,uint8_t* base){
    Environment preserve;em2::GuestReadBatch reads;auto& s=*impl_;uint64_t tick=0;std::array<uint32_t,4> owner;
    if(s.peer!=1 || !s.AiScope(brain,base,owner,tick))return {};
    if(s.ai_decisions.size()>=kAuthorityAiDecisionBatch){s.Stop("Native AI target decision batch exceeds bound");return {};}
    if(REXCVAR_GET(em2_probe_room_entity_retirements))s.authority_entities.insert(owner);
    const auto ticket=s.ai_decisions.size();RoomAiTargetDecision decision;decision.owner=owner;s.ai_decisions.push_back(decision);return ticket;
}
bool NativeRoomGameplay::RecordSpawnBatch(SpawnBatch batch,uint32_t owner,uint32_t anchor,uint8_t* base){
    Environment preserve;em2::GuestReadBatch reads;auto& s=*impl_;uint64_t tick=0;
    if(!REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_probe_room_spawn_batches) || !s.Owner() || !s.Configured() || s.stopped ||
       !s.pending || s.load_depth || s.transition_pending || s.room->state()!=RoomState::Running || !s.room->host_correction_mode() ||
       !em2::CurrentAgreedSimulationTick(tick) || tick!=s.committed)return false;
    if(s.AnnouncedLoadCancellation(tick))return false;
    const auto canonical=[&](SpawnReference& ref,uint32_t entity){
        if(!entity)return std::all_of(ref.id.begin(),ref.id.end(),[](auto word){return word==0;});
        if(!Readable(entity,20,base) || PPC_LOAD_U32(entity)!=0x8208D908)return false;
        for(unsigned i=0;i<4;++i)if(PPC_LOAD_U32(entity+4+i*4)!=ref.id[i])return false;
        ref.peer=s.HumanPeer(entity);if(ref.peer)ref.id={};return true;
    };
    if(!canonical(batch.owner,owner) || !canonical(batch.anchor,anchor) || !ValidSpawnBatch(batch))return s.Stop("Native spawn capture has unsupported ownership, identity or child side effects");
    if((s.spawn_replaying || (s.peer!=1 && REXCVAR_GET(em2_probe_room_spawn_replay))) && (!s.pending_authority || s.spawn_batches.size()>=s.pending_authority->spawn_batches.size() ||
       batch!=s.pending_authority->spawn_batches[s.spawn_batches.size()]))return s.Stop("Replayed native spawn differs from complete host parent payload");
    if(s.spawn_batches.size()>=kAuthoritySpawnBatch)return s.Stop("Native spawn capture batch exceeds bound");
    if(REXCVAR_GET(em2_probe_room_projectile_impacts) && batch.shooter){
        if(s.projectile_load!=s.load){s.owned_projectiles.clear();s.projectile_load=s.load;}
        for(const auto& child:batch.children){
            if(s.owned_projectiles.size()>=65536)return s.Stop("Native projectile authority inventory exceeds bound");
            s.owned_projectiles.insert(child.entity);
        }
    }
    if(REXCVAR_GET(em2_probe_room_entity_retirements) && !batch.shooter && batch.collection){
        for(const auto& child:batch.children)s.authority_entities.insert(child.entity);
        if(s.authority_entities.size()>65536)return s.Stop("Native collection authority inventory exceeds bound");
    }
    s.spawn_batches.push_back(std::move(batch));return true;
}
NativeImpactMode NativeRoomGameplay::ProjectileImpactMode(const SpawnId& id,uint32_t caller){
    Environment preserve;auto& s=*impl_;uint64_t tick=0;
    if(!REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_probe_room_projectile_impacts) || !REXCVAR_GET(em2_probe_room_spawn_replay) ||
       !REXCVAR_GET(em2_probe_room_world_damage_events) || !s.Owner() || !s.Configured() || s.stopped || !s.pending || s.load_depth || s.transition_pending ||
       s.room->state()!=RoomState::Running || !s.room->host_correction_mode() || s.projectile_load!=s.load || !s.owned_projectiles.contains(id) ||
       !em2::CurrentAgreedSimulationTick(tick) || tick!=s.committed || s.AnnouncedLoadCancellation(tick))return NativeImpactMode::Original;
    // All direct original callers below ignore this void operation's return.
    switch(caller){case 0x8293E5E4:case 0x8293F8D4:case 0x82940BFC:case 0x82940E48:case 0x82941538:case 0x82941588:case 0x8294275C:case 0x82943480:break;
    default:return NativeImpactMode::Original;}
    return s.peer==1?NativeImpactMode::Host:NativeImpactMode::Suppress;
}
NativeImpactMode NativeRoomGameplay::ShooterLaunchMode(std::span<const SpawnId> shots,uint32_t caller){
    Environment preserve;auto& s=*impl_;uint64_t tick=0;
    if(!REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_probe_room_shooter_launches) || !REXCVAR_GET(em2_probe_room_projectile_impacts) ||
       !REXCVAR_GET(em2_probe_room_spawn_replay) || !s.Owner() || !s.Configured() || s.stopped || !s.pending || s.load_depth || s.transition_pending ||
       s.room->state()!=RoomState::Running || !s.room->host_correction_mode() || s.projectile_load!=s.load || shots.empty() ||
       !std::all_of(shots.begin(),shots.end(),[&](const auto& id){return s.owned_projectiles.contains(id);}) ||
       !em2::CurrentAgreedSimulationTick(tick) || tick!=s.committed || s.AnnouncedLoadCancellation(tick))return NativeImpactMode::Original;
    // All six direct original callers overwrite r3 before using another result.
    switch(caller){case 0x828B9058:case 0x82957A98:case 0x82A835A0:case 0x82A8B6D8:case 0x82A993FC:case 0x82B560B4:break;default:return NativeImpactMode::Original;}
    return s.peer==1?NativeImpactMode::Host:NativeImpactMode::Suppress;
}
bool NativeRoomGameplay::RecordShooterLaunch(ShooterLaunch launch,uint32_t owner){
    Environment preserve;auto& s=*impl_;uint64_t tick=0;
    if(!s.Owner() || !s.Configured() || s.stopped || s.peer!=1 || !s.pending || s.load_depth || s.transition_pending ||
       !REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_probe_room_shooter_launches) || !em2::CurrentAgreedSimulationTick(tick) || tick!=s.committed)return false;
    if((launch.owner.peer=s.HumanPeer(owner)))launch.owner.id={};
    if(!ValidShooterLaunch(launch) || s.shooter_launches.size()>=kAuthorityLaunchBatch)return s.Stop("Native Shooter launch capture exceeds its input or batch contract");
    REXLOG_INFO("Native host Shooter launch captured: tick {}, owner peer {} GUID {:08X}/{:08X}/{:08X}/{:08X}, shots {}, success {}, FX clones {}",tick,launch.owner.peer,launch.owner.id[0],launch.owner.id[1],launch.owner.id[2],launch.owner.id[3],launch.projectiles.size(),launch.success,launch.clones.size());
    s.shooter_launches.push_back(std::move(launch));return true;
}
bool NativeRoomGameplay::ReplayShooterLaunches(PPCContext& ctx,uint8_t* base,bool required){
    auto& s=*impl_;uint64_t tick=0;
    if(!s.Owner() || !s.pending_authority || s.stopped || s.peer==1 || !s.pending || s.load_depth || s.transition_pending ||
       !em2::CurrentAgreedSimulationTick(tick) || tick!=s.committed || s.pending_authority->tick!=tick)return s.Stop("Native Shooter launch replay lost its authority tick");
    while(s.launch_cursor<s.pending_authority->shooter_launches.size()){
        const auto expected=s.pending_authority->shooter_launches[s.launch_cursor];
        NativeEntitySnapshot snapshot;const char* error=nullptr;uint32_t address=0;uint64_t before=0,after=0;
        if(!em2::SampleNativeEntityLifetimeRevision(before) || !ReadNativeEntitySnapshot(base,snapshot,error,address,false) ||
           !em2::SampleNativeEntityLifetimeRevision(after) || before!=after)return s.Stop("Native launch registry changed during owner resolution");
        uint32_t entity=0;
        if(expected.owner.peer){std::vector<OnlineNameActor> players;uint64_t local=0;
            if(!NameTagActors(players,local,base))return s.Stop("Native launch player bindings unavailable");
            for(const auto& player:players)if(player.peer==expected.owner.peer)entity=player.entity;
        }else for(const auto& row:snapshot.records)if(row.id==expected.owner.id){entity=row.debug_address;break;}
        const auto shooter=NamedComponent(base,entity,"ShooterComponent");ShooterLaunch pending;uint32_t owner=0;
        const bool ready=shooter && em2::ReadNativeShooterPending(base,shooter,pending,owner) && pending.projectiles==expected.projectiles;
        if(!ready){if(!required)return true;return s.Stop("Host launch pending list is unavailable after birth replay");}
        if(!em2::ReplayNativeShooterLaunch(ctx,base,shooter,expected,error))return s.Stop(std::string("Native host Shooter launch failed: ")+(error?error:"unknown"));
        if(CurrentNativeRoomGameplay()!=this || !s.Configured() || !s.pending_authority || s.pending_authority->tick!=tick || s.load_depth || s.transition_pending)
            return s.Stop("Native Shooter launch left its original world");
        ++s.launch_cursor;
        REXLOG_INFO("Native host Shooter launch replayed: tick {}, shots {}, success {}, FX clones {}; original launch processor emptied its pending list",tick,expected.projectiles.size(),expected.success,expected.clones.size());
    }return true;
}
bool NativeRoomGameplay::RecordProjectileImpact(ProjectileImpact impact){
    Environment preserve;auto& s=*impl_;uint64_t tick=0;
    if(!s.Owner() || !s.Configured() || s.stopped || s.peer!=1 || !s.pending || s.load_depth || s.transition_pending ||
       !REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_probe_room_projectile_impacts) || !em2::CurrentAgreedSimulationTick(tick) || tick!=s.committed)return false;
    if(!ValidProjectileImpact(impact) || !s.owned_projectiles.contains(impact.projectile) || s.projectile_impacts.size()>=kAuthorityImpactBatch)
        return s.Stop("Native projectile impact capture has unsupported identity, RNG, FX or batch data");
    REXLOG_INFO("Native host projectile impact captured: tick {} GUID {:08X}/{:08X}/{:08X}/{:08X}, FX clones {}, retirement queued {}",tick,impact.projectile[0],impact.projectile[1],impact.projectile[2],impact.projectile[3],impact.clones.size(),impact.queued);
    s.projectile_impacts.push_back(std::move(impact));return true;
}
bool NativeRoomGameplay::ReplayProjectileImpacts(PPCContext& ctx,uint8_t* base){
    auto& s=*impl_;uint64_t tick=0;
    if(s.impacts_applied)return true;
    if(!s.Owner() || !s.pending_authority || s.stopped || s.peer==1 || !s.pending || s.load_depth || s.transition_pending ||
       !em2::CurrentAgreedSimulationTick(tick) || tick!=s.committed || s.pending_authority->tick!=tick)return s.Stop("Native projectile impact replay lost its owned authority tick");
    const auto impacts=s.pending_authority->projectile_impacts;
    for(const auto& impact:impacts){
        if(s.projectile_load!=s.load || !s.owned_projectiles.contains(impact.projectile))return s.Stop("Host impact refers to a projectile outside the captured Shooter inventory");
        NativeEntitySnapshot snapshot;const char* error=nullptr;uint32_t address=0;uint64_t before=0,after=0;
        if(!em2::SampleNativeEntityLifetimeRevision(before) || !ReadNativeEntitySnapshot(base,snapshot,error,address,false) ||
           !em2::SampleNativeEntityLifetimeRevision(after) || before!=after)return s.Stop("Native impact registry changed during projectile resolution");
        const auto found=std::find_if(snapshot.records.begin(),snapshot.records.end(),[&](const auto& row){return row.id==impact.projectile;});
        if(found==snapshot.records.end())return s.Stop("Host impact projectile has no matching live guest entity");
        if(!em2::ReplayNativeProjectileImpact(ctx,base,found->debug_address,impact,error))return s.Stop(std::string("Native host projectile impact replay failed: ")+(error?error:"unknown"));
        if(CurrentNativeRoomGameplay()!=this || !s.Configured() || !s.pending_authority || s.pending_authority->tick!=tick || s.load_depth || s.transition_pending)
            return s.Stop("Native projectile impact replay left its original world");
        REXLOG_INFO("Native host projectile impact replayed: tick {} GUID {:08X}/{:08X}/{:08X}/{:08X}, FX clones {}, retirement queued {}; original cleanup and canonical clone IDs verified",tick,impact.projectile[0],impact.projectile[1],impact.projectile[2],impact.projectile[3],impact.clones.size(),impact.queued);
    }
    s.impacts_applied=true;return true;
}
bool NativeRoomGameplay::SuppressSpawnBatch(const PPCContext& ctx,uint8_t* base,unsigned depth){
    Environment preserve;em2::GuestReadBatch reads;auto& s=*impl_;uint64_t tick=0;
    if(!REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_probe_room_spawn_batches) || !REXCVAR_GET(em2_probe_room_spawn_eligibility) ||
       !s.Owner() || !s.Configured() || s.stopped || s.peer==1 || !s.pending || s.load_depth || s.transition_pending ||
       s.room->state()!=RoomState::Running || !s.room->host_correction_mode() || !s.pending_authority ||
       !em2::CurrentAgreedSimulationTick(tick) || tick!=s.committed || s.pending_authority->tick!=tick)return false;
    // The Shooter caller checks null before using the returned entity.
    // The audited Lua collection route (outer return 827B42DC) ignores r3
    // inside 827B40B0 and converts only the actual output array afterward.
    // Skipping a guest-only parent leaves that array unchanged; no null entry
    // or synthetic child is appended. Other outer/indirect callers stay native.
    const bool firing=uint32_t(ctx.lr)==0x828B71C4 && !ctx.r7.u32 && ctx.r6.u32<=1;
    const bool collection=uint32_t(ctx.lr)==0x827B4178 && ctx.r6.u32==1 && ctx.r7.u32 && Readable(ctx.r7.u32,12,base) &&
        ctx.r1.u32<=UINT32_MAX-156 && Readable(ctx.r1.u32+152,4,base) && PPC_LOAD_U32(ctx.r1.u32+152)==0x827B42DC;
    if(depth || (!firing && !collection))return false;
    const auto component=ctx.r3.u32;
    if(!Readable(component,148,base))return false;
    const auto attached=PPC_LOAD_U32(component+48);
    if(!attached || attached>UINT32_MAX-8 || !Readable(attached+8,20,base) || PPC_LOAD_U32(attached+8)!=0x8208D908)return false;
    SpawnReference owner;owner.peer=s.HumanPeer(attached+8);
    if(!owner.peer)for(unsigned i=0;i<4;++i)owner.id[i]=PPC_LOAD_U32(attached+12+i*4);
    const auto eligibility=EvaluateSpawnEligibility(s.pending_authority->spawn_batches,s.spawn_batches,owner);
    if(collection && eligibility==SpawnEligibility::Original && REXCVAR_GET(em2_probe_room_collection_drift) &&
       REXCVAR_GET(em2_room_probe_role)=="guest" && !s.spawn_replaying && !s.collection_drift_injected && tick>=800 &&
       s.spawn_batches.size()<s.pending_authority->spawn_batches.size()){
        const auto& next=s.pending_authority->spawn_batches[s.spawn_batches.size()];
        if(!next.depth && !next.shooter && next.collection && next.flag && next.owner==owner && next.anchor==owner){
            s.collection_drift_injected=true;
            REXLOG_INFO("Private matching collection birth omitted: tick {} owner {:08X}/{:08X}/{:08X}/{:08X}; host command remains pending for native replay",tick,owner.id[0],owner.id[1],owner.id[2],owner.id[3]);
            return true;
        }
    }
    switch(eligibility){
    case SpawnEligibility::Original:return false;
    case SpawnEligibility::InvalidPrefix:s.Stop("Native spawn eligibility rejected an invalid completed host prefix");return true;
    case SpawnEligibility::Suppress:
        REXLOG_INFO("Native guest-only spawn suppressed: tick {} owner peer {} GUID {:08X}/{:08X}/{:08X}/{:08X}, count {}, choice {}, used {:08X}; audited caller, original parent not entered, no RNG/counter/birth or output-collection side effects",
            tick,owner.peer,owner.id[0],owner.id[1],owner.id[2],owner.id[3],PPC_LOAD_U32(component+104),PPC_LOAD_U32(component+140),PPC_LOAD_U32(component+144));
        return true;
    }
    return false;
}
bool NativeRoomGameplay::PrepareSpawnReplay(PPCContext& ctx,uint8_t* base,unsigned depth){
    Environment preserve;em2::GuestReadBatch reads;auto& s=*impl_;uint64_t tick=0;
    if(!s.spawn_replaying){
        if(!REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_probe_room_spawn_batches) || !REXCVAR_GET(em2_probe_room_spawn_eligibility) ||
           !REXCVAR_GET(em2_probe_room_spawn_replay) || !s.Owner() || !s.Configured() || s.stopped || s.peer==1 || !s.pending ||
           s.load_depth || s.transition_pending || s.room->state()!=RoomState::Running)return true;
        // Audited native callers consume the host's parent inputs.
        // 827B4178 passes its live output array from the original Lua route;
        // missing-birth replay below uses a separate, owned temporary array.
        // Reconstructing a missing Lua continuation remains separate work.
        if(depth || (uint32_t(ctx.lr)!=0x828B71C4 && uint32_t(ctx.lr)!=0x827B4178 && uint32_t(ctx.lr)!=0x827B4B40))return true;
    }
    if(s.AnnouncedLoadCancellation(s.committed))return true;
    const auto fail=[&](const char* reason){
        REXLOG_ERROR("Native spawn replay failure context: tick {}, caller {:08X}, parent frame return {:08X}, depth {}, local batches {}, host present {}, host batches {}; {}",
            s.committed,uint32_t(ctx.lr),ctx.r1.u32<=UINT32_MAX-156 && Readable(ctx.r1.u32+152,4,base)?PPC_LOAD_U32(ctx.r1.u32+152):0,
            depth,s.spawn_batches.size(),bool(s.pending_authority),s.pending_authority?s.pending_authority->spawn_batches.size():0,reason);
        s.Stop(reason);return false;
    };
    if(s.stopped || !s.Owner() || s.peer==1 || !s.pending || s.load_depth || s.transition_pending ||
       !s.pending_authority || !em2::CurrentAgreedSimulationTick(tick) || tick!=s.committed || s.pending_authority->tick!=tick ||
       s.spawn_batches.size()>=s.pending_authority->spawn_batches.size())return fail("Native spawn replay lost its owned tick or host command");
    const bool firing_path=uint32_t(ctx.lr)==0x828B71C4 && !ctx.r7.u32 && ctx.r6.u32<=1;
    const bool collection_path=uint32_t(ctx.lr)==0x827B4178 && ctx.r7.u32 && Readable(ctx.r7.u32,12,base) && ctx.r6.u32==1;
    // 827B4AB0's indexed native spawn loop passes flag 1 and no output
    // array. Its 827B4B40 return site ignores r3 and checks component state
    // before the next iteration. Keep that original loop and its callbacks.
    const bool indexed_path=uint32_t(ctx.lr)==0x827B4B40 && !ctx.r7.u32 && ctx.r6.u32==1;
    if(depth || (!firing_path && !collection_path && !indexed_path))return fail("Native spawn replay reached an unsupported parent caller");
    const auto component=ctx.r3.u32,anchor=ctx.r5.u32;
    if(!Readable(component,148,base))return fail("Native spawn replay component unavailable");
    const auto attached=PPC_LOAD_U32(component+48);
    const auto binding=[&]{return attached && attached<=UINT32_MAX-8 && Readable(attached+8,20,base) &&
        PPC_LOAD_U32(attached+8)==0x8208D908 && Readable(anchor,20,base) && PPC_LOAD_U32(anchor)==0x8208D908 &&
        Readable(component,148,base) && PPC_LOAD_U32(component+48)==attached && (!firing_path || attached==anchor-8);};
    if(!binding())return fail("Native spawn replay owner/component binding differs from host");
    const auto reference=[&](uint32_t face){
        SpawnReference ref;ref.peer=s.HumanPeer(face);
        if(!ref.peer)for(unsigned i=0;i<4;++i)ref.id[i]=PPC_LOAD_U32(face+4+i*4);
        return ref;
    };
    const auto owner=reference(attached+8);
    if(!s.spawn_replaying && s.pending_authority->spawn_batches[s.spawn_batches.size()].owner!=owner){
        // A missing host birth may precede this local caller in the same tick.
        // Replay only that host prefix, restoring the caller's context/stack,
        // then admit its own command. Do not replay the current caller twice.
        if(!ReplayMissingSpawns(ctx,base,&owner))return false;
        if(!s.pending_authority || s.pending_authority->tick!=tick || s.spawn_batches.size()>=s.pending_authority->spawn_batches.size() || !binding())
            return fail("Native spawn prefix replay lost the pending local caller");
    }
    const auto expected=s.pending_authority->spawn_batches[s.spawn_batches.size()];
    const bool admitted=(firing_path && expected.shooter && !expected.collection && expected.owner==expected.anchor) ||
        (collection_path && !expected.shooter && expected.collection && expected.flag) ||
        (indexed_path && !expected.shooter && !expected.collection && expected.flag);
    if(expected.depth || !expected.random_present || !admitted)
        return fail("Native spawn replay reached an unsupported parent path");
    if(reference(attached+8)!=expected.owner || reference(anchor)!=expected.anchor)
        return fail("Native spawn replay owner/anchor differs from the next host command");
    for(unsigned i=0;i<3;++i)if(PPC_LOAD_U32(component+(i==0?104:i==1?140:144))!=expected.before[i]){
        REXLOG_ERROR("Native spawn pre-state mismatch: tick {} owner {:08X}/{:08X}/{:08X}/{:08X}, component {:08X}, local {}/{}/{:08X}, host {}/{}/{:08X}, tracked count {}",
            tick,owner.id[0],owner.id[1],owner.id[2],owner.id[3],component,
            PPC_LOAD_U32(component+104),PPC_LOAD_U32(component+140),PPC_LOAD_U32(component+144),
            expected.before[0],expected.before[1],expected.before[2],PPC_LOAD_U32(component+72));
        return fail("Native spawn replay pre-state differs; counters are not blindly overwritten");
    }
    if(!em2::SeedNativeSpawnReplay(ctx,base,expected.random_before))return fail("Native spawn replay cannot bind and seed its original CRT owner");
    ctx.r4.s64=expected.selection;ctx.r6.u64=expected.flag;
    return true;
}
bool NativeRoomGameplay::ReplayEntityRetirements(PPCContext& ctx,uint8_t* base,bool required){
    auto& s=*impl_;Environment environment;uint64_t tick=0;
    if(s.peer==1)return true;
    if(!s.Owner() || !s.pending || !s.delivered || s.stopped || s.load_depth || s.transition_pending || !s.pending_authority ||
       !em2::CurrentAgreedSimulationTick(tick) || tick!=s.committed || s.pending_authority->tick!=tick)
        return s.Stop("Native retirement replay lost its owned authority tick");
    const auto commands=s.pending_authority->retired_entities;
    for(const auto& id:commands){
        if(s.retirements_applied.contains(id))continue;
        if(!s.authority_entities.contains(id))return s.Stop("Host retirement refers to an entity outside the authority inventory");
        bool pending_birth=false;
        for(size_t i=s.spawn_batches.size();i<s.pending_authority->spawn_batches.size();++i)
            for(const auto& child:s.pending_authority->spawn_batches[i].children)pending_birth|=child.entity==id;
        if(pending_birth){if(!required)continue;return s.Stop("Host retirement still precedes its missing native birth");}
        std::vector<OnlineNameActor> players;uint64_t local=0;
        if(!NameTagActors(players,local,base))return s.Stop("Native retirement lost current human actor bindings");
        std::vector<uint32_t> humans;for(const auto& player:players)humans.push_back(player.entity);
        bool requested=false;const char* error=nullptr;
        if(!em2::ReplayNativeEntityRetirement(ctx,base,s.registry,id,humans,requested,error))
            return s.Stop(std::string("Native host retirement replay failed: ")+(error?error:"unknown"));
        if(CurrentNativeRoomGameplay()!=this || !s.Configured() || !s.pending_authority || s.pending_authority->tick!=tick || s.load_depth || s.transition_pending)
            return s.Stop("Native retirement replay left its original world");
        s.retirements_applied.insert(id);s.authority_entities.erase(id);
        REXLOG_INFO("Native host entity retirement replayed: tick {} GUID {:08X}/{:08X}/{:08X}/{:08X}, original request {}; registry absence verified",tick,id[0],id[1],id[2],id[3],requested);
    }
    return true;
}
bool NativeRoomGameplay::PrepareSpawnerCleanup(PPCContext& ctx,uint8_t* base){
    Environment environment;Context context(ctx);auto& s=*impl_;uint64_t tick=0;
    if(uint32_t(ctx.lr)!=0x828B91C8 || !REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_probe_room_spawn_replay) ||
       !REXCVAR_GET(em2_probe_room_spawn_eligibility) || !s.Owner() || !s.Configured() || s.peer==1 || s.spawn_replaying ||
       s.stopped || !s.pending || s.load_depth || s.transition_pending || !s.pending_authority ||
       !em2::CurrentAgreedSimulationTick(tick) || tick!=s.committed || s.pending_authority->tick!=tick ||
       s.AnnouncedLoadCancellation(tick))return true;
    const auto component=ctx.r3.u32;
    const auto admitted=[&](SpawnReference& owner){
        em2::GuestReadBatch reads;
        if(!Readable(component,148,base))return false;
        const auto attached=PPC_LOAD_U32(component+48);
        if(!attached || attached>UINT32_MAX-28 || !Readable(attached+8,20,base) || PPC_LOAD_U32(attached+8)!=0x8208D908)return false;
        owner.peer=s.HumanPeer(attached+8);if(!owner.peer)for(unsigned i=0;i<4;++i)owner.id[i]=PPC_LOAD_U32(attached+12+i*4);
        if(NamedComponent(base,attached+8,"TemplateSpawnerComponent")!=component)return false;
        return SpawnBeforeCleanupReady(s.pending_authority->spawn_batches,s.spawn_batches,owner,
            {PPC_LOAD_U32(component+104),PPC_LOAD_U32(component+140),PPC_LOAD_U32(component+144)});
    };
    SpawnReference owner;
    while(admitted(owner)){
        const auto before=s.spawn_batches.size();
        if(!ReplayMissingSpawns(ctx,base,nullptr,&owner))return false;
        if(s.stopped || !s.pending_authority || s.pending_authority->tick!=tick || s.spawn_batches.size()<=before)
            return s.Stop("Pre-cleanup Shooter replay lost its host-command progress");
        REXLOG_INFO("Native host Shooter preparation replayed before slot cleanup: tick {}, batches {}; exact parent pre-state retained, original slot cleanup follows",tick,s.spawn_batches.size()-before);
        owner={};
    }
    return true;
}
bool NativeRoomGameplay::ReplayMissingSpawns(PPCContext& ctx,uint8_t* base,const SpawnReference* stop_before,const SpawnReference* stop_after){
    auto& s=*impl_;uint64_t tick=0;Environment environment;
    if(!REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_probe_room_spawn_batches) || !REXCVAR_GET(em2_probe_room_spawn_eligibility) ||
       !REXCVAR_GET(em2_probe_room_spawn_replay) || !s.Owner() || s.stopped || s.peer==1 || !s.pending || s.load_depth || s.transition_pending ||
       !s.pending_authority || !em2::CurrentAgreedSimulationTick(tick) || tick!=s.committed || s.pending_authority->tick!=tick)
        return s.Stop("Native missing-spawn replay is outside its admitted authority scope");
    while(s.pending_authority && s.spawn_batches.size()<s.pending_authority->spawn_batches.size()){
        // Copy commands before native callbacks can enter a transition/detach.
        const auto expected=s.pending_authority->spawn_batches[s.spawn_batches.size()];
        if(stop_before && expected.owner==*stop_before)return true;
        const bool collection_replay=!expected.shooter && expected.collection && expected.flag;
        if(expected.depth || !expected.random_present || expected.owner!=expected.anchor ||
           (!collection_replay && (!expected.shooter || expected.collection)))
            return s.Stop("Missing host spawn is not an audited self-owned Shooter or collection operation");
        NativeEntitySnapshot snapshot;const char* error=nullptr;uint32_t address=0;uint64_t before=0,after=0;
        if(!em2::SampleNativeEntityLifetimeRevision(before) || !ReadNativeEntitySnapshot(base,snapshot,error,address,false) ||
           !em2::SampleNativeEntityLifetimeRevision(after) || before!=after)return s.Stop("Native missing-spawn registry changed during resolution");
        uint32_t entity=0;
        if(expected.owner.peer){
            std::vector<OnlineNameActor> players;uint64_t local=0;
            if(!NameTagActors(players,local,base))return s.Stop("Native missing-spawn player bindings unavailable");
            for(const auto& player:players)if(player.peer==expected.owner.peer)entity=player.entity;
        }else for(const auto& record:snapshot.records)if(record.id==expected.owner.id){entity=record.debug_address;break;}
        const auto spawner=NamedComponent(base,entity,"TemplateSpawnerComponent");
        const auto shooter=collection_replay?0:NamedComponent(base,entity,"ShooterComponent");
        if(!spawner || !Readable(spawner,148,base) || PPC_LOAD_U32(spawner+48)!=entity-8)
            return s.Stop("Native missing-spawn TemplateSpawner class binding unavailable");
        if(!collection_replay && (!shooter || !Readable(shooter,96,base) ||
           PPC_LOAD_U32(shooter+48)!=entity-8 || PPC_LOAD_U32(shooter+72)!=spawner))
            return s.Stop("Native missing-spawn Shooter class binding unavailable");
        if(!collection_replay && PPC_LOAD_U32(shooter+84))return s.Stop("Native Shooter has unlaunched shots while host requests fresh preparation");
        const auto old_count=s.spawn_batches.size();
        std::vector<uint32_t> output_handles;bool output_valid=true;
        const auto top=ctx.r1.u32&~15u;constexpr size_t bytes=65536;
        if(top<bytes+4096 || !Readable(top-uint32_t(bytes),bytes,base))return s.Stop("Native spawn replay has no bounded borrowed stack");
        {
            Context context(ctx);std::vector<uint8_t> backup(bytes);const auto start=top-uint32_t(bytes);
            std::memcpy(backup.data(),base+start,bytes);
            struct Restore {uint8_t* destination;const std::vector<uint8_t>& bytes;~Restore(){std::memcpy(destination,bytes.data(),bytes.size());}} restore{base+start,backup};
            struct Reset {bool& value;~Reset(){value=false;}} reset{s.spawn_replaying};s.spawn_replaying=true;
            ctx.r1.u64=top-512;PPC_STORE_U32(ctx.r1.u32,top);
            if(collection_replay){
                // Native parent appends local entity handles to this owned
                // temporary array. Release its buffer with the original array
                // destructor; never retain handles or replace Lua VM storage.
                const auto array=top-256;std::memset(base+array,0,12);
                const auto release=[&]{ctx.r1.u64=top-512;ctx.r3.u64=array;em2::GuestReadBatch::CallbackBoundary callback;__imp__sub_832212E8(ctx,base);};
                try{
                    ctx.r3.u64=spawner;ctx.r4.s64=expected.selection;ctx.r5.u64=entity;
                    ctx.r6.u64=expected.flag;ctx.r7.u64=array;ctx.lr=0x827B4178;
                    {em2::GuestReadBatch::CallbackBoundary callback;sub_827B3C60(ctx,base);}
                    const auto data=PPC_LOAD_U32(array),count=PPC_LOAD_U32(array+4),capacity=PPC_LOAD_U32(array+8);
                    output_valid=count<=kMaxSpawnChildren && count<=capacity && (!count || Readable(data,size_t(count)*4,base));
                    if(output_valid)for(unsigned i=0;i<count;++i)output_handles.push_back(PPC_LOAD_U32(data+i*4));
                }catch(...){release();throw;}
                release();
            }else{
                ctx.r3.u64=shooter;
                {em2::GuestReadBatch::CallbackBoundary callback;__imp__sub_828B92A8(ctx,base);}
            }
        }
        if(CurrentNativeRoomGameplay()!=this || !s.Configured() || s.stopped || !s.pending_authority || s.pending_authority->tick!=tick || s.load_depth || s.transition_pending)
            return s.Stop("Native spawn replay left its original owned world");
        if(s.spawn_batches.size()<=old_count)return s.Stop("Native spawn replay made no host-command progress");
        if(collection_replay){
            if(!output_valid || s.spawn_batches.size()!=old_count+1 || output_handles.size()!=expected.children.size())
                return s.Stop("Native collection replay output count differs from the host children");
            if(!ReadNativeEntitySnapshot(base,snapshot,error,address,false))return s.Stop("Native collection replay cannot verify its registered children");
            for(size_t i=0;i<expected.children.size();++i){
                const auto record=std::find_if(snapshot.records.begin(),snapshot.records.end(),[&](const auto& row){return row.id==expected.children[i].entity;});
                if(record==snapshot.records.end() || record->handle!=output_handles[i])
                    return s.Stop("Native collection replay output handle/order differs from the host child identities");
            }
            REXLOG_INFO("Native host collection spawn replayed: tick {}, children {}; original parent payload and ordered local handles verified, temporary output buffer released",tick,expected.children.size());
            if(stop_after && expected.owner==*stop_after)return true;
            continue;
        }
        // Validate the outer preparation operation's real pending-shot list as well
        // as the captured parent, rather than stopping at constructor success.
        if(!ReadNativeEntitySnapshot(base,snapshot,error,address,false))return s.Stop("Native Shooter replay cannot verify its registered children");
        const auto count=PPC_LOAD_U32(shooter+84),data=PPC_LOAD_U32(shooter+80);
        if(count>kMaxSpawnChildren || (count && !Readable(data,size_t(count)*4,base)))return s.Stop("Native Shooter replay pending-shot list is malformed");
        for(size_t i=old_count;i<s.spawn_batches.size();++i){const auto& result=s.spawn_batches[i].result;
            if(std::all_of(result.begin(),result.end(),[](auto word){return word==0;}))continue;
            const auto record=std::find_if(snapshot.records.begin(),snapshot.records.end(),[&](const auto& row){return row.id==result;});
            bool listed=false;if(record!=snapshot.records.end())for(unsigned shot=0;shot<count;++shot)listed|=PPC_LOAD_U32(data+shot*4)==record->handle;
            if(!listed)return s.Stop("Native Shooter replay child is absent from its registered pending-shot list");
        }
        REXLOG_INFO("Native host Shooter burst replayed: tick {}, batches {}, pending shots {}; original preparation, child identities, parent payload and pending-shot registration verified",tick,s.spawn_batches.size()-old_count,count);
        if(stop_after && expected.owner==*stop_after)return true;
    }
    return true;
}
bool NativeRoomGameplay::FinishAiTargetDecision(size_t ticket,uint32_t brain,uint32_t setters,uint32_t caller,uint32_t target,uint8_t* base){
    Environment preserve;em2::GuestReadBatch reads;auto& s=*impl_;uint64_t tick=0;std::array<uint32_t,4> owner;
    if(!s.AiScope(brain,base,owner,tick))return false;
    if(s.peer!=1 || ticket>=s.ai_decisions.size() || s.ai_decisions[ticket].owner!=owner || setters>1 ||
       (setters && caller!=0x827F3350 && caller!=0x827F361C))return s.Stop("Native AI target selector produced unsupported side effects");
    auto& decision=s.ai_decisions[ticket];decision.setter=setters!=0;
    if(!decision.setter)return true;
    decision.write_selection=caller==0x827F361C;
    if(decision.write_selection){
        decision.selection=PPC_LOAD_U32(brain+184);
        if(decision.selection>4096 || decision.selection>PPC_LOAD_U32(brain+208))return s.Stop("Native AI selector ordinal exceeds local inventory");
    }
    if(target){
        decision.target_peer=s.HumanPeer(target);
        if(!decision.target_peer){
            if(!Readable(target,20,base) || PPC_LOAD_U32(target)!=0x8208D908)return s.Stop("Native AI target entity layout unsupported");
            for(unsigned i=0;i<4;++i)decision.target[i]=PPC_LOAD_U32(target+4+i*4);
            if(std::all_of(decision.target.begin(),decision.target.end(),[](auto word){return word==0;}))return s.Stop("Native AI target has no portable identity");
        }
    }
    return true;
}
bool NativeRoomGameplay::ReplayAiTargetDecision(PPCContext& ctx,uint8_t* base){
    Environment preserve;em2::GuestReadBatch reads;auto& s=*impl_;uint64_t tick=0;std::array<uint32_t,4> owner;
    if(s.peer==1 || !REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_probe_room_ai_targets))return false;
    if(!s.AiScope(ctx.r3.u32,base,owner,tick))return s.stopped;
    const auto fail=[&](const char* message){s.Stop(message);return true;};
    if(!s.pending_authority || s.ai_cursor>s.pending_authority->ai_decisions.size())return fail("Guest AI selector has no valid host decision batch");
    const auto& decisions=s.pending_authority->ai_decisions;
    if(s.ai_cursor==decisions.size() || decisions[s.ai_cursor].owner!=owner){
        const bool remaining=std::any_of(decisions.begin()+s.ai_cursor,decisions.end(),[&](const auto& command){return command.owner==owner;});
        const bool retiring=REXCVAR_GET(em2_probe_room_entity_retirements) && s.authority_entities.contains(owner) &&
            std::find(s.pending_authority->retired_entities.begin(),s.pending_authority->retired_entities.end(),owner)!=s.pending_authority->retired_entities.end();
        if(!remaining && retiring){
            REXLOG_INFO("Native retiring NPC selector skipped: tick {} owner {:08X}/{:08X}/{:08X}/{:08X}; explicit host retirement pending, no target choice or command consumed",tick,owner[0],owner[1],owner[2],owner[3]);
            return true;
        }
        REXLOG_ERROR("Native AI decision mismatch: tick {} local owner {:08X}/{:08X}/{:08X}/{:08X}, consumed {}/{}, host owner remaining {}, explicit retirement {}",
            tick,owner[0],owner[1],owner[2],owner[3],s.ai_cursor,decisions.size(),remaining,retiring);
        return fail(s.ai_cursor==decisions.size()?"Guest AI selector has no matching host decision":"Guest AI selector owner/order differs from host decision");
    }
    // Native target-change callbacks can enter a load. Keep this small command
    // independent of the pending packet's storage across those callbacks.
    const auto decision=s.pending_authority->ai_decisions[s.ai_cursor++];
    if(decision.owner!=owner)return fail("Guest AI selector owner/order differs from host decision");
    const auto brain=ctx.r3.u32;
    if(decision.write_selection && decision.selection>PPC_LOAD_U32(brain+208))return fail("Host AI selector ordinal does not fit guest inventory");
    uint32_t target=0,expected_handle=0;
    if(decision.setter && (decision.target_peer || std::any_of(decision.target.begin(),decision.target.end(),[](auto word){return word!=0;}))){
        NativeEntitySnapshot snapshot;const char* error=nullptr;uint32_t address=0;uint64_t before=0,after=0;
        if(!em2::SampleNativeEntityLifetimeRevision(before) || !ReadNativeEntitySnapshot(base,snapshot,error,address,false) ||
           !em2::SampleNativeEntityLifetimeRevision(after) || before!=after)return fail("Guest AI target registry changed during resolution");
        if(decision.target_peer){
            std::vector<OnlineNameActor> players;uint64_t local=0;
            if(!NameTagActors(players,local,base))return fail("Guest AI target lost player bindings");
            const auto player=std::find_if(players.begin(),players.end(),[&](const auto& value){return value.peer==decision.target_peer;});
            if(player==players.end())return fail("Host AI target player is absent from native roster");
            const auto record=std::find_if(snapshot.records.begin(),snapshot.records.end(),[&](const auto& value){return value.debug_address==player->entity;});
            if(record!=snapshot.records.end()){target=record->debug_address;expected_handle=record->handle;}
        }else{
            const auto record=std::lower_bound(snapshot.records.begin(),snapshot.records.end(),decision.target,[](const auto& value,const auto& id){return value.id<id;});
            if(record!=snapshot.records.end() && record->id==decision.target){target=record->debug_address;expected_handle=record->handle;}
        }
        if(!target || !expected_handle)return fail("Host AI target has no matching live guest entity");
    }
    if(decision.write_selection)PPC_STORE_U32(brain+184,decision.selection);
    if(decision.setter){
        ctx.r3.u64=brain;ctx.r4.u64=target;
        {em2::GuestReadBatch::CallbackBoundary callback;__imp__sub_827F2FA8(ctx,base);}
        if(!Readable(brain,128,base) || PPC_LOAD_U32(brain+124)!=expected_handle)return fail("Native AI target setter readback differs from host command");
    }
    REXLOG_INFO("Native host AI target decision applied: tick {} owner {:08X}/{:08X}/{:08X}/{:08X}, setter {}, player {}, local handle {:08X}, selection {}/{}; independent target selection skipped",
        tick,owner[0],owner[1],owner[2],owner[3],decision.setter,decision.target_peer,expected_handle,decision.write_selection,decision.selection);
    return true;
}
bool NativeRoomGameplay::OverridePrivateAiTarget(uint32_t brain,uint32_t& target,uint8_t* base){
    Environment preserve;em2::GuestReadBatch reads;auto& s=*impl_;uint64_t tick=0;std::array<uint32_t,4> owner;
    if(s.peer!=1 || !REXCVAR_GET(em2_probe_room_ai_target_drift) || !s.AiScope(brain,base,owner,tick) || tick<120 || tick>=180)return false;
    if(!Readable(s.label_entities[0],20,base))return s.Stop("Private AI target fixture lost Mickey");
    target=s.label_entities[0];
    if(tick==120)REXLOG_INFO("Private AI target fixture: tick {} owner {:08X}/{:08X}/{:08X}/{:08X} requests host player 1; guest receives host decision only",tick,owner[0],owner[1],owner[2],owner[3]);
    return true;
}
bool NativeRoomGameplay::RecordDamageEvent(uint32_t event,uint32_t listener,uint32_t caller,uint8_t* base){
    auto& s=*impl_;uint64_t tick=0;Environment preserve;em2::GuestReadBatch reads;
    const bool world_events=REXCVAR_GET(em2_probe_room_world_damage_events);
    if(!REXCVAR_GET(em2_private_profile) || (!REXCVAR_GET(em2_probe_room_player_damage_events) && !world_events) ||
       !s.Owner() || !s.Configured() || s.stopped || s.peer!=1 || !s.pending || s.load_depth || s.transition_pending ||
       !s.room->host_correction_mode() || !em2::CurrentAgreedSimulationTick(tick) || tick!=s.committed || !Readable(event,116,base) ||
       PPC_LOAD_U16(event+4)!=14 || PPC_LOAD_U32(event)!=0x82076C70)return false;
    const auto target=PPC_LOAD_U32(event+8);
    if(!target || (!world_events && target!=s.label_handles[0] && target!=s.label_handles[1] &&
       std::none_of(s.actors->actors().begin(),s.actors->actors().end(),[&](const auto& entry){return entry.actor.handle==target;})))return false;
    DamageEvent value;const char* error=nullptr;
    if(listener || s.damage_events.size()>=kAuthorityDamageBatch || !em2::CaptureNativeDamageEvent(base,event,value,error)){
        // Preserve the first unsupported native shape for mapping. Never treat
        // these words as portable: they may be handles or process-local data.
        const auto data=PPC_LOAD_U32(event+64),count=PPC_LOAD_U32(event+68);
        REXLOG_WARN("Private native Damage failure detail: tick {} caller {:08X} target {:08X} instigator {:08X} listener {:08X} auxiliary count {} capacity {} tag {:08X}",
            tick,caller,target,PPC_LOAD_U32(event+12),listener,count,PPC_LOAD_U32(event+72),PPC_LOAD_U32(event+76));
        for(uint32_t i=0;i<std::min(count,16u);++i){
            const auto address=uint64_t(data)+uint64_t(i)*4;
            if(address>UINT32_MAX || !Readable(uint32_t(address),4,base))break;
            REXLOG_WARN("Private native Damage auxiliary[{}] = {:08X}",i,PPC_LOAD_U32(uint32_t(address)));
        }
        s.Stop(std::string("Private native Damage capture unsupported: ")+(listener?"targeted listener":error?error:"event batch bound"));return false;
    }
    const auto player_peer=[&](uint32_t handle)->uint8_t{
        if(!handle)return 0;
        uint64_t peer=0;
        for(unsigned slot=0;slot<2;++slot)if(handle==s.label_handles[slot])peer=slot+1;
        if(!peer)for(const auto& actor:s.actors->actors())if(actor.actor.handle==handle){peer=actor.room_peer;break;}
        if(!peer || peer>16 || std::none_of(s.roster.players.begin(),s.roster.players.end(),[&](const auto& player){return player.peer==peer;}))return 0;
        return uint8_t(peer);
    };
    if((value.target_peer=player_peer(target)))value.target={};
    if((value.instigator_peer=player_peer(PPC_LOAD_U32(event+12))))value.instigator.reset();
    s.damage_events.push_back(std::move(value));return true;
}
bool NativeRoomGameplay::passive_input_pending()const{const auto& s=*impl_;return s.Owner() && !s.stopped && s.pending && s.delivered && (s.consumed_passive || s.game_over_entry.has_value());}
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

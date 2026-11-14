#include "online_local_source.h"
#include "online_room.h"
#include "online_room_bootstrap.h"
#include "online_local_jump.h"
#include "online_native_triggers.h"
#include "online_button_actions.h"
#include "online_movement_frame.h"
#include "controls.h"
#include <rex/input/input_system.h>
#include <rex/input/input_driver.h>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <optional>
#include <functional>
#include <thread>
#include <type_traits>
#include <cstring>
#include <bit>
#include <algorithm>

namespace em2::net {
using rex::X_STATUS;
using rex::X_RESULT;
namespace {
struct Environment {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(csr);SetLastError(error);}};
struct Route {
    bool active=false;DWORD thread=0;uint64_t generation=0,controls=0;
    unsigned physical=0;uint64_t peer=0;
    Identity identity;LobbySnapshot lobby;
    NativeLocalMovementBinding binding;
    std::optional<NativeRoutedPadSample> last;
    uint16_t blocked_buttons=0;bool release_fenced=false;
};
thread_local Route route;
thread_local uint64_t issued=0;
thread_local bool sampling=false;
bool BindingCurrent(const NativeLocalMovementBinding& expected,PPCContext& ctx,uint8_t* base){
    NativeLocalMovementBinding actual;
    return NativeLocalMovementBindingFor(ctx,base,actual) && actual==expected;
}
bool Current(const Room& room,PPCContext& ctx,uint8_t* base){
    return route.active && route.thread==GetCurrentThreadId() && route.controls && em2::SynchronizedControlsGeneration()==route.controls &&
        room.state()==RoomState::Running && room.identity()==route.identity && room.lobby()==route.lobby && room.local_peer()==route.peer &&
        BindingCurrent(route.binding,ctx,base);
}
}
bool ArmNativeLocalSource(const Room& room,unsigned physical,const NativeLocalMovementBinding& binding,PPCContext& ctx,uint8_t* base){
    Environment environment;
    if(sampling || physical>=2 || !room.lobby().locked ||
        room.identity().initialization_policy!=InitializationPolicy::SharedNativeV1 || room.local_peer()<1 || room.local_peer()>kOnlineRoomCapacity ||
        issued==UINT64_MAX)return false;
    const auto& players=room.lobby().players;
    const auto local=std::find_if(players.begin(),players.end(),[&](const LobbyPlayer& player){return player.peer==room.local_peer();});
    if(local==players.end() || (local->character!=LobbyCharacter::Mickey && local->character!=LobbyCharacter::Oswald) ||
        binding.retail_slot!=(local->character==LobbyCharacter::Mickey?0u:1u))return false;
    const auto controls=em2::SynchronizedControlsGeneration();if(!controls || !BindingCurrent(binding,ctx,base))return false;
    if(room.state()==RoomState::Bootstrapping){
        auto* gate=CurrentRoomBootstrapGate();uint64_t generation=0;Frame published;
        if(!gate || !gate->Owns(room) || !gate->completion_pending() || !gate->PublishedRetailFrame(generation,published) ||
            generation!=controls || published.tick!=room.bootstrap_tick())return false;
    }else if(room.state()!=RoomState::Running)return false;
    if(route.active)return route.thread==GetCurrentThreadId() && route.controls==controls && route.physical==physical &&
        route.peer==room.local_peer() && route.identity==room.identity() && route.lobby==room.lobby() && route.binding==binding;
    route={};route.active=true;route.thread=GetCurrentThreadId();route.generation=++issued;route.controls=controls;
    route.physical=physical;route.peer=room.local_peer();route.identity=room.identity();route.lobby=room.lobby();route.binding=binding;return true;
}
void DisarmNativeLocalSource(){route={};}
bool FenceNativeLocalSourceButtons(const Room& room,uint16_t held,PPCContext& ctx,uint8_t* base){
    Environment environment;
    if(sampling || !Current(room,ctx,base) || route.last || route.release_fenced)return false;
    route.blocked_buttons=held;route.release_fenced=true;return true;
}
bool SampleNativeLocalSource(const Room& room,rex::input::InputSystem& input,uint64_t tick,PPCContext& ctx,uint8_t* base,NativeRoutedPadSample& output){
    Environment environment;output={};if(sampling || !Current(room,ctx,base))return false;
    if(route.last && route.last->source_tick()==tick){output=*route.last;return true;}
    const auto generation=route.generation;const auto controls=route.controls;const auto physical=route.physical;
    struct Scope {bool previous=sampling;Scope(){sampling=true;}~Scope(){sampling=previous;}} scope;
    Pad pad;if(!em2::SampleLocalControls(input,physical,tick,pad) || route.generation!=generation || !Current(room,ctx,base))return false;
    // Stage fence progression only after the genuine SDK sample and complete
    // route revalidation succeed. Never rewrite the shared physical cache.
    const auto blocked=uint16_t(route.blocked_buttons&pad.buttons);
    pad.buttons=uint16_t(pad.buttons&~blocked);
    NativeRoutedPadSample sample;sample.pad_=pad;sample.route_generation_=generation;sample.controls_generation_=controls;
    sample.source_tick_=tick;sample.physical_user_=physical;sample.native_user_=route.binding.user;sample.retail_slot_=route.binding.retail_slot;
    sample.room_peer_=route.peer;
    route.blocked_buttons=blocked;route.last=sample;output=sample;return true;
}
bool ValidateNativeLocalSource(const Room& room,const NativeRoutedPadSample& sample,const NativeLocalMovementBinding& binding,PPCContext& ctx,uint8_t* base){
    Environment environment;
    return !sampling && Current(room,ctx,base) &&
        binding==route.binding && route.last && sample==*route.last && BindingCurrent(binding,ctx,base);
}
bool TestNativeLocalSource(PPCContext& ctx,uint8_t* base,const NativeLocalMovementBinding& binding){
    Environment environment;
    bool ok=true;unsigned checks=0;auto check=[&](bool value,const char* why){++checks;if(!value){REXLOG_ERROR("Native local source: {}",why);ok=false;}return value;};
    static_assert(!std::is_constructible_v<NativeRoutedPadSample,Pad>);
    if(!check(!route.active && !em2::SynchronizedControlsGeneration() && !RoomBootstrapActive() && binding.retail_slot==1 && binding.user==1 &&
        BindingCurrent(binding,ctx,base),"fixture owns real human Oswald slot1/nativeuser1 binding and no controls/source owner"))return false;
    struct ClockFixture {
        uint8_t* base;uint8_t initialized;uint32_t threshold;std::array<uint8_t,16> a;std::array<uint8_t,12> b;
        ClockFixture(uint8_t* p):base(p),initialized(PPC_LOAD_U8(0x83396930)),threshold(PPC_LOAD_U32(0x833B65F8)){
            std::memcpy(a.data(),base+0x833D03C0,a.size());std::memcpy(b.data(),base+0x833D04D8,b.size());
            PPC_STORE_U8(0x83396930,0);PPC_STORE_U64(0x833D03C0,100000000);PPC_STORE_U64(0x833D03C8,1000003);
            PPC_STORE_U64(0x833D04D8,1000003);PPC_STORE_U32(0x833D04E0,std::bit_cast<uint32_t>(float(1./1000003.)));PPC_STORE_U32(0x833B65F8,32);
        }
        ~ClockFixture(){std::memcpy(base+0x833D03C0,a.data(),a.size());std::memcpy(base+0x833D04D8,b.data(),b.size());
            PPC_STORE_U8(0x83396930,initialized);PPC_STORE_U32(0x833B65F8,threshold);}
    }clock_fixture{base};
    std::unique_ptr<Room> host,client;
    RoomBootstrapGate gate;
    struct Cleanup {RoomBootstrapGate& gate;~Cleanup(){DisarmNativeLocalSource();DisarmNativeLocalJump();if(gate.active())gate.Detach();else em2::EndSynchronizedControls();}} cleanup{gate};
    Identity identity;identity.build[0]=1;identity.content[0]=2;identity.checkpoint[0]=3;identity.session[0]=4;
    identity.initialization_policy=InitializationPolicy::SharedNativeV1;identity.input_delay=0;identity.first_tick=40;
    auto pump=[&](auto ready){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);do{
        host->Poll();client->Poll();if(ready())return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }while(std::chrono::steady_clock::now()<end);return false;};
    auto connect=[&](bool bootstrap){
        host=std::make_unique<Room>();client=std::make_unique<Room>();
        if(!host->Host(identity,"127.0.0.1",0,2,"Host") || !client->Join(identity,"127.0.0.1",host->port(),"Guest"))return false;
        if(!pump([&]{return host->lobby().players.size()==2 && client->lobby().players.size()==2;}) || !host->SetReady(true) || !client->SetReady(true))return false;
        if(!pump([&]{return !client->profile_pending() && host->lobby().players[1].ready;}))return false;
        const bool started=bootstrap?host->StartNativeBootstrap():host->Start();if(!started || !pump([&]{return client->state()==RoomState::Loading;}))return false;
        std::array<uint8_t,32> token{};token[0]=9;
        if(bootstrap){if(!host->BootstrapWorldReady(token) || !client->BootstrapWorldReady(token))return false;}
        else if(!host->WorldReady(token) || !client->WorldReady(token))return false;
        return pump([&]{return host->state()==(bootstrap?RoomState::Bootstrapping:RoomState::Running) && client->state()==host->state();});
    };
    class Device final:public rex::input::InputDriver {
    public:
        std::array<rex::input::X_INPUT_GAMEPAD,2> pads{};std::array<unsigned,2> reads{};std::function<void()> on_poll;
        Device():InputDriver(nullptr,0){}
        rex::X_STATUS Setup()override{return X_STATUS_SUCCESS;}
        rex::X_RESULT GetCapabilities(uint32_t,uint32_t,rex::input::X_INPUT_CAPABILITIES*)override{return X_ERROR_DEVICE_NOT_CONNECTED;}
        rex::X_RESULT GetState(uint32_t user,rex::input::X_INPUT_STATE* state)override{
            if(user>=2)return X_ERROR_DEVICE_NOT_CONNECTED;++reads[user];if(on_poll)on_poll();if(state){*state={};state->gamepad=pads[user];}return X_ERROR_SUCCESS;
        }
        rex::X_RESULT SetState(uint32_t,rex::input::X_INPUT_VIBRATION*)override{return X_ERROR_DEVICE_NOT_CONNECTED;}
        rex::X_RESULT GetKeystroke(uint32_t,uint32_t,rex::input::X_INPUT_KEYSTROKE*)override{return X_ERROR_EMPTY;}
    };
    rex::input::InputSystem input(nullptr);auto driver=std::make_unique<Device>();auto* hardware=driver.get();input.AddDriver(std::move(driver));
    hardware->pads[0].thumb_lx=23000;hardware->pads[0].thumb_ly=-14000;hardware->pads[0].buttons=0x1000;hardware->pads[1].thumb_lx=-30000;
    NativeRoutedPadSample sample;
    if(!check(connect(true) && gate.Attach(*client) && gate.SetJoining(false),"real guest Room owns agreed raw bootstrap gate"))return false;
    check(!ArmNativeLocalSource(*client,0,binding,ctx,base),"source route cannot arm before a committed native round boundary");
    check(host->SubmitBootstrapPad(identity.first_tick,{}),"host supplies neutral native-start input");
    RoomBootstrapFrame raw;BootstrapAdmission admission=BootstrapAdmission::Waiting;
    if(!check(pump([&]{admission=gate.Prepare(raw);return admission==BootstrapAdmission::Ready;}) && gate.Commit(raw),"fixture publishes canonical native controls before source readiness eligibility"))return false;
    check(ArmNativeLocalSource(*client,0,binding,ctx,base) && ArmNativeLocalSource(*client,0,binding,ctx,base),"source0 to nativeOswald1 route arms idempotently at owning completed-boundary fixture without polling");
    check(!SampleNativeLocalSource(*client,input,identity.first_tick+1,ctx,base,sample) && hardware->reads[0]==0 && hardware->reads[1]==0,
        "arming does not poll hardware or certify gameplay-ready before unanimous cutover");
    DisarmNativeLocalSource();gate.Detach();
    // A direct controlled Running fixture now exercises actual physical sample
    // provenance. This test never casts a raw frame into interpreted Movement,
    // installs a readiness probe, or emits a bootstrap gameplay-ready vote.
    identity.first_tick=1000;
    if(!check(connect(false) && em2::BeginSynchronizedControls(identity.first_tick-1),"direct running fixture owns one local sampler generation"))return false;
    // The real menu/bootstrap settles the existing release-before-input gate.
    // Do that through the actual sampler here too; never bypass its guard to
    // manufacture a first press. Source-read counts below start after baseline.
    const auto configured_pad=hardware->pads[0];hardware->pads[0]={};Pad baseline;
    if(!check(em2::SampleLocalControls(input,0,identity.first_tick-1,baseline) && baseline==Pad{},"neutral physical baseline settles the real sampler release gate"))return false;
    hardware->pads[0]=configured_pad;hardware->reads={};
    check(!ArmNativeLocalSource(*host,0,binding,ctx,base) && !ArmNativeLocalSource(*client,2,binding,ctx,base),"retail slot/local-room-peer mismatch and nonexistent physical user rejected");
    if(!check(ArmNativeLocalSource(*client,0,binding,ctx,base) && SampleNativeLocalSource(*client,input,1000,ctx,base,sample),"actual physical0 poll mints canonical nativeuser1 Oswald source token"))return false;
    Pad cached;check(sample.physical_user()==0 && sample.native_user()==1 && sample.retail_slot()==1 && sample.source_tick()==1000 &&
        em2::SampleLocalControls(input,0,1000,cached) && sample.pad()==cached && hardware->reads[0]==1 && hardware->reads[1]==0,
        "sealed sample records distinct real hardware/canonical users and exact already-remapped physical0 cache");
    NativeRoutedPadSample again;check(SampleNativeLocalSource(*client,input,1000,ctx,base,again) && again==sample && hardware->reads[0]==1 &&
        !SampleNativeLocalSource(*client,input,1002,ctx,base,again),"same tick reuses exact token without repoll; source tick gaps reject");
    NativeLocalJumpBinding jump;
    if(!check(DiscoverNativeLocalJumpBinding(binding,ctx,base,jump) && ArmNativeLocalJump(jump,1,1000,ctx,base),"typed source fixture arms original native Jump math at correct source tick"))return false;
    NativeTriggerBinding button_binding;NativeTriggerSource button_source;NativeTriggerBatch button_batch;
    if(!check(DiscoverNativeTriggerBinding(binding,ctx,base,button_binding,NativeButtonScope::DigitalAndTriggers),"native Oswald digital binding discovery") ||
       !check(button_binding.actions.size()==2,"native Oswald digital binding has exact Jump pair") ||
       !check(ArmNativeTriggers(button_source,button_binding,1,1000,ctx,base),"native Oswald button source arms from neutral live history") ||
       !check(ProduceNativeTriggers(*client,button_source,button_binding,sample,1,ctx,base,button_batch),"sealed native Oswald button interpreter completes"))return false;
    if(button_batch.count!=2 || !button_batch.actions[0].dispatch || button_batch.actions[1].dispatch)
        REXLOG_ERROR("Native button source fixture: remapped buttons {:04X}, count {}, first '{}' kind {} dispatch {}, second '{}' kind {} dispatch {}",
            sample.pad().buttons,button_batch.count,button_binding.actions[0].name,button_binding.actions[0].kind,button_batch.actions[0].dispatch,
            button_binding.actions[1].name,button_binding.actions[1].kind,button_batch.actions[1].dispatch);
    if(!check(button_batch.count==2 && button_batch.actions[0].dispatch && !button_batch.actions[1].dispatch && hardware->reads[0]==1 && hardware->reads[1]==0,
        "sealed real hardware0 sample drives original Oswald nativeuser1 digital press without repolling or relabelling a raw Pad"))return false;
    RoomButtonActions interpreted_buttons;std::vector<NativeActorAction> matched_buttons;
    if(!check(EncodeNativeButtonActions(button_binding,button_batch,interpreted_buttons) &&
        MatchNativeButtonActions(button_binding,interpreted_buttons,matched_buttons) && matched_buttons.size()==1 &&
        matched_buttons[0].action==jump.actions[0].action,"real native button output encodes and matches the local Oswald definition schema"))return false;
    if(!check(!ProduceNativeTriggers(button_source,button_binding,sample.pad(),0,1,1000,ctx,base,button_batch) &&
        !ProduceNativeTriggers(*host,button_source,button_binding,sample,1,ctx,base,button_batch) &&
        !ProduceNativeTriggers(*client,button_source,button_binding,NativeRoutedPadSample{},1,ctx,base,button_batch),
        "raw physical-user shortcut, wrong Room and unissued source token cannot authorize native button production"))return false;
    RoomMovement movement;NativeLocalJumpEvents events;
    check(ProduceNativeLocalRoomMovement(*client,binding,sample,1,ctx,base,movement) && movement.pad==sample.pad() && ValidRoomMovement(movement) &&
        ProduceNativeLocalJump(*client,jump,sample,1,ctx,base,events),"typed Movement and Jump consume identical physical0 proof through nativeuser1 common math");
    auto produced=movement;produced.source=RoomMovementSource::NativeActionV3;produced.jump_button_mask=events.button_mask;produced.jump_count=events.count;produced.jump_events=events.events;
    produced.button_actions=interpreted_buttons;
    RoomMovement neutral;neutral.source=RoomMovementSource::NativeActionV1;neutral.actor_epoch=1;
    check(host->SubmitLocal(1000,neutral) && client->SubmitLocal(1000,produced),"sealed source0/native1 output enters actual authoritative Room frame without relabelling raw input");
    RoomFrame authoritative,received;
    if(!check(pump([&]{return host->PeekFrame(authoritative) && client->PeekFrame(received);}) && host->TakeFrame(authoritative) &&
        client->TakeFrame(received) && authoritative==received && received.players_mask==3 && received.players[1]==produced,
        "real TCP receiver preserves produced native Movement and ordered Jump provenance"))return false;
    std::array<uint8_t,32> fixture_digest{};fixture_digest[0]=0xC0;
    check(host->CompleteGameplay(1000,fixture_digest) && client->CompleteGameplay(1000,fixture_digest) &&
        pump([&]{return !host->gameplay_completion_pending() && !client->gameplay_completion_pending() &&
            host->verified_gameplay_tick()==1000 && client->verified_gameplay_tick()==1000;}),
        "source/math/network fixture processing explicitly acknowledges consumed frame; not a full native-world observation claim");
    check(!ProduceNativeLocalRoomMovement(binding,sample.pad(),1,ctx,base,movement,sample.physical_user()) &&
        !ProduceNativeLocalJump(jump,sample.pad(),1,1000,ctx,base,events,sample.physical_user()),"strict original producer APIs still reject physical0 pretending to be canonicalnative1");
    auto wrong=binding;++wrong.user;
    check(ValidateNativeLocalSource(*client,sample,binding,ctx,base) && !ValidateNativeLocalSource(*host,sample,binding,ctx,base) &&
        !ValidateNativeLocalSource(*client,NativeRoutedPadSample{},binding,ctx,base) && !ValidateNativeLocalSource(*client,sample,wrong,ctx,base),
        "token validation requires live Room identity/peer, issued cache and complete current native binding");
    bool foreign_rejected=false;std::thread foreign([&]{PPCContext other=ctx;foreign_rejected=!ValidateNativeLocalSource(*client,sample,binding,other,base);});foreign.join();
    check(foreign_rejected,"foreign thread has no source/control lease");
    hardware->pads[0].thumb_lx=-22000;
    NativeRoutedPadSample next;check(SampleNativeLocalSource(*client,input,1001,ctx,base,next) && hardware->reads[0]==2 && hardware->reads[1]==0 &&
        !ValidateNativeLocalSource(*client,sample,binding,ctx,base) && ValidateNativeLocalSource(*client,next,binding,ctx,base),"new source tick invalidates older token without polling canonicaldevice1");
    check(!ProduceNativeTriggers(*client,button_source,button_binding,sample,1,ctx,base,button_batch) &&
        ProduceNativeTriggers(*client,button_source,button_binding,next,1,ctx,base,button_batch) && !button_batch.actions[0].dispatch && !button_batch.actions[1].dispatch,
        "stale physical token cannot advance button histories; the new held sample executes the original interpreter without a second press");
    hardware->on_poll=[] {DisarmNativeLocalSource();};
    check(!SampleNativeLocalSource(*client,input,1002,ctx,base,again) && hardware->reads[0]==3,"route teardown during SDK poll cannot mint stale provenance");hardware->on_poll={};
    check(!ValidateNativeLocalSource(*client,next,binding,ctx,base) && ArmNativeLocalSource(*client,0,binding,ctx,base) &&
        !ValidateNativeLocalSource(*client,next,binding,ctx,base),"rearm creates a new opaque route generation");
    check(SampleNativeLocalSource(*client,input,1002,ctx,base,again) && hardware->reads[0]==3,"successful same-tick sampler retry uses cached physical sample, not a second poll");
    const auto old_generation=em2::SynchronizedControlsGeneration();check(em2::EndSynchronizedControls(old_generation) && em2::BeginSynchronizedControls(1003) &&
        !ValidateNativeLocalSource(*client,again,binding,ctx,base),"replacement synchronized-controls generation invalidates all source tokens");
    DisarmNativeLocalSource();check(ArmNativeLocalSource(*client,0,binding,ctx,base) && SampleNativeLocalSource(*client,input,1003,ctx,base,again),"new controls lease may establish a fresh validated route");
    client->Disconnect();check(!ValidateNativeLocalSource(*client,again,binding,ctx,base),"cached source token cannot survive Room disconnect");
    DisarmNativeLocalSource();DisarmNativeLocalJump();em2::EndSynchronizedControls();
    // Real sixteen-peer transport, one sealed physical producer at peer16.
    // Other participants submit explicit fixture neutral; no sixteen-world claim.
    Room cohort_host;std::array<std::unique_ptr<Room>,15> cohort;
    auto cohort_identity=identity;cohort_identity.first_tick=2001;cohort_identity.session[1]=16;
    if(!check(cohort_host.Host(cohort_identity,"127.0.0.1",0,16,"Host"),"sixteen-peer source fixture hosts"))return false;
    for(unsigned i=0;i<cohort.size();++i){cohort[i]=std::make_unique<Room>();
        if(!cohort[i]->Join(cohort_identity,"127.0.0.1",cohort_host.port(),"Peer"+std::to_string(i+2)))return false;}
    auto cohort_pump=[&](auto ready){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);do{
        cohort_host.Poll();for(auto& peer:cohort)peer->Poll();if(ready())return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }while(std::chrono::steady_clock::now()<end);return false;};
    if(!check(cohort_pump([&]{return cohort_host.lobby().players.size()==16 && std::all_of(cohort.begin(),cohort.end(),[](const auto& peer){return peer->lobby().players.size()==16;});}),
        "all sixteen real TCP participants observe the full roster"))return false;
    if(!cohort_host.SetReady(true))return false;for(auto& peer:cohort)if(!peer->SetReady(true))return false;
    if(!cohort_pump([&]{return std::all_of(cohort_host.lobby().players.begin(),cohort_host.lobby().players.end(),[](const auto& player){return player.ready;}) &&
        std::all_of(cohort.begin(),cohort.end(),[](const auto& peer){return !peer->profile_pending();});}) || !cohort_host.Start() ||
       !cohort_pump([&]{return std::all_of(cohort.begin(),cohort.end(),[](const auto& peer){return peer->state()==RoomState::Loading;});}))return false;
    std::array<uint8_t,32> cohort_ready{};cohort_ready[0]=16;
    if(!cohort_host.WorldReady(cohort_ready))return false;for(auto& peer:cohort)if(!peer->WorldReady(cohort_ready))return false;
    if(!cohort_pump([&]{return cohort_host.state()==RoomState::Running && std::all_of(cohort.begin(),cohort.end(),[](const auto& peer){return peer->state()==RoomState::Running;});}))return false;
    auto final=std::find_if(cohort.begin(),cohort.end(),[](const auto& peer){return peer->local_peer()==16;});
    if(!check(final!=cohort.end() && em2::BeginSynchronizedControls(2000),"peer16 uses a bounded native controls lease, not SDK user15"))return false;
    hardware->pads[0]={};Pad cohort_baseline;
    if(!em2::SampleLocalControls(input,0,2000,cohort_baseline))return false;
    hardware->pads[0]=configured_pad;hardware->reads={};NativeRoutedPadSample final_sample;
    if(!check(ArmNativeLocalSource(**final,0,binding,ctx,base) && SampleNativeLocalSource(**final,input,2001,ctx,base,final_sample) &&
        final_sample.room_peer()==16 && final_sample.physical_user()==0 && final_sample.native_user()==1 && final_sample.retail_slot()==1 && hardware->reads[0]==1 && hardware->reads[1]==0,
        "peer16 owns authentic physical0/nativeOswald1 provenance without inventing a sixteenth engine controller"))return false;
    NativeLocalJumpEvents final_edges;RoomMovement final_movement;
    if(!check(ArmNativeLocalJump(jump,1,2001,ctx,base) && ProduceNativeLocalRoomMovement(**final,binding,final_sample,1,ctx,base,final_movement) &&
        ProduceNativeLocalJump(**final,jump,final_sample,1,ctx,base,final_edges),"peer16 source runs original native Movement and Jump interpretation with the frozen wire actor epoch"))return false;
    final_movement.source=RoomMovementSource::NativeActionV2;final_movement.jump_button_mask=final_edges.button_mask;
    final_movement.jump_count=final_edges.count;final_movement.jump_events=final_edges.events;
    RoomMovement cohort_neutral;cohort_neutral.actor_epoch=1;cohort_neutral.source=RoomMovementSource::NativeActionV1;
    if(!cohort_host.SubmitLocal(2001,cohort_neutral))return false;
    for(auto& peer:cohort)if(!peer->SubmitLocal(2001,peer->local_peer()==16?final_movement:cohort_neutral))return false;
    RoomFrame cohort_frame;
    if(!check(cohort_pump([&]{return cohort_host.PeekFrame(cohort_frame);}) && cohort_frame.players_mask==0xFFFF && cohort_frame.players[15]==final_movement &&
        !ValidateNativeLocalSource(cohort_host,final_sample,binding,ctx,base),"full authoritative sixteen-peer frame preserves the sealed peer16 commands and rejects host impersonation"))return false;
    // A fresh context must not manufacture Jump from an already-held skip
    // button. Exercise the sealed source and BOTH original native producers.
    const auto held=final_sample.pad().buttons;
    if(!check(held==jump.button_mask && !FenceNativeLocalSourceButtons(**final,held,ctx,base),"sampled active route cannot retrofit a release fence"))return false;
    DisarmNativeLocalSource();DisarmNativeLocalJump();
    if(!check(!FenceNativeLocalSourceButtons(**final,held,ctx,base) && ArmNativeLocalSource(**final,0,binding,ctx,base) &&
        !FenceNativeLocalSourceButtons(cohort_host,held,ctx,base) && FenceNativeLocalSourceButtons(**final,held,ctx,base) &&
        !FenceNativeLocalSourceButtons(**final,held,ctx,base),"context fence requires a fresh exact Room route and installs only once"))return false;
    NativeTriggerSource fenced_buttons;
    if(!ArmNativeLocalJump(jump,3,2002,ctx,base) || !ArmNativeTriggers(fenced_buttons,button_binding,3,2002,ctx,base))return false;
    for(uint64_t tick=2002;tick<=2006;++tick){
        hardware->pads[0]=configured_pad;
        if(tick>=2003)hardware->pads[0].buttons=uint16_t(uint16_t(hardware->pads[0].buttons)|0x4000);
        if(tick==2004 || tick==2006)hardware->pads[0].buttons=0x4000;
        NativeRoutedPadSample fenced,repeated;Pad raw;
        if(!check(SampleNativeLocalSource(**final,input,tick,ctx,base,fenced) && em2::SampleLocalControls(input,0,tick,raw),"context-fenced source uses the real cached physical sample"))return false;
        auto expected=raw;if(tick<=2003)expected.buttons=uint16_t(expected.buttons&~held);
        if(!check(fenced.pad()==expected && SampleNativeLocalSource(**final,input,tick,ctx,base,repeated) && repeated==fenced &&
            hardware->reads[0]==unsigned(tick-2000) && hardware->reads[1]==0 && ValidateNativeLocalSource(**final,fenced,binding,ctx,base),
            "fence preserves axes/triggers/fresh buttons, raw cache, once-only polling and sealed provenance"))return false;
        if(tick==2003 && !check((raw.buttons&~held)!=0 && (fenced.pad().buttons&held)==0,"new digital action is forwarded while the old held action is blocked"))return false;
        NativeLocalJumpEvents edges;NativeTriggerBatch fired;
        if(!check(ProduceNativeLocalJump(**final,jump,fenced,3,ctx,base,edges) &&
            ProduceNativeTriggers(**final,fenced_buttons,button_binding,fenced,3,ctx,base,fired),"both original Jump and complete-button interpreters use the fenced sample"))return false;
        if(tick<2005){if(!check(!edges.count && !fired.actions[0].dispatch && !fired.actions[1].dispatch,"carried press and its first physical release produce no synthetic Jump edges"))return false;}
        else if(tick==2005){if(!check(edges.count==1 && edges.events[0]==RoomJumpEvent{1,RoomJumpEdge::Pressed} && fired.actions[0].dispatch && !fired.actions[1].dispatch,
            "a new post-release press reaches both original native interpreters exactly once"))return false;}
        else if(!check(edges.count==1 && edges.events[0]==RoomJumpEvent{2,RoomJumpEdge::Released} && !fired.actions[0].dispatch && fired.actions[1].dispatch,
            "post-release press retains its normal original release edge"))return false;
    }
    if(ok)REXLOG_INFO("Native local source passed {} checks: actual hardware0/canonicalOswald1, raw-boundary arm without polling/vote, sealed tick/control/binding provenance, typed native Movement/Jump, strict API mismatch rejection, cache/reentrancy/thread/Room lifecycle and context-held button release fence",checks);
    return ok;
}
}

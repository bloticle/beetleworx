#include "online_local_source.h"
#include "online_room.h"
#include "online_room_bootstrap.h"
#include "online_local_jump.h"
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
        room.identity().initialization_policy!=InitializationPolicy::SharedNativeV1 || room.local_peer()<1 || room.local_peer()>2 ||
        binding.retail_slot!=room.local_peer()-1 || issued==UINT64_MAX)return false;
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
bool SampleNativeLocalSource(const Room& room,rex::input::InputSystem& input,uint64_t tick,PPCContext& ctx,uint8_t* base,NativeRoutedPadSample& output){
    Environment environment;output={};if(sampling || !Current(room,ctx,base))return false;
    if(route.last && route.last->source_tick()==tick){output=*route.last;return true;}
    const auto generation=route.generation;const auto controls=route.controls;const auto physical=route.physical;
    struct Scope {bool previous=sampling;Scope(){sampling=true;}~Scope(){sampling=previous;}} scope;
    Pad pad;if(!em2::SampleLocalControls(input,physical,tick,pad) || route.generation!=generation || !Current(room,ctx,base))return false;
    NativeRoutedPadSample sample;sample.pad_=pad;sample.route_generation_=generation;sample.controls_generation_=controls;
    sample.source_tick_=tick;sample.physical_user_=physical;sample.native_user_=route.binding.user;sample.retail_slot_=route.binding.retail_slot;
    route.last=sample;output=sample;return true;
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
    hardware->pads[0].thumb_lx=23000;hardware->pads[0].thumb_ly=-14000;hardware->pads[1].thumb_lx=-30000;
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
    if(!check(connect(false) && em2::BeginSynchronizedControls(identity.first_tick),"direct running fixture owns one local sampler generation"))return false;
    check(!ArmNativeLocalSource(*host,0,binding,ctx,base) && !ArmNativeLocalSource(*client,2,binding,ctx,base),"retail slot/local-room-peer mismatch and nonexistent physical user rejected");
    if(!check(ArmNativeLocalSource(*client,0,binding,ctx,base) && SampleNativeLocalSource(*client,input,1000,ctx,base,sample),"actual physical0 poll mints canonical nativeuser1 Oswald source token"))return false;
    Pad cached;check(sample.physical_user()==0 && sample.native_user()==1 && sample.retail_slot()==1 && sample.source_tick()==1000 &&
        em2::SampleLocalControls(input,0,1000,cached) && sample.pad()==cached && hardware->reads[0]==1 && hardware->reads[1]==0,
        "sealed sample records distinct real hardware/canonical users and exact already-remapped physical0 cache");
    NativeRoutedPadSample again;check(SampleNativeLocalSource(*client,input,1000,ctx,base,again) && again==sample && hardware->reads[0]==1 &&
        !SampleNativeLocalSource(*client,input,1002,ctx,base,again),"same tick reuses exact token without repoll; source tick gaps reject");
    NativeLocalJumpBinding jump;
    if(!check(DiscoverNativeLocalJumpBinding(binding,ctx,base,jump) && ArmNativeLocalJump(jump,1,1000,ctx,base),"typed source fixture arms original native Jump math at correct source tick"))return false;
    RoomMovement movement;NativeLocalJumpEvents events;
    check(ProduceNativeLocalRoomMovement(*client,binding,sample,1,ctx,base,movement) && movement.pad==sample.pad() && ValidRoomMovement(movement) &&
        ProduceNativeLocalJump(*client,jump,sample,1,ctx,base,events),"typed Movement and Jump consume identical physical0 proof through nativeuser1 common math");
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
    hardware->on_poll=[] {DisarmNativeLocalSource();};
    check(!SampleNativeLocalSource(*client,input,1002,ctx,base,again) && hardware->reads[0]==3,"route teardown during SDK poll cannot mint stale provenance");hardware->on_poll={};
    check(!ValidateNativeLocalSource(*client,next,binding,ctx,base) && ArmNativeLocalSource(*client,0,binding,ctx,base) &&
        !ValidateNativeLocalSource(*client,next,binding,ctx,base),"rearm creates a new opaque route generation");
    check(SampleNativeLocalSource(*client,input,1002,ctx,base,again) && hardware->reads[0]==3,"successful same-tick sampler retry uses cached physical sample, not a second poll");
    const auto old_generation=em2::SynchronizedControlsGeneration();check(em2::EndSynchronizedControls(old_generation) && em2::BeginSynchronizedControls(1003) &&
        !ValidateNativeLocalSource(*client,again,binding,ctx,base),"replacement synchronized-controls generation invalidates all source tokens");
    DisarmNativeLocalSource();check(ArmNativeLocalSource(*client,0,binding,ctx,base) && SampleNativeLocalSource(*client,input,1003,ctx,base,again),"new controls lease may establish a fresh validated route");
    client->Disconnect();check(!ValidateNativeLocalSource(*client,again,binding,ctx,base),"cached source token cannot survive Room disconnect");
    if(ok)REXLOG_INFO("Native local source passed {} checks: actual hardware0/canonicalOswald1, raw-boundary arm without polling/vote, sealed tick/control/binding provenance, typed native Movement/Jump, strict API mismatch rejection, cache/reentrancy/thread/Room lifecycle",checks);
    return ok;
}
}

#include "online_room_runtime.h"
#include "online_world_random.h"
#include "allocation_trace.h"
#include "controls.h"
#include "players.h"
#include "native_random.h"
#include "simulation.h"
#include "game_console.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/sha256.h>
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <charconv>
#include <mutex>
#include <optional>
#include <thread>

REXCVAR_DECLARE(bool,em2_online_auto_continue);
namespace em2::net {
namespace {
using Clock=std::chrono::steady_clock;
std::atomic<bool> retained_room_active{false};
struct Environment {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(csr);SetLastError(error);}};
struct Owner {
    std::unique_ptr<Room> room;
    RoomBootstrapGate gate;
    NativeInitializationContext initialization;
    std::jthread prelaunch;
    std::atomic<uint64_t> prelaunch_polls{0};
    std::atomic<DWORD> thread{0};
    std::atomic<bool> loaded_waiting{false};
    std::atomic<bool> failure_trace_dumped{false};
    unsigned device=0;
    bool profile_claimed=false,continue_claimed=false,world_reported=false;
    std::optional<std::array<uint8_t,32>> loaded_world;
    RoomBootstrapGate::ReadinessProbe readiness=nullptr;
    std::optional<Clock::time_point> loading_poll;
    std::mutex status_mutex;NativeSessionStatus status;
    void StopPrelaunch(){if(prelaunch.joinable()){prelaunch.request_stop();prelaunch.join();}}
    ~Owner(){StopPrelaunch();}
    NativeSessionStatus Status(){std::lock_guard lock(status_mutex);return status;}
    void Publish(NativeSessionState state,std::string error={},InputWait wait=InputWait::None){
        std::lock_guard lock(status_mutex);status.state=state;status.error=std::move(error);status.wait=wait;
        if(state==NativeSessionState::None)status.continue_requested=status.profile_requested=status.save_notice_requested=false;
    }
    void Fail(std::string error){
        // The retained history is simulation-thread TLS. A prelaunch transport
        // worker must never inspect it or consume the eventual owner's dump.
        // This reads recorded values only, without a guest callback/context.
        if(thread.load(std::memory_order_acquire)==GetCurrentThreadId() &&
           !failure_trace_dumped.exchange(true,std::memory_order_acq_rel)){
            Environment preserve_diagnostics;
            try{em2::DumpOnlineRandomTraceHistory();}catch(...){/* Keep the original failure authoritative. */}
        }
        initialization.Fail();loaded_waiting.store(false,std::memory_order_release);
        if(room)room->Disconnect();
        if(Status().state!=NativeSessionState::Failed)REXLOG_ERROR("Retained native Room stopped: {}",error);
        Publish(NativeSessionState::Failed,std::move(error));
        // Keep the gate and borrowed Room alive. Failed startup remains halted
        // with its last agreed controls/RNG lease until explicit shutdown.
    }
};
Owner& Runtime(){static Owner owner;return owner;}
bool Bound(const Owner& owner){return owner.thread.load(std::memory_order_acquire)==GetCurrentThreadId();}
bool ClaimThread(Owner& owner){
    DWORD empty=0;owner.thread.compare_exchange_strong(empty,GetCurrentThreadId(),std::memory_order_acq_rel);
    if(!Bound(owner))return false;
    owner.StopPrelaunch();return owner.initialization.BindThread();
}
bool Frozen(const Room& room){
    if(room.state()!=RoomState::Loading || !room.native_bootstrap() || room.identity().initialization_policy!=InitializationPolicy::SharedNativeV1)return false;
    const auto& lobby=room.lobby();if(!lobby.locked || lobby.players.size()<2 || lobby.players.size()>kOnlineRoomCapacity || lobby.capacity<lobby.players.size() || lobby.capacity>kOnlineRoomCapacity)return false;
    uint16_t mask=0;uint64_t previous=0;std::array<bool,kOnlineRoomCapacity> colors{};
    for(const auto& player:lobby.players){
        if(!player.peer || player.peer>lobby.capacity || player.peer<=previous || !player.ready || player.color>=colors.size() || colors[player.color])return false;
        colors[player.color]=true;previous=player.peer;mask|=uint16_t(1u<<(player.peer-1));
    }
    return (mask&3)==3 && room.local_peer() && room.local_peer()<=kOnlineRoomCapacity && (mask&(1u<<(room.local_peer()-1)));
}
std::optional<std::array<uint8_t,32>> LoadedWorldToken(const Room& room){
    const auto level=em2::CurrentNativeLoadLevel();const auto end=std::find(level.begin(),level.end(),char(0));
    if(end==level.end() || end==level.begin())return {};
    const std::string_view name(level.data(),size_t(end-level.begin()));
    if(std::any_of(name.begin(),name.end(),[](unsigned char c){return c<32 || c>=127;}))return {};
    std::string bytes="Beetleworx-native-room-loaded-world-v1";
    const auto& id=room.identity();
    auto put=[&](uint64_t value,unsigned size){for(unsigned i=size;i;--i)bytes.push_back(char(value>>(8*(i-1))));};
    for(const auto* hash:{&id.build,&id.content,&id.checkpoint})bytes.append(reinterpret_cast<const char*>(hash->data()),hash->size());
    bytes.append(reinterpret_cast<const char*>(id.session.data()),id.session.size());put(id.first_tick,8);put(id.input_delay,1);
    put(uint8_t(id.startup_policy),1);put(uint8_t(id.initialization_policy),1);put(name.size(),1);bytes.append(name);
    for(const auto& player:room.lobby().players){put(player.peer,8);put(uint8_t(player.character),1);put(player.color,1);}
    const auto hex=rex::util::sha256(bytes);if(hex.size()!=64)return {};
    std::array<uint8_t,32> token{};
    for(unsigned i=0;i<token.size();++i){unsigned value=0;const auto parsed=std::from_chars(hex.data()+i*2,hex.data()+i*2+2,value,16);if(parsed.ec!=std::errc{} || parsed.ptr!=hex.data()+i*2+2)return {};token[i]=uint8_t(value);}
    return token;
}
bool PollOwned(Owner& owner,Clock::time_point now){
    if(!owner.room || owner.Status().state==NativeSessionState::Failed)return false;
    owner.room->Poll(now);
    const auto state=owner.room->state();
    if(state!=RoomState::Loading && state!=RoomState::Bootstrapping && state!=RoomState::Running){
        owner.Fail(owner.room->error().empty()?"The Room disconnected during native startup.":owner.room->error());return false;
    }
    if(owner.gate.active() && !owner.gate.error().empty()){owner.Fail(owner.gate.error());return false;}
    return true;
}
}
bool AdoptNativeRoom(std::unique_ptr<Room>& room,unsigned device){
    Environment environment;auto& owner=Runtime();
    if(!room || device>=2 || owner.room || NativeRoomActive() || LegacyNativeSessionActive() || RoomBootstrapActive() || !Frozen(*room))return false;
    if(!owner.initialization.Arm(room->identity()))return false;
    owner.room=std::move(room);owner.device=device;owner.thread.store(0,std::memory_order_release);owner.loaded_waiting.store(false,std::memory_order_release);
    owner.profile_claimed=owner.continue_claimed=owner.world_reported=false;owner.loaded_world.reset();owner.readiness=nullptr;owner.loading_poll.reset();
    owner.prelaunch_polls.store(0,std::memory_order_release);owner.failure_trace_dumped.store(false,std::memory_order_release);
    owner.Publish(NativeSessionState::WaitingForWorld);retained_room_active.store(true,std::memory_order_release);em2::CancelQueuedGameLua();return true;
}
bool NativeRoomActive()noexcept{return retained_room_active.load(std::memory_order_acquire);}
NativeSessionStatus NativeRoomStatus(){return Runtime().Status();}
NativeSessionTiming NativeRoomTiming(){
    if(!NativeRoomActive())return {};
    auto& owner=Runtime();return {true,owner.Status().state==NativeSessionState::Failed || owner.loaded_waiting.load(std::memory_order_acquire) || RoomBootstrapHalted()};
}
bool BindNativeRoomThread(){auto& owner=Runtime();return owner.room && ClaimThread(owner);}
bool StartNativeRoomPrelaunchPolling(){
    auto& owner=Runtime();if(!owner.room || owner.thread.load(std::memory_order_acquire) || owner.prelaunch.joinable() || owner.Status().state!=NativeSessionState::WaitingForWorld)return false;
    try{owner.prelaunch=std::jthread([&owner](std::stop_token stop){
        while(!stop.stop_requested()){
            // Until bound, this worker is the sole transport owner. It neither
            // inspects guest memory nor asserts a world/native producer ready.
            if(!PollOwned(owner,Clock::now()))return;
            owner.prelaunch_polls.fetch_add(1,std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });}catch(const std::exception& error){owner.Fail(error.what());return false;}return true;
}
bool PollNativeRoomLoading(){
    Environment environment;auto& owner=Runtime();if(!owner.room || !Bound(owner))return false;
    const auto now=Clock::now();if(owner.loading_poll && now>=*owner.loading_poll && now-*owner.loading_poll<std::chrono::milliseconds(50))return true;
    owner.loading_poll=now;return PollOwned(owner,now);
}
bool UpdateNativeRoomSession(uint8_t* base,bool advance){
    Environment environment;auto& owner=Runtime();if(!owner.room)return false;
    if(!ClaimThread(owner))return true;if(!PollOwned(owner,Clock::now()))return true;
    if(owner.gate.active()){
        owner.loaded_waiting.store(false,std::memory_order_release);
        if(advance && base && !owner.gate.completion_pending())owner.gate.SetJoining(!em2::NativeCoopReady(base));
        owner.Publish(owner.room->state()==RoomState::Running?NativeSessionState::Attached:NativeSessionState::Joining,{},InputWait::Peer);return true;
    }
    if(!advance || !base)return true;
    if(!owner.world_reported){
        if(!base || !owner.Status().continue_requested || (!em2::NativeOnlineJoinAllowed(base) && !em2::NativeCoopReady(base)))return true;
        const auto token=LoadedWorldToken(*owner.room);if(!token)return true;
        if(!owner.room->BootstrapWorldReady(*token)){owner.Fail("Could not agree on the loaded native Room world.");return true;}
        owner.world_reported=true;owner.loaded_world=token;
    }
    if(owner.room->state()==RoomState::Loading){owner.loaded_waiting.store(true,std::memory_order_release);owner.Publish(NativeSessionState::WaitingForWorld,{},InputWait::Peer);return true;}
    owner.loaded_waiting.store(false,std::memory_order_release);
    if(owner.room->state()!=RoomState::Bootstrapping){owner.Fail("Native Room activated before its raw startup gate was attached.");return true;}
    const auto current=LoadedWorldToken(*owner.room);
    if(!current || current!=owner.loaded_world || (!em2::NativeOnlineJoinAllowed(base) && !em2::NativeCoopReady(base))){
        owner.Fail("The loaded native world changed before Room bootstrap admission.");return true;
    }
    // This boundary belongs after the preceding native post-update. Do not
    // release/recreate synchronized controls or RNG on ordinary frame polls.
    em2::SetOnlineJoinControls(false);em2::ResetNativeSynchronizedJoinTiming();
    if(!owner.gate.Attach(*owner.room)){owner.Fail("Could not attach the agreed native Room bootstrap timeline.");return true;}
    if(!owner.gate.SetReadinessProbe(owner.readiness)){owner.Fail("Could not install the native Room readiness adapter.");return true;}
    owner.gate.SetJoining(!base || !em2::NativeCoopReady(base));
    owner.Publish(NativeSessionState::Joining,{},InputWait::Peer);return true;
}
void ReleaseNativeRoomSession(){
    Environment environment;auto& owner=Runtime();if(!owner.room && !NativeRoomActive())return;
    owner.StopPrelaunch();const bool gate_owned=owner.gate.active();owner.gate.Detach();if(!gate_owned)em2::EndNativeOnlineRandom();em2::SetOnlineJoinControls(false);
    if(owner.room)owner.room->Disconnect();owner.room.reset();owner.initialization.Release();
    owner.thread.store(0,std::memory_order_release);owner.loaded_waiting.store(false,std::memory_order_release);owner.loading_poll.reset();
    owner.profile_claimed=owner.continue_claimed=owner.world_reported=false;owner.loaded_world.reset();owner.readiness=nullptr;owner.Publish(NativeSessionState::None);retained_room_active.store(false,std::memory_order_release);
}
bool RoomPrepareWorldRandom(std::string_view destination){
    Environment environment;auto& owner=Runtime();if(!NativeRoomActive())return false;
    const auto seed=owner.initialization.World(destination);if(!seed)return false;em2::BeginNativeOnlineWorldRandom(seed->value);
    REXLOG_INFO("Native Room world initialization: sequence {}, destination '{}', thread {}, seed {:08X}",seed->sequence,destination,GetCurrentThreadId(),seed->value);return true;
}
bool RoomPreparePlayerRandom(uint32_t slot,uint32_t mode){
    Environment environment;auto& owner=Runtime();if(!NativeRoomActive())return false;
    const auto seed=owner.initialization.Player(slot,mode,owner.gate.active());if(!seed)return false;em2::BeginNativeOnlineWorldRandom(seed->value);
    REXLOG_INFO("Native Room player initialization: slot {}, mode {}, sequence {}, thread {}, seed {:08X}",slot,mode,seed->sequence,GetCurrentThreadId(),seed->value);return true;
}
bool RoomClaimProfile(unsigned& device){
    auto& owner=Runtime();if(!NativeRoomActive() || !Bound(owner))return false;
    std::lock_guard lock(owner.status_mutex);if(owner.status.state!=NativeSessionState::WaitingForWorld || owner.profile_claimed)return false;
    owner.profile_claimed=true;device=owner.device;return true;
}
void RoomFinishProfile(bool accepted){
    auto& owner=Runtime();if(!NativeRoomActive() || !Bound(owner))return;
    std::lock_guard lock(owner.status_mutex);if(owner.status.state==NativeSessionState::WaitingForWorld){if(accepted)owner.status.profile_requested=true;else owner.profile_claimed=false;}
}
bool RoomClaimContinue(){
    auto& owner=Runtime();if(!NativeRoomActive() || !Bound(owner))return false;
    std::lock_guard lock(owner.status_mutex);if(owner.status.state!=NativeSessionState::WaitingForWorld || owner.continue_claimed)return false;
    owner.continue_claimed=true;return true;
}
void RoomFinishContinue(bool accepted){
    auto& owner=Runtime();if(!NativeRoomActive() || !Bound(owner))return;bool park=false;
    {std::lock_guard lock(owner.status_mutex);if(owner.status.state==NativeSessionState::WaitingForWorld){if(accepted){owner.status.continue_requested=true;park=true;}else owner.continue_claimed=false;}}
    if(park)em2::SetOnlineJoinControls(true,false);
}
bool RoomClaimSaveNotice(){
    auto& owner=Runtime();if(!NativeRoomActive() || !Bound(owner) || !REXCVAR_GET(em2_online_auto_continue))return false;
    std::lock_guard lock(owner.status_mutex);if(owner.status.state!=NativeSessionState::WaitingForWorld || !owner.status.profile_requested ||
        owner.status.continue_requested || owner.status.save_notice_requested)return false;owner.status.save_notice_requested=true;return true;
}
bool SetNativeRoomReadinessProbe(RoomBootstrapGate::ReadinessProbe probe){
    auto& owner=Runtime();if(!NativeRoomActive() || !Bound(owner) || owner.Status().state==NativeSessionState::Failed)return false;
    if(owner.gate.active() && !owner.gate.SetReadinessProbe(probe))return false;owner.readiness=probe;return true;
}
bool TestNativeRoomSession(){
    bool ok=true;unsigned checks=0;auto check=[&](bool value,const char* why){++checks;if(!value){REXLOG_ERROR("Native Room owner: {}",why);ok=false;}return value;};
    if(!check(!NativeRoomActive() && !LegacyNativeSessionActive() && !RoomBootstrapActive(),"fixture starts without another runtime owner"))return false;
    struct Cleanup {~Cleanup(){ReleaseNativeRoomSession();}} cleanup;
    Identity id;id.build[0]=1;id.content[0]=2;id.checkpoint[0]=3;id.session[0]=4;id.input_delay=0;
    id.startup_policy=StartupPolicy::NativeMenu;id.initialization_policy=InitializationPolicy::SharedNativeV1;
    std::unique_ptr<Room> host;std::vector<std::unique_ptr<Room>> clients;
    auto pump=[&](auto done){const auto until=Clock::now()+std::chrono::seconds(4);do{
        if(host)host->Poll();for(auto& client:clients)client->Poll();if(done())return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }while(Clock::now()<until);return false;};
    auto frozen=[&](InitializationPolicy policy,bool raw){
        clients.clear();host=std::make_unique<Room>();auto identity=id;identity.initialization_policy=policy;
        if(!host->Host(identity,"127.0.0.1",0,3,"Host"))return false;
        for(unsigned i=0;i<2;++i){clients.push_back(std::make_unique<Room>());if(!clients.back()->Join(identity,"127.0.0.1",host->port(),"Guest "+std::to_string(i)))return false;}
        if(!pump([&]{return host->lobby().players.size()==3 && clients[0]->lobby().players.size()==3 && clients[1]->lobby().players.size()==3;}))return false;
        if(!host->SetReady(true) || !clients[0]->SetReady(true) || !clients[1]->SetReady(true))return false;
        if(!pump([&]{return !clients[0]->profile_pending() && !clients[1]->profile_pending() && std::all_of(host->lobby().players.begin(),host->lobby().players.end(),[](const auto& p){return p.ready;});}))return false;
        const bool started=raw?host->StartNativeBootstrap():host->Start();if(!started)return false;
        return pump([&]{return host->state()==RoomState::Loading && clients[0]->state()==RoomState::Loading && clients[1]->state()==RoomState::Loading;});
    };
    auto empty=std::make_unique<Room>();
    check(!AdoptNativeRoom(empty) && bool(empty) && !RoomPrepareWorldRandom("offline") && !RoomPreparePlayerRandom(0,0),"unfrozen and offline initialization cannot adopt or seed");
    if(!check(frozen(InitializationPolicy::Original,true),"original-policy rejection fixture freezes real TCP room"))return false;
    check(!AdoptNativeRoom(host) && bool(host) && !NativeRoomActive(),"unshared native initialization policy leaves ownership with caller");
    if(!check(frozen(InitializationPolicy::SharedNativeV1,false),"direct-start rejection fixture freezes real TCP room"))return false;
    check(!AdoptNativeRoom(host) && bool(host),"interpreted direct-start Loading cannot masquerade as raw native startup");
    if(!check(frozen(InitializationPolicy::SharedNativeV1,true),"three-peer native-start room freezes installed lobby"))return false;
    check(!AdoptNativeRoom(host,2) && bool(host),"invalid local device rejected without stealing transport");
    if(!check(AdoptNativeRoom(host,1) && !host && NativeRoomActive() && !LegacyNativeSessionActive() && !RoomBootstrapActive() &&
        NativeRoomStatus().state==NativeSessionState::WaitingForWorld && NativeRoomTiming().fixed_step && !NativeRoomTiming().halted,"frozen room transfers exactly once; timing fixed before native world readiness"))return false;
    check(!AdoptNativeRoom(empty) && bool(empty) && !RoomPrepareWorldRandom("not-bound") && !RoomPreparePlayerRandom(0,0) &&
        !PollNativeRoomLoading(),"duplicate adoption and unbound guest/loading callbacks cannot inspect or seed retained transport");
    if(!check(StartNativeRoomPrelaunchPolling() && !StartNativeRoomPrelaunchPolling(),"one temporary prelaunch poller owns the retained socket"))return false;
    const auto wait=Clock::now()+std::chrono::milliseconds(2150);
    do{for(auto& client:clients)client->Poll();std::this_thread::sleep_for(std::chrono::milliseconds(5));}while(Clock::now()<wait);
    check(Runtime().prelaunch_polls.load(std::memory_order_acquire)>10 && std::all_of(clients.begin(),clients.end(),[](const auto& client){return client->state()==RoomState::Loading;}) &&
        NativeRoomStatus().state==NativeSessionState::WaitingForWorld && !RoomBootstrapActive(),"real TCP keepalive is serviced solely by worker during initialization, with no world assertion or gate");
    if(!check(BindNativeRoomThread() && !StartNativeRoomPrelaunchPolling(),"main owner joins prelaunch worker before binding immutable initialization identity"))return false;
    const auto stopped=Runtime().prelaunch_polls.load(std::memory_order_acquire);std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(Runtime().prelaunch_polls.load(std::memory_order_acquire)==stopped,"joined prelaunch worker cannot continue touching Room");
    const auto seed=Runtime().initialization.World("room-owner-test.section");
    check(seed && seed->sequence==1 && seed->value==NativeWorldInitializationSeed(id,"room-owner-test.section",1) &&
        RoomPrepareWorldRandom("room-owner-test.section") && RoomPreparePlayerRandom(0,0) && !RoomPreparePlayerRandom(2,0) && !RoomPreparePlayerRandom(0,3),"world/player seeds use retained identity and bounded native initializer inputs");
    bool wrong_thread=false;std::thread foreign([&]{unsigned device=99;
        wrong_thread=!BindNativeRoomThread() && !PollNativeRoomLoading() && !RoomPrepareWorldRandom("foreign") && !RoomPreparePlayerRandom(0,0) &&
            !RoomClaimProfile(device) && !RoomClaimContinue() && !SetNativeRoomReadinessProbe(nullptr);
    });foreign.join();check(wrong_thread,"worker thread cannot steal simulation binding, loader seeds or native startup authorization");
    unsigned device=99;
    check(RoomClaimProfile(device) && device==1 && !RoomClaimProfile(device),"profile callback has one bounded local device claim");RoomFinishProfile(false);
    check(RoomClaimProfile(device),"rejected native profile callback may retry");RoomFinishProfile(true);
    check(NativeRoomStatus().profile_requested && !RoomClaimProfile(device),"accepted profile callback is not duplicated");
    {const auto saved=REXCVAR_GET(em2_online_auto_continue);REXCVAR_SET(em2_online_auto_continue,true);
        check(RoomClaimSaveNotice() && !RoomClaimSaveNotice(),"authored existing-save notice gets one explicit authorization");REXCVAR_SET(em2_online_auto_continue,saved);}
    check(RoomClaimContinue() && !RoomClaimContinue(),"native Continue transition is single-flight");RoomFinishContinue(false);
    check(RoomClaimContinue(),"rejected native Continue may retry");RoomFinishContinue(true);
    check(NativeRoomStatus().continue_requested && !RoomClaimContinue(),"accepted native Continue parks controls before loaded-world polling");
    check(SetNativeRoomReadinessProbe(nullptr) && Runtime().readiness==nullptr && UpdateNativeRoomSession(nullptr,true) && PollNativeRoomLoading() &&
        !Runtime().world_reported && !Runtime().gate.active() && !RoomBootstrapActive() && Runtime().room->state()==RoomState::Loading &&
        !NativeRoomTiming().halted,"null guest world cannot fabricate loaded-world readiness or gameplay admission; no default readiness probe");
    for(auto& client:clients)client->Disconnect();
    const auto lost=Clock::now()+std::chrono::seconds(3);while(NativeRoomStatus().state!=NativeSessionState::Failed && Clock::now()<lost){UpdateNativeRoomSession(nullptr);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    check(NativeRoomActive() && NativeRoomStatus().state==NativeSessionState::Failed && NativeRoomTiming().fixed_step && NativeRoomTiming().halted &&
        !RoomPrepareWorldRandom("failed") && !RoomPreparePlayerRandom(0,0),"lost retained socket holds failed fixed startup owner and rejects later seeding until shutdown");
    ReleaseNativeRoomSession();
    check(!NativeRoomActive() && !RoomBootstrapActive() && !NativeRoomTiming().fixed_step && NativeRoomStatus().state==NativeSessionState::None &&
        !RoomClaimContinue() && !RoomPrepareWorldRandom("released"),"release clears only Room owner, startup claims, initialization and its parked controls");
    // A no-op Room release must never tear down the independent legacy owner.
    auto legacy=std::make_unique<Session>();Session peer;auto legacy_id=id;legacy_id.startup_policy=StartupPolicy::Prelaunch;
    if(!check(legacy->Host(legacy_id,"127.0.0.1",0) && peer.Join(legacy_id,"127.0.0.1",legacy->port()),"legacy coexistence rejection fixture connects"))return false;
    const auto until=Clock::now()+std::chrono::seconds(3);while((legacy->state()!=State::Ready || peer.state()!=State::Ready) && Clock::now()<until){legacy->Poll();peer.Poll();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    if(!check(AdoptNativeOnlineSession(legacy) && !legacy,"independent legacy owner adopts its ready transport"))return false;
    ReleaseNativeRoomSession();
    check(LegacyNativeSessionActive() && NativeOnlineStatus().state==NativeSessionState::WaitingForWorld && !NativeRoomActive(),"inactive Room release preserves live legacy transport/status");
    if(!check(frozen(InitializationPolicy::SharedNativeV1,true),"legacy exclusion fixture freezes separate native Room")){ReleaseNativeOnlineSession();return false;}
    check(!AdoptNativeRoom(host) && bool(host),"retained legacy owner excludes Room adoption before any pointer transfer");ReleaseNativeOnlineSession();
    if(ok)REXLOG_INFO("Native Room owner passed {} checks: real3-peer Loading adoption, policy/device/exclusive ownership, sole prelaunch keepalive, thread/identity seeds, native callback claims, null-world/default-readiness rejection, held failure and legacy-safe shutdown",checks);
    return ok;
}
}

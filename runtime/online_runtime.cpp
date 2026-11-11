#include "online_runtime.h"
#include "online_continue.h"
#include "online_uuid.h"
#include "online_world_random.h"
#include "online_native_menu.h"
#include "online_native_handoff.h"
#include "online_lobby.h"
#include "native_random.h"
#include "controls.h"
#include "players.h"
#include "simulation.h"
#include "gameplay_mods.h"
#include "native_engine.h"
#include "game_console.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/kernel/kernel_state.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <immintrin.h>
#include <mutex>
#include <thread>
#include <cstring>
#include <windows.h>

REXCVAR_DECLARE(bool,em2_experimental_character_swap);
REXCVAR_DECLARE(bool,em2_online_auto_continue);
REXCVAR_DEFINE_BOOL(em2_online_world_random,false,"Online","Experimental deterministic native world and persistent-player initialization");

namespace em2::net {
namespace {
std::atomic<bool> retained_session_active{false};
struct Owner {
    std::unique_ptr<Session> session;
    std::jthread prelaunch;
    std::atomic<DWORD> thread{0};
    std::optional<std::chrono::steady_clock::time_point> loading_poll;
    unsigned device=0;bool preparing=false,attached=false,continue_claimed=false,profile_claimed=false,menu_preparation=false;
    NativeInitializationContext initialization;
    std::mutex status_mutex;NativeSessionStatus status;
    void StopPrelaunch(){if(prelaunch.joinable()){prelaunch.request_stop();prelaunch.join();}}
    ~Owner(){StopPrelaunch();}
    void Publish(NativeSessionState state,std::string error={},InputWait wait=InputWait::None) {
        std::lock_guard lock(status_mutex);status.state=state;status.error=std::move(error);status.wait=wait;
        if(state==NativeSessionState::None)status.continue_requested=status.profile_requested=status.save_notice_requested=false;
    }
    void Fail(const std::string& error) {
        const auto detail=error;
        initialization.Fail();
        em2::DumpOnlineRandomTraceHistory();
        SetOnlineJoinControls(false);
        if(session)session->Disconnect();
        if(NativeOnlineStatus().state!=NativeSessionState::Failed)REXLOG_ERROR("Retained online session stopped: {}",detail);
        Publish(NativeSessionState::Failed,detail);
        // Keep the Session alive: an attached InputGate borrows it and must
        // remain frozen on failure until explicit world resynchronization.
    }
};
Owner& RuntimeOwner(){static Owner owner;return owner;}
bool ClaimOwnerThread(Owner& owner) {
    DWORD empty=0;const auto current=GetCurrentThreadId();
    owner.thread.compare_exchange_strong(empty,current,std::memory_order_acq_rel);
    if(owner.thread.load(std::memory_order_acquire)!=current)return false;
    owner.StopPrelaunch();return owner.initialization.BindThread();
}
bool PollLoadingAt(std::chrono::steady_clock::time_point now) {
    auto& owner=RuntimeOwner();
    // Never let an asset/render worker touch the single-owner transport. The
    // owner is bound at guest entry/outer update, not by this callback.
    if(owner.thread.load(std::memory_order_acquire)!=GetCurrentThreadId())return false;
    if(!owner.session || NativeOnlineStatus().state==NativeSessionState::Failed)return false;
    // The native callback can be invoked between individual asset operations,
    // even when its own loading-screen render is rate limited.
    if(owner.loading_poll && now>=*owner.loading_poll && now-*owner.loading_poll<std::chrono::milliseconds(50))return true;
    owner.loading_poll=now;
    struct Restore {unsigned csr=_mm_getcsr();~Restore(){_mm_setcsr(csr);}} restore;
    if(owner.attached) {
        GameplayInputGate().Poll(now);
        if(!GameplayInputGate().error().empty())owner.Fail(GameplayInputGate().error());
    } else {
        owner.session->Poll(now);
        if(owner.session->state()!=State::Ready)
            owner.Fail(owner.session->error().empty()?"Input channel lost during native loading":owner.session->error());
    }
    return true;
}
}
bool PollNativeOnlineLoading(){return PollLoadingAt(std::chrono::steady_clock::now());}
bool PrepareNativeOnlineWorldRandom(std::string_view destination){
    auto& owner=RuntimeOwner();
    const auto seed=owner.initialization.World(destination);if(!seed)return false;
    em2::BeginNativeOnlineWorldRandom(seed->value);
    struct Restore{unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Restore(){_mm_setcsr(csr);SetLastError(error);}}restore;
    try{REXLOG_INFO("Native online world initialization: sequence {}, destination '{}', thread {}, seed {:08X}",seed->sequence,destination,GetCurrentThreadId(),seed->value);}catch(...){}
    return true;
}
bool PrepareNativeOnlinePlayerRandom(uint32_t slot,uint32_t mode){
    auto& owner=RuntimeOwner();
    // 8276A828 selects the authoritative native prefab from slot/mode; these
    // are identical on both copies and do not mean the locally owned wire lane.
    // Persistent Mickey is created after the frontend has consumed more rand
    // calls, outside LoadLevel itself. Seed its complete native initializer,
    // including all component/physics/script construction, before its first draw.
    const auto seed=owner.initialization.Player(slot,mode,GameplayInputGate().active());if(!seed)return false;
    em2::BeginNativeOnlineWorldRandom(seed->value);
    struct Restore{unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Restore(){_mm_setcsr(csr);SetLastError(error);}}restore;
    try{REXLOG_INFO("Native online player initialization: slot {}, mode {}, sequence {}, thread {}, seed {:08X}",slot,mode,seed->sequence,GetCurrentThreadId(),seed->value);}catch(...){}
    return true;
}
bool BindNativeOnlineThread(){auto& owner=RuntimeOwner();return owner.session && ClaimOwnerThread(owner);}
bool StartNativeOnlinePrelaunchPolling() {
    auto& owner=RuntimeOwner();
    if(!owner.session || owner.thread.load(std::memory_order_acquire) || owner.prelaunch.joinable() ||
        NativeOnlineStatus().state!=NativeSessionState::WaitingForWorld)return false;
    owner.prelaunch=std::jthread([&owner](std::stop_token stop){
        while(!stop.stop_requested()) {
            owner.session->Poll();
            if(owner.session->state()!=State::Ready) {
                owner.Fail(owner.session->error().empty()?"Input channel lost before native startup":owner.session->error());return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    return true;
}
bool AdoptNativeOnlineSession(std::unique_ptr<Session>& ready,unsigned device) {
    auto& owner=RuntimeOwner();
    if(!ready || ready->state()!=State::Ready || device>=2 || owner.session || GameplayInputGate().active())return false;
    if(!owner.initialization.Arm(ready->identity()))return false;
    owner.session=std::move(ready);owner.device=device;owner.preparing=owner.attached=owner.menu_preparation=false;
    owner.thread.store(0,std::memory_order_release);
    owner.loading_poll.reset();
    owner.continue_claimed=owner.profile_claimed=false;
    retained_session_active.store(true,std::memory_order_release);
    CancelQueuedGameLua();
    owner.Publish(NativeSessionState::WaitingForWorld);return true;
}
bool BeginNativeOnlineMenuPreparation(std::unique_ptr<Session>& ready){
    if(!ready || ready->identity().startup_policy!=StartupPolicy::NativeMenu ||
        ready->identity().initialization_policy!=InitializationPolicy::SharedNativeV1)return false;
    if(!AdoptNativeOnlineSession(ready))return false;
    auto& owner=RuntimeOwner();
    if(!ClaimOwnerThread(owner)){owner.Fail("The native menu could not claim online transport ownership");return false;}
    owner.menu_preparation=true;return true;
}
bool FinishNativeOnlineMenuPreparation(){
    auto& owner=RuntimeOwner();
    if(!owner.menu_preparation || owner.attached || owner.thread.load()!=GetCurrentThreadId() ||
        NativeOnlineStatus().state!=NativeSessionState::WaitingForWorld)return false;
    {
        std::lock_guard lock(owner.status_mutex);
        // This is network startup bookkeeping, never a native profile flag.
        // The live title menu already has a selected local profile, and the
        // original authored reload/Resume has actually accepted the host save.
        owner.profile_claimed=owner.continue_claimed=true;
        owner.status.profile_requested=true;
    }
    owner.menu_preparation=false;FinishNativeOnlineContinue(true);return true;
}
HostCampaignRecord RecordNativeOnlineHostCampaign(const std::filesystem::path& normal,const std::filesystem::path& session){
    auto& owner=RuntimeOwner();
    if(!owner.menu_preparation || owner.attached || !owner.session || owner.thread.load()!=GetCurrentThreadId() ||
        NativeOnlineStatus().state!=NativeSessionState::WaitingForWorld)
        return {{},"The native online owner is not ready to register its campaign."};
    if(owner.session->local_player()!=0)return {};
    return RecordValidatedNativeHostCampaign(normal,session,*owner.session);
}
void FailNativeOnlineMenuPreparation(const std::string& error){
    auto& owner=RuntimeOwner();
    if(owner.menu_preparation && owner.thread.load()==GetCurrentThreadId())owner.Fail(error);
}
NativeSessionStatus NativeOnlineStatus() {
    auto& owner=RuntimeOwner();std::lock_guard lock(owner.status_mutex);return owner.status;
}
bool ClaimNativeOnlineProfile(unsigned& device) {
    auto& owner=RuntimeOwner();std::lock_guard lock(owner.status_mutex);
    if(owner.status.state!=NativeSessionState::WaitingForWorld || owner.profile_claimed || owner.menu_preparation)return false;
    owner.profile_claimed=true;device=owner.device;return true;
}
void FinishNativeOnlineProfile(bool accepted) {
    auto& owner=RuntimeOwner();std::lock_guard lock(owner.status_mutex);
    if(owner.status.state==NativeSessionState::WaitingForWorld){
        if(accepted)owner.status.profile_requested=true;
        else owner.profile_claimed=false;
    }
}
bool ClaimNativeOnlineSaveNotice(){
    if(!REXCVAR_GET(em2_online_auto_continue))return false;
    auto& owner=RuntimeOwner();std::lock_guard lock(owner.status_mutex);
    if(owner.status.state!=NativeSessionState::WaitingForWorld || !owner.status.profile_requested || owner.status.continue_requested || owner.status.save_notice_requested)return false;
    owner.status.save_notice_requested=true;
    REXLOG_INFO("Online startup authorized native existing-save autosave notice confirmation");return true;
}
bool ClaimNativeOnlineContinue() {
    auto& owner=RuntimeOwner();std::lock_guard lock(owner.status_mutex);
    if(owner.status.state!=NativeSessionState::WaitingForWorld || owner.continue_claimed || owner.menu_preparation)return false;
    owner.continue_claimed=true;return true;
}
void FinishNativeOnlineContinue(bool accepted) {
    auto& owner=RuntimeOwner();std::lock_guard lock(owner.status_mutex);
    if(owner.status.state==NativeSessionState::WaitingForWorld) {
        if(accepted){
            owner.status.continue_requested=true;
            // Resume has been accepted by the native menu. Park both ports
            // before the loaded world can poll physical movement/camera input;
            // the fixed-tick preparation later supplies only P2 Start.
            SetOnlineJoinControls(true,false);
        }
        else owner.continue_claimed=false;
    }
}
NativeSessionTiming NativeOnlineTiming() {
    auto& owner=RuntimeOwner();std::lock_guard lock(owner.status_mutex);
    return {owner.status.state!=NativeSessionState::None,owner.status.state==NativeSessionState::Failed};
}
bool SynchronizedSessionActive() noexcept {
    return retained_session_active.load(std::memory_order_acquire) || GameplayInputGate().active();
}
bool UpdateNativeOnlineSession(uint8_t* base,bool advance_world) {
    auto& owner=RuntimeOwner();if(!owner.session)return false;
    if(!ClaimOwnerThread(owner))return true;
    struct Restore {unsigned csr=_mm_getcsr();~Restore(){_mm_setcsr(csr);}} restore;
    if(NativeOnlineStatus().state==NativeSessionState::Failed)return true;
    owner.session->Poll();
    if(owner.session->state()!=State::Ready) {owner.Fail(owner.session->error().empty()?"Input channel lost during native startup":owner.session->error());return true;}
    if(owner.attached) {
        if(!GameplayInputGate().error().empty()){owner.Fail(GameplayInputGate().error());return true;}
        if(owner.preparing && advance_world && NativeCoopReady(base)){
            owner.preparing=false;GameplayInputGate().SetNativeJoinPending(false);
            REXLOG_INFO("Retained online connection attached to native gameplay at tick {}",owner.session->next_tick());
        } else if(!owner.preparing && advance_world && NativeOnlineJoinAllowed(base)) {
            // A checkpoint/area load may restore the single-player companion.
            // Repeat the agreed native join instead of leaving an AI Oswald
            // alongside the online players. Camera changes alone must not
            // restart joining; require the actual single-player AI record.
            owner.preparing=true;GameplayInputGate().SetNativeJoinPending(true);
            em2::ResetNativeSynchronizedJoinTiming();
            REXLOG_INFO("Online level transition: restoring human companion control at tick {}",owner.session->next_tick());
        }
        owner.Publish(owner.preparing?NativeSessionState::Joining:NativeSessionState::Attached,{},GameplayInputGate().wait_reason());
        return true;
    }
    if(!advance_world || owner.menu_preparation)return true;
    const bool ready=NativeCoopReady(base);
    if(!ready && !NativeOnlineJoinAllowed(base)){owner.Publish(NativeSessionState::WaitingForWorld);return true;}
    // Start the shared timeline while both native characters still exist in
    // the loaded single-player world. Waiting for asynchronous P2 activation
    // here lets AI/RNG advance independently before network tick zero.
    SetOnlineJoinControls(false);
    auto* kernel=rex::kernel::kernel_state();auto* input=kernel?kernel->input_system():nullptr;
    if(!input || !GameplayInputGate().Attach(*owner.session,*input,owner.device)) {
        owner.Fail("Could not attach retained connection to the native simulation");return true;
    }
    owner.attached=true;owner.preparing=!ready;GameplayInputGate().SetNativeJoinPending(!ready);
    owner.Publish(ready?NativeSessionState::Attached:NativeSessionState::Joining);
    if(ready)REXLOG_INFO("Retained online connection attached to native gameplay at tick {}",owner.session->identity().first_tick);
    else REXLOG_INFO("Retained input timeline attached before native co-op activation at tick {}",owner.session->identity().first_tick);
    return true;
}
void ReleaseNativeOnlineSession() {
    auto& owner=RuntimeOwner();if(!owner.session)return;
    owner.StopPrelaunch();
    if(owner.attached)GameplayInputGate().Detach();
    SetOnlineJoinControls(false);owner.session->Disconnect();owner.session.reset();
    owner.preparing=owner.attached=owner.continue_claimed=owner.profile_claimed=owner.menu_preparation=false;owner.Publish(NativeSessionState::None);
    retained_session_active.store(false,std::memory_order_release);
    owner.thread.store(0,std::memory_order_release);
    owner.loading_poll.reset();
    owner.initialization.Release();
}
bool TestNativeOnlineSession() {
    if(!TestNativeOnlineHandoff())return false;
    if(!TestNativeOnlineMenu())return false;
    if(!TestOnlineLobby())return false;
    if(!TestNativeWorldInitializationSeed())return false;
    if(!TestOnlineUuidProvenance())return false;
    {
        const bool enabled=REXCVAR_GET(em2_online_world_random);
        REXCVAR_SET(em2_online_world_random,true);
        const bool offline_untouched=!PrepareNativeOnlineWorldRandom("offline-test") && !PrepareNativeOnlinePlayerRandom(0,0);
        REXCVAR_SET(em2_online_world_random,enabled);
        if(!offline_untouched){REXLOG_ERROR("Offline native world loader queued an online RNG seed");return false;}
    }
    if(!em2::TestOfflineStartup())return false;
    auto check=[](bool ok,const char* text){if(!ok)REXLOG_ERROR("Native online owner test: {}",text);return ok;};
    struct Cleanup {
        int speed=em2::SimulationSpeedPercent();float player_speed=em2::PlayerSpeedOverride();bool swap=REXCVAR_GET(em2_experimental_character_swap);
        ~Cleanup(){ReleaseNativeOnlineSession();em2::SetSimulationSpeedPercent(speed);em2::SetPlayerSpeedOverride(player_speed);REXCVAR_SET(em2_experimental_character_swap,swap);}
    } cleanup;
    if(!check(!SynchronizedSessionActive() && em2::SetSimulationSpeedPercent(50) && em2::SetPlayerSpeedOverride(7),"offline preferences can be set before connection adoption"))return false;
    if(!check(!ClaimNativeOnlineContinue() && !RecordNativeOnlineHostCampaign({},{}).error.empty(),"offline startup cannot claim Continue or publish host campaign authority"))return false;
    if(!TestNativeOnlineContinue())return false;
    auto host=std::make_unique<Session>(),client=std::make_unique<Session>();
    Identity id;id.build[0]=1;id.content[0]=2;id.checkpoint[0]=3;id.session[0]=4;id.initialization_policy=InitializationPolicy::SharedNativeV1;
    if(!check(host->Host(id,"127.0.0.1") && client->Join(id,"127.0.0.1",host->port()),"owner fixture sockets open"))return false;
    if(!check(!AdoptNativeOnlineSession(host) && host,"non-ready connection stays owned by its caller"))return false;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    do{host->Poll();client->Poll();if(host->state()==State::Ready && client->state()==State::Ready)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(std::chrono::steady_clock::now()<deadline);
    auto* retained=host.get();
    if(!check(em2::QueueGameLua("return 17").starts_with("Queued Lua") && em2::PendingGameLua()==1,
        "offline Lua request queued before ownership transfer"))return false;
    if(!check(!BeginNativeOnlineMenuPreparation(host) && host && !AdoptNativeOnlineSession(host,2) && host && em2::PendingGameLua()==1 && AdoptNativeOnlineSession(host) && !host && em2::PendingGameLua()==0 &&
        NativeOnlineStatus().state==NativeSessionState::WaitingForWorld,"valid ready connection transfers exactly once with a bounded local device"))return false;
    if(!check(StartNativeOnlinePrelaunchPolling() && !StartNativeOnlinePrelaunchPolling(),
        "prelaunch transport gets one temporary owner"))return false;
    {
        // Real elapsed time beyond the Ready peer's five-second timeout, with
        // no polling of the retained Session from the startup/guest thread.
        const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(6250);
        do{client->Poll();if(client->state()!=State::Ready)break;std::this_thread::sleep_for(std::chrono::milliseconds(5));}
        while(std::chrono::steady_clock::now()<end);
        if(!check(client->state()==State::Ready && NativeOnlineStatus().state==NativeSessionState::WaitingForWorld,
            "real TCP prelaunch keepalive survives a six-second runtime initialization stall without a guest world"))return false;
    }
    if(!check(!PollNativeOnlineLoading() && BindNativeOnlineThread(),
        "loading callbacks cannot claim transport ownership before the simulation owner"))return false;
    {
        struct RestorePolicy{bool value=REXCVAR_GET(em2_online_world_random);~RestorePolicy(){REXCVAR_SET(em2_online_world_random,value);em2::EndNativeOnlineRandom();}}restore_policy;
        REXCVAR_SET(em2_online_world_random,false);
        if(!check(PrepareNativeOnlineWorldRandom("policy-latch-probe") && PrepareNativeOnlinePlayerRandom(0,0),
            "negotiated shared initialization remains active when the console preference is changed off"))return false;
        REXCVAR_SET(em2_online_world_random,true);
        if(!check(PrepareNativeOnlineWorldRandom("policy-latch-probe") && PrepareNativeOnlinePlayerRandom(1,1),
            "console changes cannot replace the active session's initialization policy"))return false;
    }
    if(!check(retained->state()==State::Ready && retained->next_tick()==id.first_tick && !GameplayInputGate().active() &&
        !StartNativeOnlinePrelaunchPolling(),"main binding joins prelaunch owner before session access and cannot restart it"))return false;
    if(!check(UpdateNativeOnlineSession(nullptr) && NativeOnlineStatus().state==NativeSessionState::WaitingForWorld &&
        !GameplayInputGate().active(),"presentation polling does not inspect an absent native world or advance join preparation"))return false;
    {
        const auto start=std::chrono::steady_clock::now();
        const auto tick=retained->next_tick();const auto initial_csr=_mm_getcsr();
        // Twenty seconds on the transport clock, with no outer update, native
        // world access, input submission or simulation-frame consumption.
        for(unsigned step=0;step<=200;++step) {
            const auto now=start+std::chrono::milliseconds(step*100);
            client->Poll(now);PollLoadingAt(now);client->Poll(now);PollLoadingAt(now);
            if(!check(client->state()==State::Ready && retained->state()==State::Ready,
                "loading-frame service keeps both real TCP peers alive beyond the normal timeout"))return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        bool foreign_polled=true,foreign_bound=true;
        std::thread worker([&]{foreign_bound=BindNativeOnlineThread();foreign_polled=PollLoadingAt(start+std::chrono::seconds(100));});worker.join();
        if(!check(!foreign_bound && !foreign_polled && retained->state()==State::Ready && retained->next_tick()==tick &&
            !retained->verified_tick() && !GameplayInputGate().active() &&
            NativeOnlineStatus().state==NativeSessionState::WaitingForWorld && _mm_getcsr()==initial_csr,
            "loading poll rejects foreign threads, preserves FP state and never attaches or advances gameplay"))return false;
    }
    if(!check(!ClaimNativeOnlineSaveNotice(),"autosave notice cannot be authorized before native profile selection"))return false;
    if(!check(TestNativeOnlineProfile(),"native profile bridge guards and caller preservation"))return false;
    {
        unsigned device=99;
        // The bridge fixture accepted one selection; its claim remains held.
        if(!check(NativeOnlineStatus().profile_requested && !ClaimNativeOnlineProfile(device),"accepted native profile bridge does not repeat selection"))return false;
        // Explicitly release only the fixture's claim to exercise rejection/retry.
        FinishNativeOnlineProfile(false);
        if(!check(ClaimNativeOnlineProfile(device) && device==0 && !ClaimNativeOnlineProfile(device),"native profile selection has one claim and keeps the adopted local controller"))return false;
        FinishNativeOnlineProfile(false);
        if(!check(NativeOnlineStatus().profile_requested && ClaimNativeOnlineProfile(device),"a rejected later profile event may retry without erasing earlier acceptance"))return false;
        FinishNativeOnlineProfile(true);
        if(!check(NativeOnlineStatus().profile_requested && !ClaimNativeOnlineProfile(device) && !NativeOnlineStatus().continue_requested,"accepted profile selection is single-flight and does not pretend Continue was requested"))return false;
    }
    {
        const bool enabled=REXCVAR_GET(em2_online_auto_continue);
        REXCVAR_SET(em2_online_auto_continue,false);const bool rejected=!ClaimNativeOnlineSaveNotice();REXCVAR_SET(em2_online_auto_continue,enabled);
        if(!check(rejected && ClaimNativeOnlineSaveNotice() && !ClaimNativeOnlineSaveNotice() && NativeOnlineStatus().save_notice_requested && !NativeOnlineStatus().continue_requested,"existing-save notice authorization respects opt-out and is independent/single-flight"))return false;
    }
    const auto preparing=NativeOnlineTiming();
    if(!check(SynchronizedSessionActive() && em2::EffectiveSimulationSpeedPercent()==100 && em2::SimulationSpeedPercent()==50 && em2::PlayerSpeedOverride()==7,
        "retained pre-world owner uses normal speed while preserving offline preferences"))return false;
    if(!check(!em2::SetSimulationSpeedPercent(0) && !em2::RequestSimulationSteps(1) && !em2::SetPlayerSpeedOverride(12) &&
        !em2::QueuePlayerImpulse(1,0,0,.5f) && em2::SimulationSpeedPercent()==50 && em2::PlayerSpeedOverride()==7,
        "pre-world retained session rejects offline time, speed, impulse and step mutations"))return false;
    const auto native_command=em2::QueueNativeEngineCommand({"anim.event","EVENT_Start_Running_On_Ground"});
    if(!check(native_command && native_command->find("offline")!=std::string::npos,"native commands cannot queue mutations before online world creation"))return false;
    if(!check(em2::QueueGameLua("return 1").find("offline")!=std::string::npos,"Lua cannot queue mutations before online world creation"))return false;
    REXCVAR_SET(em2_experimental_character_swap,true);
    PPCContext swap_context{};const auto original_swap=swap_context;const auto swap_csr=_mm_getcsr();
    em2::RequestCharacterSwap();em2::UpdateCharacterSwap(swap_context,nullptr);
    if(!check(!std::memcmp(&swap_context,&original_swap,sizeof swap_context) && _mm_getcsr()==swap_csr &&
        std::string(em2::CharacterSwapNotice()).find("online")!=std::string::npos,"pre-world online swap is rejected without guest access or caller changes"))return false;
    if(!check(preparing.fixed_step && !preparing.halted && !GameplayInputGate().active(),"fixed simulation starts before world readiness and input-gate attachment"))return false;
    em2::SimulationScheduler clock;em2::SimulationDebt debt;
    unsigned ticks=0;int64_t previous=0;
    for(unsigned frame=0;frame<=120;++frame) {
        const auto now=int64_t(frame)*1000000000/120;
        ticks+=em2::ScheduleSimulationFrame(clock,debt,now-previous,false,preparing.halted,em2::EffectiveSimulationSpeedPercent());previous=now;
    }
    if(!check(ticks==31,"pre-world retained owner schedules thirty-Hz simulation at 120-Hz presentation"))return false;
    if(!check(!AdoptNativeOnlineSession(client) && client && client->state()==State::Ready,"duplicate adoption preserves the caller's connection"))return false;
    if(!TestNativeOnlineContinue())return false;
    {
        // Exercise the real retained owner against native player records,
        // including an area load after the first successful human attachment.
        auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));
        if(!base)return false;
        rex::input::InputSystem input(nullptr);
        struct Restore {
            uint8_t* base;
            ~Restore(){GameplayInputGate().Detach();auto& owner=RuntimeOwner();owner.attached=owner.preparing=false;
                owner.Publish(NativeSessionState::WaitingForWorld);SetOnlineJoinControls(true,false);VirtualFree(base,0,MEM_RELEASE);}
        } restore{base};
        if(!VirtualAlloc(base,0x10000,MEM_COMMIT,PAGE_READWRITE) ||
           !VirtualAlloc(base+0x833D0000,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
        auto put=[&](uint32_t address,uint32_t value){value=_byteswap_ulong(value);std::memcpy(base+address,&value,4);};
        constexpr uint32_t manager=0x1000,profile=0x2000,camera=0x3000;
        put(0x833DAB60,manager);put(0x833DC87C,profile);
        put(manager+20,0x11);put(manager+28,camera);base[manager+357]=1;
        put(manager+340,0x22);put(manager+348,0x4000);
        put(camera,0x82112778);put(camera+564,0x11);put(camera+392,0x5000);put(camera+24,0x5000);
        put(profile+24,2);put(manager+352,0);
        SetOnlineJoinControls(false); // Same handoff as initial production attachment.
        if(!check(GameplayInputGate().Attach(*retained,input,0),"attach real retained gate for level-transition policy"))return false;
        auto& owner=RuntimeOwner();owner.attached=true;
        if(!check(UpdateNativeOnlineSession(base,true) && NativeOnlineStatus().state==NativeSessionState::Attached,
            "initialized human players stay attached"))return false;
        for(uint32_t mode:{1u,2u}) {
            put(profile+24,1);put(manager+352,mode);
            if(!check(UpdateNativeOnlineSession(base,false) && NativeOnlineStatus().state==NativeSessionState::Attached,
                "presentation polling never changes companion ownership"))return false;
            put(camera+24,0x5004);
            if(!check(UpdateNativeOnlineSession(base,true) && NativeOnlineStatus().state==NativeSessionState::Attached,
                "a special camera does not trigger companion replacement"))return false;
            put(camera+24,0x5000);
            if(!check(UpdateNativeOnlineSession(base,true) && NativeOnlineStatus().state==NativeSessionState::Joining,
                "restored 2D/3D AI companion re-arms synchronized human join after a level load"))return false;
            put(0x833DAB60,0);
            if(!check(UpdateNativeOnlineSession(base,true) && NativeOnlineStatus().state==NativeSessionState::Joining,
                "world teardown cannot complete a pending human join"))return false;
            put(0x833DAB60,manager);put(profile+24,2);put(manager+352,0);
            if(!check(UpdateNativeOnlineSession(base,true) && NativeOnlineStatus().state==NativeSessionState::Attached,
                "only initialized human co-op finishes the repeated join"))return false;
        }
    }
    client->Disconnect();
    const auto closed_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    do{PollNativeOnlineLoading();if(retained->state()!=State::Ready)break;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    while(std::chrono::steady_clock::now()<closed_deadline);
    if(!check(retained->state()!=State::Ready && UpdateNativeOnlineSession(nullptr) &&
        NativeOnlineStatus().state==NativeSessionState::Failed,"loading-frame poll reports real peer disconnect without guest memory access"))return false;
    if(!TestNativeOnlineContinue())return false;
    const auto stopped=NativeOnlineTiming();const auto alpha=clock.Alpha();debt.Add(4);
    if(!check(SynchronizedSessionActive() && em2::EffectiveSimulationSpeedPercent()==100 && !em2::SetPlayerSpeedOverride(12) &&
        !em2::QueuePlayerImpulse(1,0,0,.5f),"failed retained owner remains guarded before gate attachment"))return false;
    unsigned stopped_ticks=0;
    for(unsigned frame=0;frame<120;++frame)stopped_ticks+=em2::ScheduleSimulationFrame(clock,debt,1000000000,true,stopped.halted);
    if(!check(stopped.fixed_step && stopped.halted && !GameplayInputGate().active() && stopped_ticks==0 &&
        clock.Alpha()==alpha && !debt.Consume(1),"failed pre-world session holds clock and simulation without accumulating catch-up debt"))return false;
    ReleaseNativeOnlineSession();
    if(!check(!SynchronizedSessionActive() && em2::EffectiveSimulationSpeedPercent()==50 && em2::SimulationSpeedPercent()==50 && em2::PlayerSpeedOverride()==7,
        "release restores stored offline speed and player override without altering preferences"))return false;
    if(!check(NativeOnlineStatus().state==NativeSessionState::None && !NativeOnlineStatus().continue_requested && !NativeOnlineTiming().fixed_step && !NativeOnlineTiming().halted && client,"shutdown releases only the retained owner and its timing policy"))return false;
    ReleaseNativeOnlineSession();
    {
        // A paused offline preference is the important step-request boundary:
        // a nonzero speed would already reject a step without the online guard.
        if(!check(em2::SetSimulationSpeedPercent(0),"offline pause can be selected before a new session"))return false;
        auto paused_host=std::make_unique<Session>();Session paused_peer;id.session[0]^=0x33;
        if(!check(paused_host->Host(id,"127.0.0.1") && paused_peer.Join(id,"127.0.0.1",paused_host->port()),"paused-policy fixture connects"))return false;
        const auto ready_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        do{paused_host->Poll();paused_peer.Poll();if(paused_host->state()==State::Ready && paused_peer.state()==State::Ready)break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(std::chrono::steady_clock::now()<ready_deadline);
        if(!check(paused_peer.state()==State::Ready && AdoptNativeOnlineSession(paused_host) && SynchronizedSessionActive() && em2::SimulationSpeedPercent()==0 &&
            em2::EffectiveSimulationSpeedPercent()==100 && !em2::RequestSimulationSteps(1) && !em2::SetSimulationSpeedPercent(25),
            "retained session overrides offline pause and rejects stepping even with stored speed zero"))return false;
        if(!TestNativeOnlineContinue(true))return false;
        if(!check(StartNativeOnlinePrelaunchPolling(),"prelaunch disconnect fixture starts temporary owner"))return false;
        paused_peer.Disconnect();
        const auto failed_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(NativeOnlineStatus().state!=NativeSessionState::Failed && std::chrono::steady_clock::now()<failed_deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if(!check(NativeOnlineStatus().state==NativeSessionState::Failed && NativeOnlineTiming().halted,
            "peer loss during prelaunch publishes failure before any native world exists"))return false;
        ReleaseNativeOnlineSession();
        if(!check(!SynchronizedSessionActive() && em2::SimulationSpeedPercent()==0 && em2::EffectiveSimulationSpeedPercent()==0,
            "release restores the paused offline preference"))return false;
    }
    {
        auto original_host=std::make_unique<Session>();Session original_peer;auto original=id;
        original.session[0]^=0x71;original.initialization_policy=InitializationPolicy::Original;
        if(!check(original_host->Host(original,"127.0.0.1") && original_peer.Join(original,"127.0.0.1",original_host->port()),"original-policy fixture opens"))return false;
        const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        do{original_host->Poll();original_peer.Poll();if(original_host->state()==State::Ready && original_peer.state()==State::Ready)break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(std::chrono::steady_clock::now()<end);
        struct RestorePreference{bool value=REXCVAR_GET(em2_online_world_random);~RestorePreference(){REXCVAR_SET(em2_online_world_random,value);ReleaseNativeOnlineSession();}}restore_preference;
        REXCVAR_SET(em2_online_world_random,true);
        if(!check(AdoptNativeOnlineSession(original_host) && BindNativeOnlineThread() &&
            !PrepareNativeOnlineWorldRandom("must-stay-original") && !PrepareNativeOnlinePlayerRandom(0,0),
            "negotiated original diagnostic policy remains unseeded despite the console preference changing on"))return false;
    }
    REXLOG_INFO("Native online owner tests passed: ready-only adoption, negotiated initialization policy latched across console changes, pre-world normal-speed scheduling, offline mutation/swap/command guards, preserved preferences, real pre-world disconnect halt, failed-owner guards, one owner, caller preservation and idempotent shutdown");return true;
}
}

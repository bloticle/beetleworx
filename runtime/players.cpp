#include "online_actor_input.h"
#include "online_local_movement.h"
#include "online_retail_input.h"
#include "online_retail_jump.h"
#include "em1_preview_runtime.h"
#include "em1_level_entry.h"
#include "em1_level_collision.h"
#include "ppc_context.h"
#include "players.h"
#include "controls.h"
#include "allocation_trace.h"
#include "native_engine.h"
#include "mickey_ears.h"
#include "demo_setup.h"
#include "game_console.h"
#include "online_continue.h"
#include "simulation.h"
#include "online_runtime.h"
#include "online_world_random.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <stdexcept>
#include <windows.h>

PPC_EXTERN_FUNC(__imp__sub_8276D6C8);
PPC_EXTERN_FUNC(__imp__sub_8276CCF0);
PPC_EXTERN_FUNC(__imp__sub_8276A828);
PPC_EXTERN_FUNC(__imp__sub_82A105F8);
PPC_EXTERN_FUNC(__imp__sub_8276A288);
PPC_EXTERN_FUNC(__imp__sub_8276A350);
PPC_EXTERN_FUNC(__imp__sub_8276BBD0);
PPC_EXTERN_FUNC(__imp__sub_82850FF0);
PPC_EXTERN_FUNC(__imp__sub_8276C0C0);
PPC_EXTERN_FUNC(__imp__sub_827DDD88);
PPC_EXTERN_FUNC(__imp__sub_827B58D0);
PPC_EXTERN_FUNC(__imp__sub_82A69948);
PPC_EXTERN_FUNC(__imp__sub_8276E4F8);
PPC_EXTERN_FUNC(__imp__sub_827692E0);
PPC_EXTERN_FUNC(__imp__sub_82769268);
PPC_EXTERN_FUNC(sub_8276D6C8);
PPC_EXTERN_FUNC(sub_8276B878);
PPC_EXTERN_FUNC(sub_82A104F8);
PPC_EXTERN_FUNC(sub_82A166D8);
PPC_EXTERN_FUNC(sub_82A28250);
PPC_EXTERN_FUNC(sub_82A26348);
PPC_EXTERN_FUNC(__imp__sub_82A2DC20);
PPC_EXTERN_FUNC(__imp__sub_8289D178);
PPC_EXTERN_FUNC(__imp__sub_829E3178);
PPC_EXTERN_FUNC(__imp__sub_829E2318);
PPC_EXTERN_FUNC(__imp__sub_829DA2C0);
PPC_EXTERN_FUNC(sub_82827B00);
PPC_EXTERN_FUNC(__imp__sub_82783CE8);
PPC_EXTERN_FUNC(__imp__sub_828364F0);
PPC_EXTERN_FUNC(sub_825430D8);
PPC_EXTERN_FUNC(sub_822F1C10);
PPC_EXTERN_FUNC(sub_82836170);
PPC_EXTERN_FUNC(__imp__sub_82827320);
PPC_EXTERN_FUNC(__imp__sub_82A3C930);
PPC_EXTERN_FUNC(sub_82A27328);
PPC_EXTERN_FUNC(__imp__sub_8276EF50);
PPC_EXTERN_FUNC(sub_8276EF50);
REXCVAR_DECLARE(std::string,em2_user_data_directory);
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(bool,em2_auto_continue);
REXCVAR_DECLARE(std::string,em1_register_level);
REXCVAR_DEFINE_STRING(em2_qa_boot_level,"","Debug","Private test: replace the initial engine scene, bypassing title/Continue; does not initialize campaign player state");
REXCVAR_DEFINE_STRING(em2_qa_start_level,"","Debug","Private automatic-Continue test: enter a retail level through native CleanLoadLevel after the saved world is ready");
REXCVAR_DEFINE_BOOL(em2_trace_players,false,"Debug","Trace native player mode transitions and entity replacement");
REXCVAR_DECLARE(bool,em2_trace_auto_continue);
REXCVAR_DEFINE_BOOL(em2_native_player_actions,true,"CPU","Use native player action-name filtering");
REXCVAR_DEFINE_BOOL(em2_benchmark_player_actions,false,"Debug","Benchmark native action filtering during runtime regression tests");
REXCVAR_DEFINE_BOOL(em2_experimental_character_swap,true,"Gameplay","Allow single-player character switching; Mickey waits while Oswald is controlled");

namespace {
struct Record {
    uint32_t entity=0,controller=0,mode=0;
};
thread_local bool canonical_join_dispatch=false;
struct NativeJoinLease {uint64_t generation=0,tick=0;DWORD thread=0;};
thread_local NativeJoinLease native_join_lease;
bool ExplicitJoinOwnsCurrentTick();
bool SuppressOrdinaryJoin(bool scoped,bool legacy,bool explicit_owner){return !scoped && (legacy || explicit_owner);}
uint32_t direct_boot_controls_pending=0;
std::atomic<bool> swap_requested{false},oswald_selected{false},swap_recovering{false};
std::atomic<const char*> swap_notice{nullptr};
uint32_t selected_character_property=0;
std::atomic<int64_t> swap_notice_until{0};
int64_t NoticeClock() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void ShowSwapNotice(const char* text) {
    swap_notice.store(text);swap_notice_until.store(NoticeClock()+3500);
}
void PublishSwapState(PPCContext& ctx,uint8_t* base,bool selected,bool coop) {
    const auto properties=PPC_LOAD_U32(0x833DCBA4);
    if(!properties) return;
    const auto saved=ctx;
    const auto stack=ctx.r1.u32-256;
    PPC_STORE_U32(stack,ctx.r1.u32);
    auto publish=[&](uint32_t name,bool value) {
        PPC_STORE_U32(stack+96,3);
        PPC_STORE_U32(stack+100,0);
        PPC_STORE_U64(stack+104,value?0x3FF0000000000000ull:0ull);
        ctx.r1.u64=stack;ctx.r3.u64=properties;ctx.r4.u64=name;ctx.r5.u64=stack+96;
        sub_82836170(ctx,base);
    };
    // Match the native join/leave notification: behavior controllers subscribe
    // to changes, so overriding the getter alone never activates their actions.
    publish(properties+36,selected || coop);
    if(selected_character_property) {
        PPC_STORE_U32(stack+128,selected_character_property);
        publish(stack+128,selected);
    }
    ctx=saved;
}
struct SwapNativeCalls {
    PPCFunc* resolve=sub_8276B878;
    PPCFunc* mode=sub_8276D6C8;
    PPCFunc* rebuild=sub_82A26348;
    PPCFunc* attach=sub_82A104F8;
    PPCFunc* camera_target=sub_82A166D8;
    PPCFunc* player_input=sub_82A28250;
    PPCFunc* enable=sub_82A27328;
    void (*publish)(PPCContext&,uint8_t*,bool,bool)=PublishSwapState;
    void (*reset_motion)(PPCContext&,uint8_t*)=em2::ResetPlayerMotionHistory;
};
const SwapNativeCalls native_swap_calls{};
// Only the headless regression suite installs a backend. Production uses the
// actual native factory, camera, controller and property notification calls.
thread_local const SwapNativeCalls* swap_test_calls=nullptr;
struct SwapSession {
    uint32_t manager=0,camera=0,mickey=0,oswald=0,ai_mode=1;
    bool restoring=false;
    bool failure_reported=false;
    std::chrono::steady_clock::time_point retry_at{};
    uint32_t mickey_input=0;
} swap_session;
struct JoinedPlayerControls {
    uint32_t manager=0,entity=0;
} joined_player_controls;
thread_local PPCFunc* viewport_test_activate=nullptr;

bool SinglePlayerViewport(uint32_t manager,uint8_t* base) {
    const auto profile=PPC_LOAD_U32(0x833DC87C);
    return manager && manager==PPC_LOAD_U32(0x833DAB60) && profile && PPC_LOAD_U32(profile+24)==1;
}
template<class Activate>void ActivatePlayerViewport(PPCContext& ctx,uint8_t* base,Activate activate) {
    // A human companion used for possession does not add a signed-in player.
    // Respawn must not turn that companion into a second local viewport.
    if(!SinglePlayerViewport(ctx.r3.u32,base))activate(ctx,base);
}
uint32_t PossessedCamera(uint32_t manager,uint32_t camera,uint8_t* base) {
    if(oswald_selected.load(std::memory_order_relaxed) && SinglePlayerViewport(manager,base) &&
       camera && camera==PPC_LOAD_U32(manager+348)) {
        const auto primary=PPC_LOAD_U32(manager+28);
        if(primary)return primary;
    }
    return camera;
}

struct RestorationResult {
    bool character=false,camera=false,missing=false,unavailable=false;
    bool Complete() const {return character && camera;}
};
template<typename Read,typename Mode,typename Rebuild,typename Attach>
RestorationResult RestoreSwapTransaction(bool coop,uint32_t ai_mode,uint32_t mickey,
                                        Read read,Mode mode,Rebuild rebuild,Attach attach) {
    const auto before=read();
    // The native factory only supports companion modes 0..2. If the new area
    // explicitly has no companion, restore Mickey without spawning an actor
    // that the scene has disabled.
    if(!coop && !before.entity && before.mode>2)
        return {true,mickey && attach()==mickey,true,true};
    const auto recovery_mode=before.mode==1 || before.mode==2 ? before.mode : ai_mode;
    if(!coop) {
        if(!before.entity && before.mode==recovery_mode) rebuild(recovery_mode);
        else if(!before.entity || before.mode==0) mode(recovery_mode);
        // A scene transition may already have installed the appropriate
        // 2D/3D AI. Preserve that native choice instead of forcing the old mode.
    }
    const auto after=read();
    const bool character=after.entity && (coop ? after.mode==0 : after.mode==1 || after.mode==2);
    return {character,mickey && attach()==mickey,!after.entity};
}
bool TestSwapRestoration() {
    Record state{22,0,0};
    unsigned modes=0,rebuilds=0,attachments=0,last_rebuild=0;
    bool factory_ok=false,mode_ok=true;
    uint32_t linked=1;
    auto read=[&]{return state;};
    auto mode=[&](uint32_t value){++modes;state.mode=mode_ok?value:0;state.entity=factory_ok?33:0;};
    auto rebuild=[&](uint32_t value){++rebuilds;last_rebuild=value;state.mode=value;state.entity=factory_ok?44:0;};
    auto attach=[&]{++attachments;return linked;};
    auto run=[&](bool coop=false){return RestoreSwapTransaction(coop,1,1,read,mode,rebuild,attach);};
    auto require=[](bool value,const char* reason){if(!value) REXLOG_ERROR("Swap restoration fault test: {}",reason);return value;};
    auto result=run();
    if(!require(!result.Complete() && !result.character && result.camera && result.missing && modes==1 && !rebuilds,
        "failed AI factory must not be reported restored")) return false;
    factory_ok=true;linked=99;result=run();
    if(!require(!result.Complete() && result.character && !result.camera && !result.missing && modes==1 && rebuilds==1,
        "same-mode missing entity must rebuild, and camera failure must remain pending")) return false;
    linked=1;result=run();
    if(!require(result.Complete() && modes==1 && rebuilds==1 && state.entity==44,
        "camera retry must preserve the recovered AI entity")) return false;
    state={55,0,0};result=run(true);
    if(!require(result.Complete() && state.mode==0 && state.entity==55 && modes==1 && rebuilds==1,
        "native co-op takeover must preserve human P1")) return false;
    state={66,0,2};result=run();
    if(!require(result.Complete() && state.mode==2 && modes==1 && rebuilds==1,
        "native scene-selected 2D AI must be preserved")) return false;
    state={0,0,0};result=run(true);
    if(!require(!result.Complete() && result.missing && modes==1 && rebuilds==1,
        "missing native co-op player must not be recreated as AI")) return false;
    state={77,0,0};mode_ok=false;result=run();
    if(!require(!result.Complete() && !result.character && !result.missing && modes==2,
        "wrong control mode must fail even when entity and camera exist")) return false;
    const auto prior_attachments=attachments;
    result=RestoreSwapTransaction(true,1,0,read,mode,rebuild,attach);
    if(!require(!result.Complete() && !result.camera && attachments==prior_attachments,
        "missing Mickey handle must not cause a camera attach")) return false;
    state={0,0,2};result=run();
    if(!require(result.Complete() && last_rebuild==2 && state.mode==2,
        "missing scene-selected 2D AI must rebuild as 2D, not the old 3D mode"))return false;
    const auto native_calls=modes+rebuilds;
    state={0,0,0xFFFFFFFFu};result=run();
    if(!require(result.Complete() && result.unavailable && result.missing && modes+rebuilds==native_calls,
        "a scene with a disabled companion must restore Mickey without spawning Oswald"))return false;
    REXLOG_INFO("Swap restoration fault tests passed: failed mode/factory, same-mode rebuild, camera retry, native co-op takeover, missing 2D AI and disabled-companion preservation");
    return true;
}

Record ReadRecord(uint32_t manager,uint32_t slot,uint8_t* base) {
    if(!manager || slot>=2) return {};
    const auto record=manager+slot*320;
    return {PPC_LOAD_U32(record+20),PPC_LOAD_U32(record+28),PPC_LOAD_U32(record+32)};
}
bool NormalPlayerCamera(uint32_t camera,const Record& p0,uint8_t* base) {
    return camera && p0.entity && p0.mode==0 &&
        PPC_LOAD_U32(camera)==0x82112778 && PPC_LOAD_U32(camera+564) &&
        PPC_LOAD_U32(camera+392) && PPC_LOAD_U32(camera+24)==PPC_LOAD_U32(camera+392);
}
bool JoinedHumanPlayer(bool joined,uint32_t players,const Record& before,const Record& after) {
    return joined && players>1 && (before.mode==1 || before.mode==2) && after.mode==0 && after.entity;
}
enum class JoinedControlsResult {None,Cancelled,Deferred,Enabled};
template<typename Enable>
JoinedControlsResult UpdateJoinedControls(JoinedPlayerControls& pending,PPCContext& ctx,uint8_t* base,Enable enable) {
    if(!pending.manager) return JoinedControlsResult::None;
    const auto manager=PPC_LOAD_U32(0x833DAB60),profile=PPC_LOAD_U32(0x833DC87C);
    if(manager!=pending.manager || !profile || PPC_LOAD_U32(profile+24)<2) {
        pending={};return JoinedControlsResult::Cancelled;
    }
    const auto p0=ReadRecord(manager,0,base),p1=ReadRecord(manager,1,base);
    if(p1.entity!=pending.entity || p1.mode!=0) {pending={};return JoinedControlsResult::Cancelled;}
    if(!PPC_LOAD_U8(manager+357) || !NormalPlayerCamera(p0.controller,p0,base))
        return JoinedControlsResult::Deferred;
    const auto saved=ctx;const auto csr=_mm_getcsr();
    // The native join factory leaves a new human actor's listeners disabled.
    // Apply its ordinary controller/default policy once, between completed
    // native updates rather than during action/component-list traversal.
    ctx.r3.u64=manager;ctx.r4.u64=1;ctx.r5.u64=1;ctx.r6.u64=1;
    enable(ctx,base);
    ctx=saved;_mm_setcsr(csr);
    pending={};return JoinedControlsResult::Enabled;
}
bool ReadyForSwap(uint32_t camera,const Record& p0,const Record& p1,uint8_t* base) {
    return p1.entity && p1.mode<=2 && NormalPlayerCamera(camera,p0,base);
}
bool ReadyForSwapFrame(bool restoring,bool enabled,uint32_t camera,const Record& p0,const Record& p1,uint8_t* base) {
    // Starting a swap needs both actors. Recovery must also work after the
    // selected actor has disappeared; the native factory runs below this gate.
    return enabled && (restoring?NormalPlayerCamera(camera,p0,base):ReadyForSwap(camera,p0,p1,base));
}
bool NeutralizeMovementIntent(uint32_t component,uint8_t* base) {
    if(!component || PPC_LOAD_U32(component+24)!=0x8211DCE0) return false;
    // Basic movement handler 82783CE8 writes interface+168/+172 where that
    // interface is component+80. Stop the parked character's last command;
    // retain physics velocity, gravity, animation and all neighboring state.
    PPC_STORE_U32(component+248,0);PPC_STORE_U32(component+252,0);
    return true;
}

bool ActionBelongsToSlot(uint32_t action,uint32_t slot,uint8_t* base) {
    if(slot>=2) return false;
    const auto name=PPC_LOAD_U32(action);
    // 82769268 calls NiFixedString::Contains("2"), then negates it for P0.
    // Match its pointer-identity and empty-string shortcuts, and its distinct
    // static-string / interned-string length representations (8250D7E8).
    const uint32_t needle=slot ? 0x82078740u : 0x8207873Cu;
    bool contains=false;
    if(name==needle) contains=true;
    else if(name && PPC_LOAD_U8(name) && name!=PPC_LOAD_U32(0x8203D278)) {
        const auto* bytes=reinterpret_cast<const char*>(PPC_RAW_ADDR(name));
        const size_t length=name>0x80000000u && name<0xA0000000u
            ? std::strlen(bytes) : PPC_LOAD_U16(name-2);
        contains=std::memchr(bytes,'2',length)!=nullptr;
    }
    return slot ? contains : !contains;
}
}

PPC_FUNC_IMPL(sub_8276EF50) {
    if(!canonical_join_dispatch && SuppressOrdinaryJoin(false,
        em2::net::SynchronizedSessionActive() && em2::net::NativeOnlineStatus().state==em2::net::NativeSessionState::Joining,
        ExplicitJoinOwnsCurrentTick())){ctx.r3.u64=0;return;}
    const auto manager=ctx.r3.u32;
    const auto before=ReadRecord(manager,1,base);
    __imp__sub_8276EF50(ctx,base);
    if(!manager) return;
    const auto profile=PPC_LOAD_U32(0x833DC87C);
    const auto after=ReadRecord(manager,1,base);
    if(profile && JoinedHumanPlayer(ctx.r3.u32!=0,PPC_LOAD_U32(profile+24),before,after)) {
        joined_player_controls={manager,after.entity};
        REXLOG_INFO("Native co-op join: queued player-two controller activation for {:08X}",after.entity);
    }
}

PPC_FUNC_IMPL(sub_8289D178) {
    if(REXCVAR_GET(em2_trace_players)) {
        const auto name=ctx.r4.u32?PPC_LOAD_U32(ctx.r4.u32):0;
        REXLOG_INFO("Native profile-gated level request: '{}'",name?reinterpret_cast<const char*>(PPC_RAW_ADDR(name)):"");
    }
    __imp__sub_8289D178(ctx,base);
}

PPC_FUNC_IMPL(sub_829E2318) {
    // 829E2BD8 pumps this loading-frame routine while the normal outer frame
    // is blocked. Keep TCP heartbeats/digests flowing on the same native owner;
    // this does not advance the input timeline or relax world verification.
    em2::net::PollNativeOnlineLoading();
    __imp__sub_829E2318(ctx,base);
    em2::net::PollNativeOnlineLoading();
}

PPC_FUNC_IMPL(sub_829E3178) {
    std::string script_error;
    if(!em2::em1::EndRegisteredLevelScripts(ctx,base,script_error)){
        REXLOG_ERROR("[em1] native level transition retained original script ownership: {}",script_error);ctx.r3.u64=0;return;
    }
    em2::InvalidateMickeyEarInstances();
    direct_boot_controls_pending=0;
    em2::InvalidateNativeConsoleWorld();
    const auto* requested=ctx.r4.u32?reinterpret_cast<const char*>(PPC_RAW_ADDR(ctx.r4.u32)):"";
    const std::string requested_level(requested,strnlen_s(requested,96));
    if(REXCVAR_GET(em2_trace_players))
        REXLOG_INFO("Native level loader '{}' caller {:08X}",requested_level,uint32_t(ctx.lr));
    const auto load=[&] {
        const auto* actual=ctx.r4.u32?reinterpret_cast<const char*>(PPC_RAW_ADDR(ctx.r4.u32)):"";
        const std::string destination(actual,strnlen_s(actual,96));
        em2::RecordNativeLoadLevel(destination);
        em2::em1::ObserveRegisteredLevelEntryRequest(destination);
        std::string collision_error;
        if(!em2::em1::ReleaseRegisteredLevelCollision(ctx,base,collision_error)){
            REXLOG_ERROR("[em1] level transition retained collision ownership: {}",collision_error);ctx.r3.u64=0;return;
        }
        if(!em2::em1::BeforeArchitectureLevelChange(ctx,base,destination)){ctx.r3.u64=0;return;}
        if(em2::em1::TryEnterArchitectureLevel(ctx,base,ctx.r3.u32,destination)){
            if(ctx.r3.u32 && em2::Em1DemoSession())direct_boot_controls_pending=PPC_LOAD_U32(0x833DAB60);
            return;
        }
        auto& gate=em2::net::GameplayInputGate();
        if(!gate.NativeLoadPoint(destination,em2::net::LoadPhase::Begin)){ctx.r3.u64=0;return;}
        em2::net::PrepareNativeOnlineWorldRandom(destination);
        if(!em2::em1::BeginRegisteredLevelScripts(base,destination,collision_error)){
            REXLOG_ERROR("[em1] original script world could not begin: {}",collision_error);
            gate.NativeLoadPoint(destination,em2::net::LoadPhase::End,false);ctx.r3.u64=0;return;
        }
        {em2::em1::RegisteredLevelActorScope source_actors(em2::em1::RegisteredLevelBlueprints(destination));
         __imp__sub_829E3178(ctx,base);}
        if(ctx.r3.u32 && !em2::em1::AttachRegisteredLevelCollision(ctx,base,destination,collision_error)){
            REXLOG_ERROR("[em1] registered level collision admission failed: {}",collision_error);ctx.r3.u64=0;
        }
        if(ctx.r3.u32 && !em2::em1::CommitRegisteredLevelScripts(base,collision_error)){
            REXLOG_ERROR("[em1] original script world could not commit: {}",collision_error);ctx.r3.u64=0;
        }
        if(!gate.NativeLoadPoint(destination,em2::net::LoadPhase::End,ctx.r3.u32!=0))ctx.r3.u64=0;
        if(!ctx.r3.u32 && !em2::em1::EndRegisteredLevelScripts(ctx,base,collision_error))
            REXLOG_ERROR("[em1] failed native load retained original script ownership: {}",collision_error);
        if(ctx.r3.u32){em2::em1::AfterArchitectureLevelChange(ctx,base,destination);em2::em1::NotifyArchitecturePreviewLevelLoaded(destination);}
        if(REXCVAR_GET(em2_trace_auto_continue)){
            const auto csr=_mm_getcsr();
            REXLOG_INFO("Automatic startup native level '{}' load result {}",destination,ctx.r3.u32!=0);
            _mm_setcsr(csr);
        }
    };
    static bool redirected=false;
    const auto boot=REXCVAR_GET(em2_qa_boot_level);
    if(!redirected && uint32_t(ctx.lr)==0x829EF960 && !boot.empty() && boot.size()<=47 &&
       boot.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_.")==std::string::npos &&
       REXCVAR_GET(em2_private_profile) && !REXCVAR_GET(em2_user_data_directory).empty()){
        redirected=true;
        if(boot.starts_with("em1_") && em2::em1::RegisteredLevelState(REXCVAR_GET(em1_register_level))!=boot){
            REXLOG_ERROR("Private EM1 startup stopped: requested state '{}' was not successfully registered; see the preceding asset import error",boot);
            spdlog::shutdown();ExitProcess(1);
        }
        const auto startup_level=ctx.r3.u32+0x1067C;
        std::memcpy(PPC_RAW_ADDR(startup_level),boot.c_str(),boot.size()+1);
        ctx.r4.u64=startup_level;
        REXLOG_INFO("Private direct engine boot: initial scene '{}' -> '{}'; title/Continue bypassed, campaign player state unproven",requested_level,boot);
        load();
        if(!ctx.r3.u32){
            REXLOG_ERROR("Private direct engine boot failed: '{}'; see preceding native load/admission error; no title-screen fallback",boot);
            spdlog::shutdown();ExitProcess(1);
        }
        direct_boot_controls_pending=PPC_LOAD_U32(0x833DAB60);
        return;
    }
    load();
}

PPC_FUNC_IMPL(sub_829DA2C0) {
    if(REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_trace_players)){
        const auto error=GetLastError();const auto csr=_mm_getcsr();
        static unsigned requests=0;
        if(requests++<24){
            REXLOG_INFO("Private native level request: caller {:08X} owner {:08X} name pointer {:08X}",uint32_t(ctx.lr),ctx.r3.u32,ctx.r4.u32);
            auto stack=ctx.r1.u32;
            for(unsigned i=0;i<12 && stack>=4096;++i){
                MEMORY_BASIC_INFORMATION info{};
                if(!VirtualQuery(base+stack,&info,sizeof(info)) || info.State!=MEM_COMMIT || (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))break;
                const auto next=PPC_LOAD_U32(stack);
                if(next<=stack || next-stack>65536)break;
                if(!VirtualQuery(base+next-8,&info,sizeof(info)) || info.State!=MEM_COMMIT || (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))break;
                REXLOG_INFO("Private native level request frame {}: stack {:08X} saved return {:08X}",i,stack,PPC_LOAD_U32(next-8));stack=next;
            }
        }
        _mm_setcsr(csr);SetLastError(error);
    }
    __imp__sub_829DA2C0(ctx,base);
}

void em2::MaybeQueueNativeStartLevel() {
    if(!REXCVAR_GET(em2_private_profile) || !REXCVAR_GET(em2_auto_continue))return;
    const auto level=REXCVAR_GET(em2_qa_start_level);
    if(level.empty() || REXCVAR_GET(em2_user_data_directory).empty() || net::SynchronizedSessionActive())return;
    struct Restore {DWORD error=GetLastError();unsigned csr=_mm_getcsr();~Restore(){SetLastError(error);_mm_setcsr(csr);}} restore;
    static bool attempted=false;static std::optional<uint64_t> ready_step;
    if(attempted || !OfflineStartupContinueAccepted())return;
    if(!NativeConsoleWorldReady()){ready_step.reset();return;}
    const auto step=simulation_steps.load(std::memory_order_relaxed);
    if(!ready_step){ready_step=step;return;}
    if(step<*ready_step || step-*ready_step<30)return;
    // Use the same validated native command as the player's level menu.
    // Replacing only the low-level destination during checkpoint restoration
    // carries the old level's spawn/preservation state into the new world.
    attempted=true;
    const auto result=ExecuteGameplayConsoleTool({"level.load",level});
    REXLOG_INFO("Private native start-level command: '{}' result='{}'; normal checkpoint restored first, native CleanLoadLevel, no gameplay input",level,result.value_or("Command was not recognized"));
}

PPC_FUNC_IMPL(sub_82783CE8) {
    if(em2::net::SuppressRetailJumpInput(ctx,base,true) || em2::net::SuppressRetailMovement(ctx,base))return;
    if(em2::net::SuppressOwnedActorInput(ctx,base))return;
    if(oswald_selected.load(std::memory_order_relaxed) && swap_session.mickey_input &&
       ctx.r3.u32==swap_session.mickey_input) {ctx.r3.u64=0;return;}
    if(REXCVAR_GET(em2_trace_players) && ctx.r5.u32) {
        const auto name=PPC_LOAD_U32(ctx.r5.u32);
        const auto* text=name?reinterpret_cast<const char*>(PPC_RAW_ADDR(name)):"";
        const std::string action(text,strnlen_s(text,96));
        static thread_local unsigned samples=0;
        if(samples<100 && (action=="Jump" || action=="Jump2" ||
           ((action=="Movement" || action=="Movement2") && (ctx.f1.f64!=0 || ctx.f2.f64!=0)))) {
            ++samples;
            const auto handler=ctx.r3.u32;
            const auto manager=PPC_LOAD_U32(0x833DAD98);
            REXLOG_INFO("Character input '{}': interface {:08X}, owner {:08X}, values {}, {}, flags {:02X}, blocked {}, listeners {}",
                action,handler,PPC_LOAD_U32(handler-16),ctx.f1.f64,ctx.f2.f64,
                PPC_LOAD_U8(handler-12),manager?PPC_LOAD_U8(manager+102):0,
                PPC_LOAD_U32(handler+116));
            if(action=="Jump" || action=="Jump2") {
                const auto listeners=PPC_LOAD_U32(handler+112);
                const auto count=std::min(PPC_LOAD_U32(handler+116),32u);
                for(uint32_t i=0;listeners && i<count;++i) {
                    const auto listener=PPC_LOAD_U32(listeners+4*i);
                    const auto table=listener?PPC_LOAD_U32(listener):0;
                    REXLOG_INFO("Character action listener {}: object {:08X}, flags {:08X}, method {:08X}",
                        i,listener,listener?PPC_LOAD_U32(listener+8):0,table?PPC_LOAD_U32(table+40):0);
                }
            }
        }
    }
    __imp__sub_82783CE8(ctx,base);
}

PPC_FUNC_IMPL(sub_82A3C930) {
    if(em2::net::SuppressRetailJumpInput(ctx,base,false) || em2::net::SuppressRetailMovement(ctx,base))return;
    if(em2::net::SuppressOwnedActorInput(ctx,base))return;
    if(oswald_selected.load(std::memory_order_relaxed) && swap_session.mickey_input &&
       ctx.r3.u32==swap_session.mickey_input) {ctx.r3.u64=0;return;}
    em2::net::ObserveNativeLocalMovementBinding(ctx,base);
    __imp__sub_82A3C930(ctx,base);
}

PPC_FUNC_IMPL(sub_82769268) {
    // Character selection aliases the input device, retaining native P1/P2
    // action names. Abilities such as Oswald's remote require the P2 names.
    const auto action=ctx.r4.u32,slot=ctx.r5.u32;
    if(!REXCVAR_GET(em2_native_player_actions)) {
        ctx.r5.u64=slot;
        __imp__sub_82769268(ctx,base);
        return;
    }
    ctx.r3.u64=ActionBelongsToSlot(action,slot,base);
    if(REXCVAR_GET(em2_trace_players) && slot<2) {
        // Bounded, per-thread first-observation trace; never log every poll.
        static thread_local std::array<uint64_t,64> seen{};
        static thread_local unsigned count=0;
        const auto name=PPC_LOAD_U32(action);
        const uint64_t key=(uint64_t(name)<<2)|slot;
        if(count<seen.size() && std::find(seen.begin(),seen.begin()+count,key)==seen.begin()+count) {
            seen[count++]=key;
            const auto* text=name ? reinterpret_cast<const char*>(PPC_RAW_ADDR(name)) : "";
            REXLOG_INFO("Native player action filter P{}: '{}' accepted {}",slot,std::string(text,strnlen_s(text,96)),ctx.r3.u32);
        }
    }
}

PPC_FUNC_IMPL(sub_82A2DC20) {
    const auto output=ctx.r3.u32;
    __imp__sub_82A2DC20(ctx,base);
    if(output && oswald_selected.load(std::memory_order_relaxed))
        PPC_STORE_U64(output+8,0x3FF0000000000000ull); // Script boolean is a numeric variant.
}

PPC_FUNC_IMPL(sub_82827320) {
    const auto slot=ctx.r4;
    // Oswald's human controller also queries its logical player's device.
    // Borrow P0's device while selected without adding a second signed-in
    // player, changing the profile count or enabling split screen.
    if(oswald_selected.load(std::memory_order_relaxed) && slot.u32==1 &&
       ctx.r3.u32==PPC_LOAD_U32(0x833DC87C)) ctx.r4.u64=0;
    __imp__sub_82827320(ctx,base);
    ctx.r4=slot;
}

PPC_FUNC_IMPL(sub_82A105F8) {
    // Native camera reset is void. Extra/remote actors need not own a local
    // camera: 8276BBD0 legitimately returns null for those actors during their
    // fall/damage recovery. Preserve the rest of that event, without resetting
    // somebody else's camera or invoking a method on a missing receiver.
    if(!ctx.r3.u32)return;
    __imp__sub_82A105F8(ctx,base);
}

PPC_FUNC_IMPL(sub_8276A288) {ActivatePlayerViewport(ctx,base,viewport_test_activate?viewport_test_activate:__imp__sub_8276A288);}
PPC_FUNC_IMPL(sub_8276BBD0) {
    const auto manager=ctx.r3.u32;
    __imp__sub_8276BBD0(ctx,base);
    ctx.r3.u64=PossessedCamera(manager,ctx.r3.u32,base);
}

PPC_FUNC_IMPL(sub_82850FF0) {
    // The native movement-reference installer 8276C0C0 reads the player
    // record's camera directly, then resolves its entity here. It does NOT
    // call 8276BBD0. Consequently Oswald retained the hidden P2 camera's
    // basis even though the displayed P0 camera followed him. Select the
    // displayed camera before the original resolver and native reference
    // setter run, including their normal reference ownership/cache updates.
    if(oswald_selected.load(std::memory_order_relaxed))
        ctx.r3.u64=PossessedCamera(PPC_LOAD_U32(0x833DAB60),ctx.r3.u32,base);
    __imp__sub_82850FF0(ctx,base);
}

bool em2::NativeCoopReady(uint8_t* base) {
    const auto manager=PPC_LOAD_U32(0x833DAB60),profile=PPC_LOAD_U32(0x833DC87C);
    if(!manager || !profile || PPC_LOAD_U32(profile+24)!=2 || joined_player_controls.manager || !PPC_LOAD_U8(manager+357)) return false;
    const auto p0=ReadRecord(manager,0,base),p1=ReadRecord(manager,1,base);
    return p1.entity && p1.controller && p1.mode==0 && NormalPlayerCamera(p0.controller,p0,base);
}
bool em2::NativeOnlineJoinAllowed(uint8_t* base) {
    const auto manager=PPC_LOAD_U32(0x833DAB60),profile=PPC_LOAD_U32(0x833DC87C);
    if(!manager || !profile || PPC_LOAD_U32(profile+24)!=1 || joined_player_controls.manager ||
       swap_session.manager || oswald_selected.load(std::memory_order_relaxed) || !PPC_LOAD_U8(manager+357))return false;
    const auto p0=ReadRecord(manager,0,base),p1=ReadRecord(manager,1,base);
    return p1.entity && (p1.mode==1 || p1.mode==2) && NormalPlayerCamera(p0.controller,p0,base);
}
bool em2::OswaldIsSelected(){return oswald_selected.load(std::memory_order_relaxed);}
void em2::RequestCharacterSwap() {
    if(REXCVAR_GET(em2_experimental_character_swap) && !net::SynchronizedSessionActive()) swap_requested.store(true,std::memory_order_relaxed);
}

const char* em2::CharacterSwapNotice() {
    if(swap_recovering.load()) return "Returning to Mickey...";
    if(net::SynchronizedSessionActive()) return "Character switching is unavailable during online play.";
    if(NoticeClock()<swap_notice_until.load()) return swap_notice.load();
    return oswald_selected.load()?"Playing as Oswald (Mickey is waiting)":nullptr;
}

void em2::ReadSelectedCharacterProperty(PPCContext& ctx,uint8_t* base) {
    const auto output=ctx.r3.u32;
    PPC_STORE_U32(output,3); // The game's numeric ScriptVariant / GFx value.
    PPC_STORE_U64(output+8,oswald_selected.load()?0x3FF0000000000000ull:0ull);
}

PPC_FUNC_IMPL(sub_828364F0) {
    const auto manager=ctx.r3.u32,getter=ctx.r5.u32;
    __imp__sub_828364F0(ctx,base);
    if(getter!=0x82A2DC20) return;
    // Register a separate read-only HUD property beside the native Oswald
    // property; do not overload its boolean semantics or pretend co-op joined.
    const auto output=ctx;
    const auto csr=_mm_getcsr();
    const auto stack=ctx.r1.u32-256;
    PPC_STORE_U32(stack,ctx.r1.u32);
    constexpr char name[]="PCSelectedCharacter";
    std::memcpy(PPC_RAW_ADDR(stack+128),name,sizeof(name));
    ctx.r1.u64=stack;ctx.r3.u64=stack+128;
    sub_825430D8(ctx,base);
    PPC_STORE_U32(stack+96,ctx.r3.u32);
    selected_character_property=ctx.r3.u32;
    ctx.r3.u64=manager;ctx.r4.u64=stack+96;ctx.r5.u64=em2::kSelectedCharacterGetter;
    __imp__sub_828364F0(ctx,base);
    ctx.r3.u64=stack+96;
    sub_822F1C10(ctx,base);
    ctx=output;_mm_setcsr(csr);
    REXLOG_INFO("Registered single-player character HUD property");
}

void em2::UpdateNativeJoinedPlayerControls(PPCContext& ctx,uint8_t* base) {
    em2::em1::UpdateRegisteredLevelCollisionActivity(ctx,base);
    if(direct_boot_controls_pending){
        const auto manager=PPC_LOAD_U32(0x833DAB60);
        if(manager!=direct_boot_controls_pending)direct_boot_controls_pending=0;
        else if(manager && PPC_LOAD_U8(manager+357) &&
                NormalPlayerCamera(ReadRecord(manager,0,base).controller,ReadRecord(manager,0,base),base)){
            const auto saved=ctx;const auto csr=_mm_getcsr();const auto error=GetLastError();
            const auto profile=PPC_LOAD_U32(0x833DC87C);
            if(!profile){ctx=saved;_mm_setcsr(csr);SetLastError(error);return;}
            if(PPC_LOAD_U32(profile+24)==0){
                const auto devices=PPC_LOAD_U32(profile+12);
                // Same slot/device registration as the retail JoinPadOne path.
                // It owns the device binding, count and native notifications;
                // assigning only the count leaves input routing uninitialized.
                if(PPC_LOAD_U32(profile+4)!=UINT32_MAX || !devices || !PPC_LOAD_U32(devices)){
                    ctx=saved;_mm_setcsr(csr);SetLastError(error);return;
                }
                ctx.r3.u64=profile;ctx.r4.u64=0;ctx.r5.u64=0;sub_82827B00(ctx,base);
                REXLOG_INFO("Private direct boot: native local-player registration count={} slot0_device={}",PPC_LOAD_U32(profile+24),PPC_LOAD_U32(profile+4));
            }
            if(PPC_LOAD_U32(profile+24)!=1 || PPC_LOAD_U32(profile+4)!=0){
                ctx=saved;_mm_setcsr(csr);SetLastError(error);return;
            }
            ctx.r3.u64=manager;ctx.r4.u64=0;ctx.r5.u64=1;ctx.r6.u64=1;
            sub_82A27328(ctx,base);
            ctx=saved;_mm_setcsr(csr);SetLastError(error);
            direct_boot_controls_pending=0;
            REXLOG_INFO("Private direct boot: activated player-one native controller/default policy after camera readiness");
        }
    }
    const auto joined_entity=joined_player_controls.entity;
    if(UpdateJoinedControls(joined_player_controls,ctx,base,sub_82A27328)==JoinedControlsResult::Enabled)
        REXLOG_INFO("Native co-op: activated player-two controllers for {:08X}",joined_entity);
}
void em2::UpdateCharacterSwap(PPCContext& ctx,uint8_t* base) {
    const bool synchronized=net::SynchronizedSessionActive();
    if(!synchronized)UpdateNativeJoinedPlayerControls(ctx,base);
    const bool requested=swap_requested.exchange(false,std::memory_order_relaxed) && !synchronized;
    if(!requested && !swap_session.manager) return;
    const auto manager=PPC_LOAD_U32(0x833DAB60);
    const auto profile=PPC_LOAD_U32(0x833DC87C);
    if(!manager || !profile) {
        oswald_selected.store(false,std::memory_order_relaxed);
        if(swap_session.manager) {
            // Loading may temporarily remove either global while the human
            // companion still exists. Keep rollback ownership until live
            // records are available, without following any retained pointer.
            swap_session.restoring=true;swap_session.mickey_input=0;swap_session.retry_at={};
            swap_recovering.store(true);
        } else swap_recovering.store(false);
        if(requested) {ShowSwapNotice("Character switching is unavailable right now.");REXLOG_INFO("Character swap unavailable: single-player world is not ready");}
        return;
    }
    if(SinglePlayerViewport(manager,base) && PPC_LOAD_U8(manager+680)) {
        const auto saved=ctx;const auto csr=_mm_getcsr();const auto error=GetLastError();
        ctx.r3.u64=manager;__imp__sub_8276A350(ctx,base);
        ctx=saved;_mm_setcsr(csr);SetLastError(error);
    }
    if(swap_session.manager && manager!=swap_session.manager) {
        // The retired manager and its actors may already be freed. Rebind the
        // pending transaction to current records; normal readiness/scene mode
        // checks below own recovery and property notifications from here.
        const auto ai_mode=swap_session.ai_mode;
        swap_session={manager,0,0,0,ai_mode,true};
        oswald_selected.store(false,std::memory_order_relaxed);swap_recovering.store(true);
    }
    const bool coop=PPC_LOAD_U32(profile+24)>1;
    if(coop && !swap_session.manager) {
        if(requested) {ShowSwapNotice("Character switching is for single player.");REXLOG_INFO("Character swap unavailable: native co-op controls player two");}
        return;
    }
    const auto p0=ReadRecord(manager,0,base),p1=ReadRecord(manager,1,base);
    const auto camera=p0.controller;
    if(swap_session.manager && (camera!=swap_session.camera || p0.entity!=swap_session.mickey || p1.entity!=swap_session.oswald)) {
        oswald_selected.store(false,std::memory_order_relaxed);swap_session.restoring=true;
    }
    if(coop && swap_session.manager) {
        oswald_selected.store(false,std::memory_order_relaxed);swap_session.restoring=true;
    }
    if(swap_session.manager && (synchronized || requested || !REXCVAR_GET(em2_experimental_character_swap))) {
        swap_session.restoring=true;
        oswald_selected.store(false,std::memory_order_relaxed);
    }
    const bool restoring=swap_session.manager && swap_session.restoring;
    swap_recovering.store(restoring);
    const bool ready=ReadyForSwapFrame(restoring,PPC_LOAD_U8(manager+357)!=0,camera,p0,p1,base);
    if(!ready) {
        if(requested) {ShowSwapNotice(!p1.entity?"Oswald is not available in this area.":"Wait until normal gameplay to switch characters.");REXLOG_INFO("Character swap unavailable: both characters and the normal gameplay camera are required");}
        return;
    }
    if(!swap_session.manager && (!requested || p1.mode==0)) {
        if(requested) REXLOG_INFO("Character swap unavailable: Oswald is already controlled by player two");
        return;
    }
    if(swap_session.manager && !restoring) return;
    if(restoring && !requested && std::chrono::steady_clock::now()<swap_session.retry_at) return;
    const auto& native=swap_test_calls?*swap_test_calls:native_swap_calls;

    // Run only before the native frame, outside component-pool iteration.
    // Native calls use the guest stack, but cannot leak their register/FP
    // results into the frame function that we intercepted.
    struct RestoreContext {
        PPCContext& ctx; PPCContext saved; unsigned mxcsr=_mm_getcsr();
        ~RestoreContext(){ctx=saved;_mm_setcsr(mxcsr);}
    } restore{ctx,ctx};
    auto resolve=[&](unsigned slot) {
        ctx.r3.u64=manager;ctx.r4.u64=slot;native.resolve(ctx,base);return ctx.r3.u32;
    };
    auto mode=[&](unsigned value) {
        ctx.r3.u64=manager;ctx.r4.u64=1;ctx.r5.u64=value;ctx.r6.u64=0;native.mode(ctx,base);
    };
    auto rebuild=[&](unsigned value) {
        // The mode setter deliberately does nothing when the stored mode
        // already matches. Recovery from a failed factory needs the game's
        // context-aware replacement wrapper, not another no-op mode request.
        // 8276D6C8 inverts its option byte before virtual +0x4C: our normal
        // mode(..., option=0) therefore invokes this wrapper with option=1.
        ctx.r3.u64=manager;ctx.r4.u64=1;ctx.r5.u64=value;ctx.r6.u64=1;native.rebuild(ctx,base);
    };
    auto attach=[&](uint32_t entity) {
        ctx.r3.u64=camera;ctx.r4.u64=entity;native.attach(ctx,base);
        ctx.r3.u64=camera+564;native.camera_target(ctx,base);
        return ctx.r3.u32 ? PPC_LOAD_U32(ctx.r3.u32+84) : 0u;
    };
    const auto mickey=resolve(0);
    if(!mickey) return;
    auto restore_control=[&] {
        oswald_selected.store(false,std::memory_order_relaxed);
        swap_recovering.store(true);
        native.publish(ctx,base,false,coop);
        auto result=RestoreSwapTransaction(coop,swap_session.ai_mode,p0.entity,
            [&]{
                auto state=ReadRecord(manager,1,base);
                if(state.entity && !resolve(1)) state.entity=0;
                return state;
            },mode,rebuild,[&]{return attach(mickey);});
        const auto current_p0=ReadRecord(manager,0,base);
        result.camera&=current_p0.entity==p0.entity && current_p0.controller==camera && PPC_LOAD_U32(0x833DAB60)==manager;
        native.reset_motion(ctx,base);
        if(result.Complete()) {
            swap_session={};swap_recovering.store(false);
            ShowSwapNotice(result.unavailable?"Playing as Mickey (Oswald unavailable here)":"Playing as Mickey");
            REXLOG_INFO("Character swap: verified return to Mickey; native co-op active {}, companion unavailable {}",coop,result.unavailable);
        } else {
            swap_session.restoring=true;
            swap_session.retry_at=std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
            if(!swap_session.failure_reported) {
                ShowSwapNotice("Returning to Mickey...");
                REXLOG_ERROR("Character swap restoration pending: Oswald state valid {}, Mickey camera linked {}; will retry when ready",result.character,result.camera);
            }
            swap_session.failure_reported=true;
        }
    };
    if(restoring) {
        restore_control();
        return;
    }
    if(!resolve(1)) return;
    const auto previous_mode=p1.mode;
    ctx.r3.u64=manager+20;native.player_input(ctx,base);
    const auto mickey_controller=ctx.r3.u32;
    if(!mickey_controller || PPC_LOAD_U32(mickey_controller+24)!=0x8211DCE0) {
        REXLOG_INFO("Character swap unavailable: unsupported Mickey controller");return;
    }
    // Retain the rollback contract before the first native mutation.
    swap_session={manager,camera,p0.entity,p1.entity,previous_mode,true};
    swap_session.mickey_input=mickey_controller+80;
    oswald_selected.store(true,std::memory_order_relaxed);
    native.publish(ctx,base,true,false);
    mode(0);
    const auto after=ReadRecord(manager,1,base);
    const auto oswald=resolve(1);
    // Use the native EnableAllPlayerControllers policy, including its game-
    // specific default-controller list and persistent per-controller state.
    // Replacing an AI actor alone leaves the new human actor disabled.
    ctx.r3.u64=manager;ctx.r4.u64=1;ctx.r5.u64=1;ctx.r6.u64=1;
    native.enable(ctx,base);
    native.publish(ctx,base,true,false);
    if(after.mode!=0 || !after.entity || !oswald || attach(oswald)!=after.entity) {
        REXLOG_ERROR("Character swap: native transition failed; attempting verified restoration");
        restore_control();
        return;
    }
    NeutralizeMovementIntent(mickey_controller,base);
    native.reset_motion(ctx,base);
    swap_session={manager,camera,p0.entity,after.entity,previous_mode,false};
    swap_session.mickey_input=mickey_controller+80;
    ShowSwapNotice("Playing as Oswald (Mickey is waiting)");
    REXLOG_INFO("Character swap: controlling Oswald, Mickey waits; native camera proxy linked to {:08X}",after.entity);
}

// Native interface installation, not entity ownership. The interface survives
// entity replacement and supplies the camera target through vtable slot 0x24.
PPC_FUNC_IMPL(sub_827692E0) {
    const bool trace=REXCVAR_GET(em2_trace_players);
    const auto slot=ctx.r4.u32,controller=ctx.r5.u32;
    __imp__sub_827692E0(ctx,base);
    if(trace && slot<2 && controller) {
        const auto table=PPC_LOAD_U32(controller);
        const auto target_method=table ? PPC_LOAD_U32(table+36) : 0;
        REXLOG_INFO("Player interface installed P{}: controller {:08X}, vtable {:08X}, camera target method {:08X}",
            slot,controller,table,target_method);
    }
}

PPC_FUNC_IMPL(sub_8276A828) {
    const bool trace=REXCVAR_GET(em2_trace_players);
    const auto slot=ctx.r4.u32,mode=ctx.r5.u32,caller=uint32_t(ctx.lr);
    em2::net::PrepareNativeOnlinePlayerRandom(slot,mode);
    __imp__sub_8276A828(ctx,base);
    if(trace && slot<2)
        REXLOG_INFO("Player native factory P{} mode {} caller {:08X}: entity {:08X}",slot,mode,caller,ctx.r3.u32);
}

PPC_FUNC_IMPL(sub_8276D6C8) {
    const bool trace=REXCVAR_GET(em2_trace_players);
    const auto manager=ctx.r3.u32,slot=ctx.r4.u32,mode=ctx.r5.u32,option=ctx.r6.u32;
    const auto caller=uint32_t(ctx.lr);
    Record before;
    if(trace && slot<2) before=ReadRecord(manager,slot,base);
    __imp__sub_8276D6C8(ctx,base);
    if(trace && manager && slot<2) {
        const auto after=ReadRecord(manager,slot,base);
        REXLOG_INFO("Player mode transition P{} manager {:08X} caller {:08X}: mode {} -> {} (requested {}, option {}), entity {:08X} -> {:08X}, controller {:08X} -> {:08X}",
            slot,manager,caller,before.mode,after.mode,mode,option,before.entity,after.entity,before.controller,after.controller);
    }
}

PPC_FUNC_IMPL(sub_8276CCF0) {
    const bool trace=REXCVAR_GET(em2_trace_players);
    const auto manager=ctx.r3.u32,slot=ctx.r4.u32,mode=ctx.r5.u32,option=ctx.r6.u32;
    const auto caller=uint32_t(ctx.lr);
    Record before;
    if(trace && slot<2) before=ReadRecord(manager,slot,base);
    __imp__sub_8276CCF0(ctx,base);
    if(trace && manager && slot<2) {
        const auto after=ReadRecord(manager,slot,base);
        REXLOG_INFO("Player entity replacement P{} manager {:08X} caller {:08X}: requested mode {}, option {}, entity {:08X} -> {:08X}, controller {:08X} -> {:08X}, record mode {} -> {}",
            slot,manager,caller,mode,option,before.entity,after.entity,before.controller,after.controller,before.mode,after.mode);
    }
}

namespace {
struct SwapLifecycleFixture;
thread_local SwapLifecycleFixture* swap_fixture=nullptr;
struct SwapLifecycleFixture {
    static constexpr uint32_t manager=0x20000,profile=0x21000,camera=0x22000;
    static constexpr uint32_t mickey_object=0x23000,oswald_object=0x24000,input=0x25000,proxy=0x26000;
    bool factory_ok=true,attach_ok=true,arguments_ok=true,published_selected=false,published_coop=false;
    unsigned modes=0,rebuilds=0,attachments=0,enables=0,resolves=0,resets=0,publishes=0;
    uint32_t last_mode=99,last_rebuild=99;
    uint32_t current_manager=manager;
    static void Clobber(PPCContext& ctx){ctx.r31.u64^=0xBADF00D;ctx.fpscr.enableFlushMode();}
    static void Resolve(PPCContext& ctx,uint8_t* base){
        auto& f=*swap_fixture;++f.resolves;
        f.arguments_ok &= ctx.r3.u32==f.current_manager && ctx.r4.u32<2;
        const auto slot=ctx.r4.u32;const auto record=ReadRecord(f.current_manager,slot,base);
        ctx.r3.u64=record.entity?(slot?oswald_object:mickey_object):0;Clobber(ctx);
    }
    static void Replace(PPCContext& ctx,uint8_t* base,bool rebuild){
        auto& f=*swap_fixture;
        f.arguments_ok &= ctx.r3.u32==f.current_manager && ctx.r4.u32==1 && ctx.r5.u32<=2 && ctx.r6.u32==unsigned(rebuild);
        const auto mode=ctx.r5.u32;
        if(rebuild){++f.rebuilds;f.last_rebuild=mode;}else{++f.modes;f.last_mode=mode;}
        PPC_STORE_U32(f.current_manager+352,mode);
        PPC_STORE_U32(f.current_manager+340,mode==0?0x33:f.factory_ok?0x44:0);
        Clobber(ctx);
    }
    static void Mode(PPCContext& ctx,uint8_t* base){Replace(ctx,base,false);}
    static void Rebuild(PPCContext& ctx,uint8_t* base){Replace(ctx,base,true);}
    static void Attach(PPCContext& ctx,uint8_t* base){
        auto& f=*swap_fixture;++f.attachments;
        f.arguments_ok &= ctx.r3.u32==PPC_LOAD_U32(f.current_manager+28) && (ctx.r4.u32==mickey_object || ctx.r4.u32==oswald_object);
        if(f.attach_ok)PPC_STORE_U32(proxy+84,ReadRecord(f.current_manager,ctx.r4.u32==oswald_object?1:0,base).entity);
        Clobber(ctx);
    }
    static void Target(PPCContext& ctx,uint8_t* base){
        swap_fixture->arguments_ok &= ctx.r3.u32==PPC_LOAD_U32(swap_fixture->current_manager+28)+564;ctx.r3.u64=proxy;Clobber(ctx);
    }
    static void Input(PPCContext& ctx,uint8_t*){
        swap_fixture->arguments_ok &= ctx.r3.u32==swap_fixture->current_manager+20;ctx.r3.u64=input;Clobber(ctx);
    }
    static void Enable(PPCContext& ctx,uint8_t*){
        auto& f=*swap_fixture;++f.enables;
        f.arguments_ok &= ctx.r3.u32==f.current_manager && ctx.r4.u32==1 && ctx.r5.u32==1 && ctx.r6.u32==1;Clobber(ctx);
    }
    static void Publish(PPCContext& ctx,uint8_t*,bool selected,bool coop){
        auto& f=*swap_fixture;++f.publishes;f.published_selected=selected;f.published_coop=coop;Clobber(ctx);
    }
    static void Reset(PPCContext& ctx,uint8_t*){++swap_fixture->resets;Clobber(ctx);}
    static SwapNativeCalls Calls(){return {Resolve,Mode,Rebuild,Attach,Target,Input,Enable,Publish,Reset};}
};

bool TestSwapLifecycle(uint8_t* base) {
    using F=SwapLifecycleFixture;
    const auto calls=F::Calls();
    struct Restore {
        const SwapNativeCalls* previous=swap_test_calls;SwapLifecycleFixture* fixture=swap_fixture;
        bool experimental=REXCVAR_GET(em2_experimental_character_swap);unsigned csr=_mm_getcsr();
        ~Restore(){swap_test_calls=previous;swap_fixture=fixture;swap_requested.store(false);oswald_selected.store(false);
            swap_recovering.store(false);swap_session={};joined_player_controls={};
            REXCVAR_SET(em2_experimental_character_swap,experimental);_mm_setcsr(csr);}
    } restore;
    swap_test_calls=&calls;
    SwapLifecycleFixture fixture;swap_fixture=&fixture;
    PPCContext ctx{};ctx.r1.u64=0x3F000;ctx.r3.u64=0xAA;ctx.r31.u64=0x12345678;ctx.lr=0x87654321;ctx.fpscr.InitHost();
    const auto initial=ctx;const auto initial_csr=_mm_getcsr();
    auto check=[](bool value,const char* reason){if(!value)REXLOG_ERROR("Character swap lifecycle: {}",reason);return value;};
    auto frame=[&]{
        em2::UpdateCharacterSwap(ctx,base);
        return check(fixture.arguments_ok && !std::memcmp(&ctx,&initial,sizeof(ctx)) && _mm_getcsr()==initial_csr,
                     "native calls must have correct arguments and leave the outer frame context/FP state intact");
    };
    auto start=[&]{
        fixture={};swap_session={};joined_player_controls={};oswald_selected.store(false);swap_recovering.store(false);swap_requested.store(false);
        REXCVAR_SET(em2_experimental_character_swap,true);
        PPC_STORE_U32(0x833DAB60,F::manager);PPC_STORE_U32(0x833DC87C,F::profile);
        PPC_STORE_U32(F::profile+24,1);
        PPC_STORE_U32(F::manager+20,0x11);PPC_STORE_U32(F::manager+28,F::camera);PPC_STORE_U32(F::manager+32,0);
        PPC_STORE_U32(F::manager+340,0x22);PPC_STORE_U32(F::manager+352,1);PPC_STORE_U8(F::manager+357,1);
        PPC_STORE_U32(F::camera,0x82112778);PPC_STORE_U32(F::camera+564,F::proxy);
        PPC_STORE_U32(F::camera+392,0x27000);PPC_STORE_U32(F::camera+24,0x27000);PPC_STORE_U32(F::proxy+84,0x11);
        PPC_STORE_U32(F::input+24,0x8211DCE0);PPC_STORE_U32(F::input+248,123);PPC_STORE_U32(F::input+252,456);
        em2::RequestCharacterSwap();
        return frame() && check(oswald_selected.load() && !swap_session.restoring && swap_session.oswald==0x33 &&
            PPC_LOAD_U32(F::proxy+84)==0x33 && fixture.enables==1 && fixture.published_selected && !fixture.published_coop &&
            !PPC_LOAD_U32(F::input+248) && !PPC_LOAD_U32(F::input+252),"actual update must complete entry into Oswald control");
    };
    auto returned=[&]{return check(!oswald_selected.load() && !swap_recovering.load() && !swap_session.manager &&
        PPC_LOAD_U32(F::proxy+84)==0x11 && !fixture.published_selected,"recovery must verify Mickey's camera and clear ownership");};
    unsigned scenarios=0;
    if(!start())return false;
    PPC_STORE_U32(F::manager+340,0); // Death/despawn while the slot is still human.
    if(!frame() || !returned() || !check(fixture.last_mode==1 && PPC_LOAD_U32(F::manager+340)==0x44,
        "missing actor must reach the factory instead of waiting for its own recovery"))return false;
    ++scenarios;

    if(!start())return false;
    PPC_STORE_U32(F::manager+340,0);PPC_STORE_U32(F::manager+352,2);
    if(!frame() || !returned() || !check(fixture.last_rebuild==2 && PPC_LOAD_U32(F::manager+352)==2,
        "2D transition must rebuild the current scene's 2D AI"))return false;
    ++scenarios;

    if(!start())return false;
    PPC_STORE_U32(F::manager+340,0);PPC_STORE_U8(F::manager+357,0);
    const auto calls_before=fixture.resolves+fixture.modes+fixture.rebuilds+fixture.attachments;
    if(!frame() || !check(swap_session.restoring && fixture.resolves+fixture.modes+fixture.rebuilds+fixture.attachments==calls_before,
        "loading/disabled-player state must defer native recovery"))return false;
    PPC_STORE_U8(F::manager+357,1);PPC_STORE_U32(F::camera+24,0x28000);
    if(!frame() || !check(swap_recovering.load() && !oswald_selected.load() &&
        fixture.resolves+fixture.modes+fixture.rebuilds+fixture.attachments==calls_before &&
        std::strcmp(em2::CharacterSwapNotice(),"Returning to Mickey...")==0,"scripted camera must defer native recovery and report pending control"))return false;
    PPC_STORE_U32(F::camera+24,0x27000);
    if(!frame() || !returned())return false;
    ++scenarios;

    if(!start())return false;
    constexpr uint32_t respawn_camera=0x2A000;
    PPC_STORE_U32(F::manager+20,0x66);PPC_STORE_U32(F::manager+28,respawn_camera);
    PPC_STORE_U32(respawn_camera,0x82112778);PPC_STORE_U32(respawn_camera+564,F::proxy);
    PPC_STORE_U32(respawn_camera+392,0x27000);PPC_STORE_U32(respawn_camera+24,0x27000);
    PPC_STORE_U32(F::manager+340,0x77);PPC_STORE_U32(F::manager+352,2);
    const auto respawn_factories=fixture.modes+fixture.rebuilds;
    if(!frame() || !check(!swap_session.manager && !oswald_selected.load() && !swap_recovering.load() &&
        PPC_LOAD_U32(F::proxy+84)==0x66 && PPC_LOAD_U32(F::manager+340)==0x77 && PPC_LOAD_U32(F::manager+352)==2 &&
        fixture.modes+fixture.rebuilds==respawn_factories,"respawn must use the replacement Mickey/camera and preserve already-restored scene AI"))return false;
    ++scenarios;

    if(!start())return false;
    fixture.factory_ok=false;PPC_STORE_U32(F::manager+340,0);
    if(!frame() || !check(swap_session.restoring && swap_recovering.load() && !PPC_LOAD_U32(F::manager+340),
        "failed factory must remain pending"))return false;
    swap_session.retry_at=std::chrono::steady_clock::now()+std::chrono::hours(1);
    const auto pending_calls=fixture.resolves+fixture.modes+fixture.rebuilds;
    if(!frame() || !check(fixture.resolves+fixture.modes+fixture.rebuilds==pending_calls,"recovery must respect retry backoff"))return false;
    fixture.factory_ok=true;swap_session.retry_at={};
    if(!frame() || !returned() || !check(fixture.rebuilds==1,"retry must rebuild the missing same-mode AI"))return false;
    ++scenarios;

    if(!start())return false;
    fixture.attach_ok=false;em2::RequestCharacterSwap();
    if(!frame() || !check(swap_session.restoring && PPC_LOAD_U32(F::manager+352)==1,"camera failure must keep recovery pending"))return false;
    const auto recovered_factories=fixture.modes+fixture.rebuilds;
    fixture.attach_ok=true;swap_session.retry_at={};
    if(!frame() || !returned() || !check(fixture.modes+fixture.rebuilds==recovered_factories,"camera retry must preserve recovered AI"))return false;
    ++scenarios;

    if(!start())return false;
    PPC_STORE_U32(F::manager+340,0);PPC_STORE_U32(F::manager+352,0xFFFFFFFFu);
    const auto factories=fixture.modes+fixture.rebuilds;
    if(!frame() || !returned() || !check(fixture.modes+fixture.rebuilds==factories && !PPC_LOAD_U32(F::manager+340),
        "disabled companion area must not spawn Oswald"))return false;
    ++scenarios;

    if(!start())return false;
    PPC_STORE_U32(F::profile+24,2);PPC_STORE_U32(F::manager+340,0);
    const auto coop_factories=fixture.modes+fixture.rebuilds;
    if(!frame() || !check(swap_session.restoring && fixture.published_coop && fixture.modes+fixture.rebuilds==coop_factories,
        "missing co-op human must remain native-owned rather than becoming AI"))return false;
    PPC_STORE_U32(F::manager+340,0x55);swap_session.retry_at={};
    if(!frame() || !returned() || !check(fixture.modes+fixture.rebuilds==coop_factories && PPC_LOAD_U32(F::manager+352)==0 &&
        fixture.published_coop,"native co-op handoff must preserve the second human and co-op property"))return false;
    ++scenarios;

    if(!start())return false;
    REXCVAR_SET(em2_experimental_character_swap,false);
    if(!frame() || !returned())return false;
    ++scenarios;
    for(bool missing_profile:{false,true}) {
        if(!start())return false;
        const auto prior_calls=fixture.resolves+fixture.modes+fixture.rebuilds+fixture.attachments+fixture.publishes;
        PPC_STORE_U32(missing_profile?0x833DC87C:0x833DAB60,0);
        if(!frame() || !check(swap_session.manager==F::manager && swap_session.restoring && swap_recovering.load() &&
            !oswald_selected.load() && fixture.resolves+fixture.modes+fixture.rebuilds+fixture.attachments+fixture.publishes==prior_calls,
            "a missing manager/profile must defer recovery without discarding ownership or calling native code"))return false;
        PPC_STORE_U32(0x833DAB60,F::manager);PPC_STORE_U32(0x833DC87C,F::profile);
        if(!frame() || !returned() || !check(PPC_LOAD_U32(F::manager+352)==1,
            "returning globals must restore the human companion to scene AI"))return false;
        ++scenarios;
    }
    for(unsigned replacement_case=0;replacement_case<5;++replacement_case) {
        const bool coop=replacement_case==1;
        const bool missing=replacement_case>=3;
        const uint32_t scene_mode=coop || replacement_case==2?0:replacement_case==3?UINT32_MAX:2;
        const uint32_t expected_mode=replacement_case==2?1:scene_mode;
        const uint32_t expected_actor=replacement_case==2 || replacement_case==4?0x44:missing?0:0x77;
        if(!start())return false;
        constexpr uint32_t replacement=0x10000,replacement_camera=0x2A000;
        std::memcpy(base+replacement,base+F::manager,640);
        PPC_STORE_U32(replacement+20,0x66);PPC_STORE_U32(replacement+28,replacement_camera);
        PPC_STORE_U32(replacement+340,missing?0:0x77);PPC_STORE_U32(replacement+352,scene_mode);
        PPC_STORE_U8(replacement+357,0);
        PPC_STORE_U32(replacement_camera,0x82112778);PPC_STORE_U32(replacement_camera+564,F::proxy);
        PPC_STORE_U32(replacement_camera+392,0x27000);PPC_STORE_U32(replacement_camera+24,0x27000);
        PPC_STORE_U32(0x833DAB60,replacement);PPC_STORE_U32(F::profile+24,coop?2:1);
        fixture.current_manager=replacement;
        struct Protect {
            uint8_t* address;DWORD previous=0;bool active=false;
            ~Protect(){if(active){DWORD ignored;VirtualProtect(address,4096,previous,&ignored);}}
        } retired{base+F::manager};
        retired.active=VirtualProtect(retired.address,4096,PAGE_NOACCESS,&retired.previous)!=0;
        if(!check(retired.active,"retired manager page must be inaccessible during replacement recovery"))return false;
        const auto prior_publishes=fixture.publishes,prior_factories=fixture.modes+fixture.rebuilds;
        if(!frame() || !check(swap_session.manager==replacement && swap_session.restoring && swap_recovering.load() &&
            !oswald_selected.load() && fixture.publishes==prior_publishes,
            "replacement manager must inherit pending recovery but wait for enabled gameplay"))return false;
        PPC_STORE_U8(replacement+357,1);
        if(!frame() || !check(!swap_session.manager && !oswald_selected.load() && !swap_recovering.load() &&
            PPC_LOAD_U32(F::proxy+84)==0x66 && PPC_LOAD_U32(replacement+340)==expected_actor &&
            PPC_LOAD_U32(replacement+352)==expected_mode && fixture.publishes==prior_publishes+1 &&
            !fixture.published_selected && fixture.published_coop==coop &&
            fixture.modes+fixture.rebuilds==prior_factories+unsigned(replacement_case==2 || replacement_case==4),
            "replacement recovery must publish Mickey, attach the new camera and preserve scene AI or real co-op"))return false;
        const auto settled_calls=fixture.resolves+fixture.attachments+fixture.publishes;
        if(!frame() || !check(fixture.resolves+fixture.attachments+fixture.publishes==settled_calls,
            "completed replacement recovery must not repeat native notifications"))return false;
        ++scenarios;
    }
    REXLOG_INFO("Character swap outer-frame lifecycle passed {} scenarios with an injected native backend, including missing manager/profile and inaccessible retired-manager replacement; live campaign acceptance remains pending",scenarios);
    return true;
}
}

namespace {
void CameraReferenceAdd(PPCContext& ctx,uint8_t* base) {
    PPC_STORE_U32(ctx.r3.u32+200,PPC_LOAD_U32(ctx.r3.u32+200)+1);
}
void CameraReferenceRelease(PPCContext& ctx,uint8_t* base) {
    PPC_STORE_U32(ctx.r3.u32+200,PPC_LOAD_U32(ctx.r3.u32+200)-1);
}
bool TestNativePossessionCamera(uint8_t* base) {
    // Run the real direct-record installer, registry/hash, camera resolver,
    // component lookup and NiPointer setter. Only entity refcount virtuals
    // are fixture callbacks; the camera choice/cache write are native code.
    if(!VirtualAlloc(base+PPC_IMAGE_BASE+PPC_IMAGE_SIZE,4096,MEM_COMMIT,PAGE_READWRITE))return false;
    constexpr uint32_t code=uint32_t(PPC_CODE_BASE),manager=0xC000,profile=0xD000;
    constexpr uint32_t primary=0xE000,secondary=0xE800,registry=0x10000,buckets=0x11000;
    constexpr uint32_t actor=0x18008,component=0x19000,table=0x19800;
    constexpr uint32_t primary_entity=0x1A000,secondary_entity=0x1B000,reference_table=0x1C000;
    PPC_LOOKUP_FUNC(base,code)=__imp__sub_827B58D0;
    PPC_LOOKUP_FUNC(base,code+4)=__imp__sub_82A69948;
    PPC_LOOKUP_FUNC(base,code+8)=__imp__sub_827DDD88;
    PPC_LOOKUP_FUNC(base,code+12)=CameraReferenceAdd;
    PPC_LOOKUP_FUNC(base,code+16)=CameraReferenceRelease;
    const bool selected_before=oswald_selected.load();
    struct RestoreSelected {bool value;~RestoreSelected(){oswald_selected.store(value);}} restore{selected_before};
    auto check=[](bool condition,const char* message){if(!condition)REXLOG_ERROR("Native possessed camera regression: {}",message);return condition;};
    unsigned cases=0;
    for(unsigned fallback=0;fallback<2;++fallback) for(unsigned players:{1u,2u})
    for(bool selected:{false,true}) for(unsigned slot:{0u,1u}) {
        std::memset(base+manager,0,0x400);std::memset(base+registry,0,0x3000);
        std::memset(base+actor-8,0,0x5000);
        PPC_STORE_U32(0x833DAB60,manager);PPC_STORE_U32(0x833DC87C,profile);PPC_STORE_U32(profile+24,players);
        PPC_STORE_U32(manager+28,primary);PPC_STORE_U32(manager+348,secondary);
        PPC_STORE_U32(manager+20,0x22);PPC_STORE_U32(manager+340,0x22);
        PPC_STORE_U32(primary+20,0x31);PPC_STORE_U32(secondary+20,0x32);
        PPC_STORE_U32(0x833DA7E4,registry);PPC_STORE_U32(registry+4,code);PPC_STORE_U32(registry+8,code+4);
        PPC_STORE_U32(registry+16,buckets);PPC_STORE_U32(registry+20,4);
        for(unsigned i=0;i<3;++i) {
            const auto handle=i?0x30u+i:0x22u,node=0x12000u+i*16,key=0x13000u;
            PPC_STORE_U32(key,handle);PPCContext hash{};hash.r3.u64=key;__imp__sub_827B58D0(hash,base);
            const auto bucket=hash.r3.u32%4;
            PPC_STORE_U32(node,handle);PPC_STORE_U32(node+4,i==0?actor:i==1?primary_entity:secondary_entity);
            PPC_STORE_U32(node+8,PPC_LOAD_U32(buckets+bucket*4));PPC_STORE_U32(buckets+bucket*4,node);
        }
        PPC_STORE_U32(actor+88,0x18800);PPC_STORE_U32(actor+36,0x18900);
        PPC_STORE_U8(0x833DAE58,3);PPC_STORE_U8(0x833DBD04,4);
        PPC_STORE_U8(0x18803,fallback?255:0);PPC_STORE_U8(0x18804,0);
        PPC_STORE_U32(0x18900,component+8);PPC_STORE_U32(component,table);
        PPC_STORE_U32(table+108,code+8);PPC_STORE_U32(table+120,code+8);
        PPC_STORE_U32(reference_table,code+12);PPC_STORE_U32(reference_table+4,code+16);
        PPC_STORE_U32(primary_entity,reference_table);PPC_STORE_U32(secondary_entity,reference_table);
        PPC_STORE_U32(component+80,secondary_entity);PPC_STORE_U32(secondary_entity+200,1);
        oswald_selected.store(selected);
        PPCContext ctx{};ctx.fpscr.InitHost();ctx.r1.u64=0x3F000;ctx.lr=0x12345678;
        ctx.r3.u64=manager;ctx.r4.u64=slot;
        __imp__sub_8276C0C0(ctx,base);
        const bool primary_expected=slot==0 || (players==1 && selected);
        const auto expected=primary_expected?primary_entity:secondary_entity;
        if(!check(PPC_LOAD_U32(component+80)==expected && ctx.r1.u32==0x3F000 && ctx.lr==0x12345678,
                  "direct native installer must cache the displayed camera entity during possession, retaining real co-op ownership"))return false;
        if(!check(PPC_LOAD_U32(primary_entity+200)==unsigned(primary_expected) &&
                  PPC_LOAD_U32(secondary_entity+200)==unsigned(!primary_expected),
                  "native cache replacement must release the hidden camera and retain the displayed camera exactly once"))return false;
        // A live entity update must be observed through the retained reference
        // without reinstalling/copying a pose. These are fixture markers, not
        // an assertion about the camera's mathematical rotation or game feel.
        for(unsigned turn=0;turn<4;++turn) {
            PPC_STORE_U32(primary_entity+240,turn);PPC_STORE_U32(secondary_entity+240,99);
            const auto cached=PPC_LOAD_U32(component+80);
            if(!check(PPC_LOAD_U32(cached+240)==(primary_expected?turn:99),"cached reference must observe live primary entity updates instead of the hidden secondary entity"))return false;
        }
        ++cases;
    }
    REXLOG_INFO("Native possessed camera passed {} direct-record installer cases: original registry/hash and NiPointer setter, both native component branches, solo/co-op, both slots, reference ownership and live entity updates; visible movement/aim remains a gameplay check",cases);
    return true;
}
}

bool em2::TestNativePlayerActions() {
    if(!TestSwapRestoration()) return false;
    {
        PPCContext missing{};missing.fpscr.InitHost();missing.r4.u64=0x123456789ABCDEF0ull;
        missing.r1.u64=0xBAD;missing.lr=0x82B2C26C;
        std::array<uint8_t,sizeof(PPCContext)> before;std::memcpy(before.data(),&missing,sizeof(missing));
        const auto csr=_mm_getcsr();const auto error=GetLastError();
        sub_82A105F8(missing,nullptr);
        if(std::memcmp(before.data(),&missing,sizeof(missing)) || _mm_getcsr()!=csr || GetLastError()!=error) {
            REXLOG_ERROR("Missing camera reset changed native caller state");return false;
        }
        REXLOG_INFO("Missing native camera reset passed: null receiver needs no guest memory and preserves complete caller, FP and error state");
    }
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));
    if(!base) return false;
    struct Release {
        uint8_t* base; unsigned mxcsr=_mm_getcsr();
        ~Release() {_mm_setcsr(mxcsr);VirtualFree(base,0,MEM_RELEASE);}
    } release{base};
    if(!VirtualAlloc(base,0x40000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0x82000000,0x10000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0x82030000,0x10000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0x82070000,0x10000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0x833D0000,0x10000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0xA0000000ull,0x10000,MEM_COMMIT,PAGE_READWRITE)) return false;
    PPC_STORE_U8(0x8207873C,'2'); PPC_STORE_U8(0x82078740,'2');
    PPC_STORE_U32(0x8203D278,0x2000);
    constexpr uint32_t action=0x100;
    unsigned comparisons=0;
    auto compare=[&](uint32_t name) {
        PPC_STORE_U32(action,name);
        for(uint32_t slot : {0u,1u,2u,0xFFFFFFFFu}) {
            PPCContext reference{};reference.fpscr.InitHost();
            reference.r1.u64=0x3F000;reference.lr=0x12345678;
            reference.r4.u64=action;reference.r5.u64=slot;
            __imp__sub_82769268(reference,base);
            const auto expected=reference.r3.u32;
            PPCContext actual{};actual.r1.u64=0x3F000;actual.lr=0x12345678;
            actual.r4.u64=action;actual.r5.u64=slot;
            sub_82769268(actual,base);
            ++comparisons;
            if(actual.r3.u32!=expected || actual.r1.u32!=0x3F000 || actual.lr!=0x12345678) {
                REXLOG_ERROR("Native player action mismatch: string {:08X}, slot {}, expected {}, actual {}",name,slot,expected,actual.r3.u32);
                return false;
            }
        }
        return true;
    };
    const auto saved=REXCVAR_GET(em2_native_player_actions);
    const auto saved_experimental=REXCVAR_GET(em2_experimental_character_swap);
    REXCVAR_SET(em2_native_player_actions,true);
    struct Restore {
        bool saved,experimental;
        ~Restore(){REXCVAR_SET(em2_native_player_actions,saved);REXCVAR_SET(em2_experimental_character_swap,experimental);
            oswald_selected.store(false);swap_recovering.store(false);swap_requested.store(false);swap_session={};joined_player_controls={};}
    } restore{saved,saved_experimental};
    if(!TestSwapLifecycle(base) || !TestNativePossessionCamera(base))return false;
    auto check=[](bool condition,const char* message) {
        if(!condition) REXLOG_ERROR("Player swap policy test failed: {}",message);
        return condition;
    };
    // Both the character-specific and generic dispatchers must stop the
    // waiting actor before any ability handler runs, preserving all other
    // guest state. This previously allowed Mickey to jump off camera.
    swap_session.mickey_input=0x9050;oswald_selected.store(true);
    for(auto dispatch : {sub_82A3C930,sub_82783CE8}) {
        PPCContext actual{};actual.r3.u64=0x9050;actual.r4.u64=0xABCDEF;
        actual.r5.u64=0x123456;actual.lr=0x87654321;actual.fpscr.InitHost();
        auto expected=actual;expected.r3.u64=0;
        dispatch(actual,base);
        if(!check(!std::memcmp(&actual,&expected,sizeof(actual)),
            "waiting Mickey must ignore actions without modifying guest state")) return false;
    }
    swap_session={};oswald_selected.store(false);
    constexpr uint32_t input_profile=0xB000;
    PPC_STORE_U32(0x833DC87C,input_profile);
    PPC_STORE_U32(input_profile+4,2);PPC_STORE_U32(input_profile+8,0xFFFFFFFF);
    PPC_STORE_U32(input_profile+24,1);
    for(bool selected : {false,true}) for(uint32_t query_profile : {input_profile,input_profile+32})
        for(uint32_t slot : {0u,1u,2u,0xFFFFFFFFu}) {
            oswald_selected.store(selected);
            PPCContext actual{};actual.fpscr.InitHost();actual.r3.u64=query_profile;
            actual.r4.u64=slot;actual.lr=0x12345678;
            auto expected=actual;
            if(selected && query_profile==input_profile && slot==1) expected.r4.u64=0;
            __imp__sub_82827320(expected,base);expected.r4.u64=slot;
            sub_82827320(actual,base);
            if(!check(!std::memcmp(&actual,&expected,sizeof(actual)) &&
                PPC_LOAD_U32(input_profile+8)==0xFFFFFFFF && PPC_LOAD_U32(input_profile+24)==1,
                "selected Oswald borrows only the active profile device without joining co-op")) return false;
        }
    PPC_STORE_U32(0x833DC87C,0);oswald_selected.store(false);
    {
        constexpr uint32_t manager=0xC000,profile=0xD000,camera=0xE000,entity=0x22;
        if(!check(JoinedHumanPlayer(true,2,{1,0,1},{entity,0,0}) &&
            JoinedHumanPlayer(true,2,{1,0,2},{entity,0,0}) &&
            !JoinedHumanPlayer(false,2,{1,0,1},{entity,0,0}) &&
            !JoinedHumanPlayer(true,1,{1,0,1},{entity,0,0}) &&
            !JoinedHumanPlayer(true,2,{1,0,0},{entity,0,0}) &&
            !JoinedHumanPlayer(true,2,{1,0,1},{0,0,0}) &&
            !JoinedHumanPlayer(true,2,{1,0,1},{entity,0,1}),
            "controller activation requires a successful native AI-to-human co-op join")) return false;
        auto setup=[&] {
            PPC_STORE_U32(0x833DAB60,manager);PPC_STORE_U32(0x833DC87C,profile);
            PPC_STORE_U32(profile+24,2);PPC_STORE_U32(manager+20,0x11);
            PPC_STORE_U32(manager+28,camera);PPC_STORE_U32(manager+32,0);
            PPC_STORE_U32(manager+340,entity);PPC_STORE_U32(manager+352,0);PPC_STORE_U8(manager+357,1);
            PPC_STORE_U32(camera,0x82112778);PPC_STORE_U32(camera+564,0x11);
            PPC_STORE_U32(camera+392,0xF000);PPC_STORE_U32(camera+24,0xF000);
        };
        {
            struct Restore {PPCFunc* previous=viewport_test_activate;~Restore(){viewport_test_activate=previous;oswald_selected.store(false);}} restore;
            viewport_test_activate=+[](PPCContext& native,uint8_t* base){PPC_STORE_U8(native.r3.u32+680,1);};
            for(uint32_t players:{0u,1u,2u})for(bool selected:{false,true}) {
                setup();PPC_STORE_U32(profile+24,players);PPC_STORE_U32(manager+348,camera+0x800);
                PPC_STORE_U8(manager+680,0);PPC_STORE_U8(manager+681,0);oswald_selected.store(selected);
                PPCContext call{};call.fpscr.InitHost();call.r1.u64=0x3F000;call.r3.u64=manager;call.r4.u64=1;
                __imp__sub_8276E4F8(call,base);
                if(!check(PPC_LOAD_U8(manager+681)==1 && PPC_LOAD_U8(manager+680)==(players!=1),
                    "actual native respawn split-screen enable respects signed-in count, including possessed Oswald and real co-op"))return false;
                const auto result=PossessedCamera(manager,camera+0x800,base);
                if(!check(result==((players==1 && selected)?camera:camera+0x800) &&
                    PossessedCamera(manager,camera,base)==camera && PossessedCamera(manager,0,base)==0,
                    "possessed movement/aim uses the displayed primary camera; real co-op and missing cameras retain native ownership"))return false;
            }
            PPC_STORE_U8(manager+680,0);oswald_selected.store(false);
        }
        setup();
        unsigned enabled=0;bool arguments_ok=true;
        auto enable=[&](PPCContext& native,uint8_t* memory) {
            ++enabled;arguments_ok &= memory==base && native.r3.u32==manager && native.r4.u32==1 &&
                native.r5.u32==1 && native.r6.u32==1;
            native.r31.u64=0xABCDEF;native.r3.u64=0;
            _mm_setcsr(_mm_getcsr()^0x8000);
        };
        PPCContext actual{};actual.fpscr.InitHost();actual.r1.u64=0x3F000;actual.r3.u64=0xABC;
        actual.lr=0x12345678;const auto expected=actual;const auto csr=_mm_getcsr();
        JoinedPlayerControls pending{manager,entity};
        if(!check(UpdateJoinedControls(pending,actual,base,enable)==JoinedControlsResult::Enabled &&
            UpdateJoinedControls(pending,actual,base,enable)==JoinedControlsResult::None && enabled==1 && arguments_ok &&
            !std::memcmp(&actual,&expected,sizeof(actual)) && _mm_getcsr()==csr,
            "native co-op activation runs once and restores the full guest context and FP state")) return false;
        pending={manager,entity};PPC_STORE_U8(manager+357,0);
        if(!check(UpdateJoinedControls(pending,actual,base,enable)==JoinedControlsResult::Deferred && pending.manager==manager,
            "unready gameplay defers joined controller activation")) return false;
        PPC_STORE_U8(manager+357,1);PPC_STORE_U32(camera+24,0xF004);
        if(!check(UpdateJoinedControls(pending,actual,base,enable)==JoinedControlsResult::Deferred && enabled==1,
            "cutscene and special cameras do not force-enable player-two controllers")) return false;
        PPC_STORE_U32(camera+24,0xF000);
        if(!check(UpdateJoinedControls(pending,actual,base,enable)==JoinedControlsResult::Enabled && enabled==2,
            "deferred native co-op activation resumes when normal gameplay returns")) return false;
        for(unsigned condition=0;condition<5;++condition) {
            setup();pending={manager,entity};
            if(condition==0) PPC_STORE_U32(0x833DAB60,manager+64);
            if(condition==1) PPC_STORE_U32(0x833DC87C,0);
            if(condition==2) PPC_STORE_U32(profile+24,1);
            if(condition==3) PPC_STORE_U32(manager+340,entity+1);
            if(condition==4) PPC_STORE_U32(manager+352,1);
            if(!check(UpdateJoinedControls(pending,actual,base,enable)==JoinedControlsResult::Cancelled && !pending.manager && enabled==2 &&
                !std::memcmp(&actual,&expected,sizeof(actual)) && _mm_getcsr()==csr,
                "world/profile changes, leaving co-op and replaced/AI actors cancel stale activation")) return false;
        }
        for(unsigned condition=0;condition<7;++condition) {
            setup();joined_player_controls={};PPC_STORE_U32(manager+348,camera+0x800);
            if(condition==1)PPC_STORE_U32(profile+24,1);
            if(condition==2)PPC_STORE_U32(manager+352,1);
            if(condition==3)PPC_STORE_U32(manager+348,0);
            if(condition==4)PPC_STORE_U8(manager+357,0);
            if(condition==5)PPC_STORE_U32(camera+24,0xF004);
            if(condition==6)joined_player_controls={manager,entity};
            if(!check(NativeCoopReady(base)==(condition==0),"online attachment requires initialized native humans, camera and completed controller activation"))return false;
        }
        joined_player_controls={};
        for(uint32_t ai_mode : {1u,2u})for(unsigned condition=0;condition<9;++condition) {
            setup();PPC_STORE_U32(profile+24,1);PPC_STORE_U32(manager+352,ai_mode);
            if(condition==1)PPC_STORE_U32(profile+24,2);
            if(condition==2)PPC_STORE_U32(manager+352,0);
            if(condition==3)PPC_STORE_U32(manager+340,0);
            if(condition==4)PPC_STORE_U8(manager+357,0);
            if(condition==5)PPC_STORE_U32(camera+24,0xF004);
            if(condition==6)joined_player_controls={manager,entity};
            if(condition==7)oswald_selected.store(true);
            if(condition==8)swap_session.manager=manager;
            const auto allowed=NativeOnlineJoinAllowed(base);
            joined_player_controls={};oswald_selected.store(false);swap_session={};
            if(!check(allowed==(condition==0),"automatic online join requires normal single-player gameplay, AI Oswald and no pending character/control transaction"))return false;
        }
        PPC_STORE_U32(0x833DAB60,0);PPC_STORE_U32(0x833DC87C,0);
        REXLOG_INFO("Native co-op controller activation tests passed: join policy, deferred readiness, one-shot activation, cancellation and guest context/FP restoration");
    }
    constexpr uint32_t camera=0x6000;
    PPC_STORE_U32(camera,0x82112778);PPC_STORE_U32(camera+564,3);
    PPC_STORE_U32(camera+392,0x7000);PPC_STORE_U32(camera+24,0x7000);
    const Record p0{1,camera,0},p1{2,0,1};
    if(!check(ReadyForSwap(camera,p0,p1,base) && !ReadyForSwap(0,p0,p1,base) &&
        !ReadyForSwap(camera,{0,camera,0},p1,base) && !ReadyForSwap(camera,p0,{0,0,1},base) &&
        !ReadyForSwap(camera,p0,{2,0,0xFFFFFFFF},base),"absent characters and disabled modes must reject swaps")) return false;
    PPC_STORE_U32(camera+24,0x7100);
    if(!check(!ReadyForSwap(camera,p0,p1,base),"cutscene/special camera must reject swaps")) return false;
    PPC_STORE_U32(camera+24,0x7000);
    constexpr uint32_t component=0x9000;
    std::memset(PPC_RAW_ADDR(component),0xA5,512);
    if(!check(!NeutralizeMovementIntent(component,base),"unknown controller must not be modified")) return false;
    PPC_STORE_U32(component+24,0x8211DCE0);
    if(!check(NeutralizeMovementIntent(component,base) && !PPC_LOAD_U32(component+248) && !PPC_LOAD_U32(component+252) &&
        PPC_LOAD_U32(component+244)==0xA5A5A5A5 && PPC_LOAD_U32(component+256)==0xA5A5A5A5,"parked movement clears only verified command fields")) return false;
    REXCVAR_SET(em2_experimental_character_swap,false);
    RequestCharacterSwap();
    if(!check(!swap_requested.load(),"disabled experimental feature must ignore F6 requests")) return false;
    REXCVAR_SET(em2_experimental_character_swap,true);
    PPCContext guarded{};guarded.r1.u64=0x3F000;guarded.lr=0x87654321;guarded.r3.u64=0xAA;
    guarded.fpscr.InitHost();
    const auto initial=guarded;
    const auto initial_mxcsr=_mm_getcsr();
    auto reject=[&] {
        RequestCharacterSwap();UpdateCharacterSwap(guarded,base);
        return check(!swap_requested.load() && !oswald_selected.load() && !swap_session.manager &&
            std::memcmp(&guarded,&initial,sizeof(guarded))==0 && _mm_getcsr()==initial_mxcsr,
            "refused swaps must preserve guest context, FP state and ownership");
    };
    if(!reject()) return false; // No world/profile.
    PPC_STORE_U32(0x833DAB60,0x3000);PPC_STORE_U32(0x833DC87C,0x4000);
    PPC_STORE_U32(0x4000+24,2);
    if(!reject()) return false; // Native co-op owns the second player.
    PPC_STORE_U32(0x4000+24,1);
    PPC_STORE_U32(0x3000+20,1);PPC_STORE_U32(0x3000+28,camera);
    PPC_STORE_U32(0x3000+352,1);PPC_STORE_U8(0x3000+357,1);
    if(!reject()) return false; // Prologue has no Oswald entity.
    PPC_STORE_U32(0x3000+340,2);PPC_STORE_U32(0x3000+352,0);
    if(!reject()) return false; // P1 already human, even with one profile.
    PPC_STORE_U32(0x833DAB60,0);PPC_STORE_U32(0x833DC87C,0);
    for(bool selected : {false,true}) {
        oswald_selected.store(selected);
        PPCContext script{};script.r1.u64=0x3F000;script.r3.u64=0x8000;script.fpscr.InitHost();
        sub_82A2DC20(script,base);
        if(!check(script.r3.u32==1 && PPC_LOAD_U32(0x8000)==3 &&
            PPC_LOAD_U64(0x8008)==(selected?0x3FF0000000000000ull:0ull),"Oswald human-condition script variant")) return false;
    }
    oswald_selected.store(false);
    for(bool selected : {false,true}) {
        oswald_selected.store(selected);
        PPCContext property{};property.r3.u64=0x2400;property.lr=0x12345678;
        const auto before=property;
        ReadSelectedCharacterProperty(property,base);
        if(!check(PPC_LOAD_U32(0x2400)==3 &&
            PPC_LOAD_U64(0x2408)==(selected?0x3FF0000000000000ull:0ull) &&
            std::memcmp(&property,&before,sizeof(property))==0,
            "selected-character HUD property must preserve guest context and return a numeric selection")) return false;
    }
    oswald_selected.store(false);
    REXLOG_INFO("Experimental swap guard tests passed: inactive world, native co-op, missing Oswald, existing P2, camera isolation, script variant and context preservation");
    unsigned routing_comparisons=0;
    for(const char* name : {"Movement","Movement2"}) {
        std::memcpy(PPC_RAW_ADDR(0x82078008),name,std::strlen(name)+1);
        PPC_STORE_U32(action,0x82078008);
        for(bool selected : {false,true}) for(bool native : {false,true}) {
            oswald_selected.store(selected);REXCVAR_SET(em2_native_player_actions,native);
            for(uint32_t caller : {0x82A3C9E4u,0x82A3C9FCu,0x82A148D8u,0x82A4D8B8u,0x82A5D324u,0u}) for(uint32_t slot=0;slot<4;++slot) {
                PPCContext expected{},actual{};expected.fpscr.InitHost();actual.fpscr.InitHost();
                expected.r1.u64=actual.r1.u64=0x3F000;expected.lr=actual.lr=caller;
                expected.r4.u64=actual.r4.u64=action;actual.r5.u64=slot;
                expected.r5.u64=slot;
                __imp__sub_82769268(expected,base);sub_82769268(actual,base);
                if(!check(actual.r3.u32==expected.r3.u32 && actual.r1.u32==0x3F000 && actual.lr==caller,
                    "production wrapper routing must match native slot behavior with optimization on and off")) return false;
                ++routing_comparisons;
            }
        }
    }
    oswald_selected.store(false);REXCVAR_SET(em2_native_player_actions,true);
    REXLOG_INFO("Character action wrapper passed {} comparisons preserving native action slots while switching, including translated fallback",routing_comparisons);
    for(uint32_t name : {0u,0x2000u,0x8207873Cu,0x82078740u}) if(!compare(name)) return false;
    uint32_t random=0xE21C360;
    for(unsigned sample=0;sample<192;++sample) {
        std::string name;
        if(sample<8) name=std::array<const char*,8>{"Movement","Movement2","Jump","JumpRelease2","MouseCursor2","PaintStream","2",""}[sample];
        else {
            for(unsigned i=0;i<sample%96;++i) {
                random=random*1664525u+1013904223u;
                name+=char(' '+(random>>24)%95);
            }
            // Exercise the interned length representation independently of
            // strlen, including bytes following an embedded NUL.
            if(sample%13==0 && name.size()>4) {name[2]=0;name.back()='2';}
        }
        for(uint32_t address : {0x1008u,0x82078008u,0xA0001008u}) {
            std::memcpy(PPC_RAW_ADDR(address),name.c_str(),name.size()+1);
            PPC_STORE_U16(address-2,uint16_t(name.size()));
            if(!compare(address)) return false;
        }
    }
    if(REXCVAR_GET(em2_benchmark_player_actions)) {
        constexpr unsigned iterations=10000;
        auto run=[&](bool native) {
            PPCContext ctx{};ctx.fpscr.InitHost();ctx.r1.u64=0x3F000;
            const auto start=std::chrono::steady_clock::now();
            uint32_t total=0;
            for(unsigned i=0;i<iterations;++i) {
                ctx.r3.u64=0;ctx.r4.u64=action;ctx.r5.u64=i&1;
                if(native) sub_82769268(ctx,base);else __imp__sub_82769268(ctx,base);
                total+=ctx.r3.u32;
            }
            if(total!=iterations/2) REXLOG_ERROR("Player action benchmark checksum mismatch");
            return std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/iterations;
        };
        for(const char* name : {"Movement","MouseCursor2"}) for(uint32_t address : {0x82078008u,0xA0001008u}) {
            const auto length=std::strlen(name);
            std::memcpy(PPC_RAW_ADDR(address),name,length+1);
            PPC_STORE_U16(address-2,uint16_t(length));
            PPC_STORE_U32(action,address);
            std::array<double,5> translated{},native{};
            for(unsigned round=0;round<5;++round) {
                if(round&1) {native[round]=run(true);translated[round]=run(false);}
                else {translated[round]=run(false);native[round]=run(true);}
            }
            std::sort(translated.begin(),translated.end());std::sort(native.begin(),native.end());
            REXLOG_INFO("Player action synthetic benchmark '{}' {}: translated {} ns/call, native {} ns/call (median of 5; not a game FPS measurement)",
                name,address<0xA0000000u?"static":"interned",translated[2],native[2]);
        }
    }
    REXLOG_INFO("Native player action filtering passed {} comparisons against translated game code",comparisons);
    return true;
}

PPC_EXTERN_FUNC(__imp__sub_831C13F0);
PPC_EXTERN_FUNC(__imp__sub_831C1360);
PPC_EXTERN_FUNC(__imp__sub_827C40A8);
namespace {
// The native join debounce uses a cached local wall clock. Retain the retail
// helper calls and timer-memory effects, replacing only this join call site's
// elapsed result with agreed ticks. Explicit resets retain future-delay semantics.
// Other timer users and offline joins continue through their retail helpers.
struct NativeJoinClock {
    bool initialized=false;
    uint32_t manager=0;
    uint64_t current_tick=0,reset_tick=0;
    double delay=0;
    uint64_t owner_generation=0,last_dispatch_tick=0;
    bool dispatch_claimed=false;
    void Begin(uint32_t owner,uint64_t tick,uint64_t generation=0){
        if(!initialized || manager!=owner || tick<current_tick || owner_generation!=generation){
            *this={};initialized=true;manager=owner;current_tick=reset_tick=tick;owner_generation=generation;
        }
        current_tick=tick;
    }
    bool ClaimDispatch(){
        if(dispatch_claimed && last_dispatch_tick==current_tick)return false;
        dispatch_claimed=true;last_dispatch_tick=current_tick;return true;
    }
    double Elapsed()const{return double(float(double(current_tick-reset_tick)/30.0-delay));}
    void Reset(double seconds){reset_tick=current_tick;delay=seconds>0?seconds:0;}
};
thread_local NativeJoinClock native_join_clock;
bool JoinTimerCall(const PPCContext& ctx,uint32_t caller){
    return canonical_join_dispatch && native_join_clock.initialized && ctx.lr==caller && ctx.r3.u32==native_join_clock.manager+696;
}
void JoinElapsed(PPCContext& ctx,uint8_t* base,PPCFunc* original){
    const bool canonical=JoinTimerCall(ctx,0x8276EFC8);
    original(ctx,base);
    if(canonical){const auto csr=_mm_getcsr();ctx.f1.f64=native_join_clock.Elapsed();_mm_setcsr(csr);}
}
void JoinReset(PPCContext& ctx,uint8_t* base,PPCFunc* original){
    const bool canonical=JoinTimerCall(ctx,0x8276F070);const double delay=ctx.f1.f64;
    original(ctx,base);
    if(canonical){const auto csr=_mm_getcsr();native_join_clock.Reset(delay);_mm_setcsr(csr);}
}
bool TestRetailJoinHelpers(){
    struct RestoreFixtureState {
        NativeJoinClock clock=native_join_clock;bool dispatch=canonical_join_dispatch;unsigned csr=_mm_getcsr();
        ~RestoreFixtureState(){native_join_clock=clock;canonical_join_dispatch=dispatch;_mm_setcsr(csr);}
    }restore_fixture_state;
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));
    if(!base)return false;
    struct Release{uint8_t* base;~Release(){VirtualFree(base,0,MEM_RELEASE);}}release{base};
    for(const auto page:{uint32_t(0),uint32_t(0x82002000),uint32_t(0x8208C000),uint32_t(0x833D0000),uint32_t(0x833FC000)}){
        const size_t bytes=page?4096:65536;
        if(!VirtualAlloc(base+page,bytes,MEM_COMMIT,PAGE_READWRITE))return false;
    }
    PPC_STORE_U32(0x82002060,0);
    PPC_STORE_U64(0x833D04D8,1000000);
    PPC_STORE_U32(0x833D04E0,std::bit_cast<uint32_t>(1e-6f));
    std::memcpy(base+0x8208CB20,"JoinPadOne",11);
    std::memcpy(base+0x8208CB2C,"JoinPadTwo",11);
    std::memcpy(base+0x3100,"JoinPadTwo",11);
    // Actual retail resolver: both its interned pointer and equal string bytes
    // select device 1, without reading controller/action caches.
    for(const auto name:{uint32_t(0x8208CB2C),uint32_t(0x3100),uint32_t(0x8208CB20)}){
        PPCContext ctx{};ctx.r1.u64=0xF000;ctx.r4.u64=0x3000;ctx.fpscr.InitHost();PPC_STORE_U32(0x3000,name);
        __imp__sub_827C40A8(ctx,base);
        if(ctx.r3.u32!=(name==0x8208CB20?0:1))return false;
    }
    const auto initial_csr=_mm_getcsr();
    std::vector<uint8_t> before(65536),expected_memory(65536);
    for(const auto wall_now:{uint64_t(100000000),uint64_t(800000000)})
    for(const bool dispatch:{false,true})for(const bool caller:{false,true})for(const bool address:{false,true}){
        native_join_clock={};native_join_clock.Begin(0x2000,100);native_join_clock.Begin(0x2000,130);
        canonical_join_dispatch=dispatch;
        PPC_STORE_U64(0x833FCA60,wall_now);
        PPC_STORE_U64(0x2000+696,wall_now-(wall_now==100000000?7000000:19000000));
        PPC_STORE_U64(0x2000+704,wall_now-11000000);
        PPCContext ctx{};ctx.r1.u64=0xF000;ctx.r3.u64=0x2000+(address?696:704);ctx.lr=caller?0x8276EFC8:0x8276EFCC;ctx.fpscr.InitHost();
        const auto input_csr=_mm_getcsr();auto expected=ctx;
        std::memcpy(before.data(),base,before.size());
        __imp__sub_831C13F0(expected,base);const auto expected_csr=_mm_getcsr();
        std::memcpy(expected_memory.data(),base,expected_memory.size());
        if(dispatch && caller && address)expected.f1.f64=1.0;
        std::memcpy(base,before.data(),before.size());_mm_setcsr(input_csr);
        JoinElapsed(ctx,base,__imp__sub_831C13F0);
        if(std::memcmp(&ctx,&expected,sizeof(ctx)) || std::memcmp(base,expected_memory.data(),expected_memory.size()) || _mm_getcsr()!=expected_csr)return false;
    }
    // Real positive-delay reset: retain the native future timestamp, stack
    // writes and all scratch registers/FP effects; only track its agreed epoch.
    for(const double delay:{0.0,0.5}){
        canonical_join_dispatch=true;native_join_clock={};native_join_clock.Begin(0x2000,100);
        PPC_STORE_U64(0x833FCA60,100000000);
        PPCContext ctx{};ctx.r1.u64=0xF000;ctx.r3.u64=0x2000+696;ctx.lr=0x8276F070;ctx.f1.f64=delay;ctx.fpscr.InitHost();
        const auto input_csr=_mm_getcsr();auto expected=ctx;std::memcpy(before.data(),base,before.size());
        __imp__sub_831C1360(expected,base);const auto expected_csr=_mm_getcsr();std::memcpy(expected_memory.data(),base,expected_memory.size());
        std::memcpy(base,before.data(),before.size());_mm_setcsr(input_csr);
        JoinReset(ctx,base,__imp__sub_831C1360);
        if(std::memcmp(&ctx,&expected,sizeof(ctx)) || std::memcmp(base,expected_memory.data(),expected_memory.size()) || _mm_getcsr()!=expected_csr || native_join_clock.Elapsed()!=-delay)return false;
    }
    _mm_setcsr(initial_csr);
    return true;
}
void DispatchCanonicalJoin(PPCContext& ctx,uint8_t* base,uint32_t manager,PPCFunc* handler,uint64_t tick=0,bool trace_allowed=false){
    if(ctx.r1.u32<1024)return;
    struct Restore {PPCContext& ctx;std::array<uint8_t,sizeof(PPCContext)> saved;bool prior=canonical_join_dispatch;unsigned csr=_mm_getcsr();DWORD error=GetLastError();
        explicit Restore(PPCContext& value):ctx(value){std::memcpy(saved.data(),&ctx,sizeof(ctx));}
        ~Restore(){std::memcpy(&ctx,saved.data(),sizeof(ctx));canonical_join_dispatch=prior;_mm_setcsr(csr);SetLastError(error);}}restore{ctx};
    const auto stack=ctx.r1.u32-512,event=stack+96,argument=stack+100;
    PPC_STORE_U32(stack,ctx.r1.u32);PPC_STORE_U32(event,0x8211F6B4);PPC_STORE_U32(argument,0x8208CB2C);
    const bool trace=trace_allowed && REXCVAR_GET(em2_trace_players);
    const auto counter=trace?PPC_LOAD_U32(manager+684):0;
    const auto mode=trace?PPC_LOAD_U32(manager+352):0;
    const auto clock=trace?PPC_LOAD_U64(0x833FCA60):0;
    const auto start=trace?PPC_LOAD_U64(manager+696):0;
    canonical_join_dispatch=true;ctx.r1.u64=stack;ctx.r3.u64=manager;ctx.r4.u64=event;ctx.r5.u64=argument;
    handler(ctx,base);
    if(trace)REXLOG_INFO("Native synchronized join: tick {}, result {}, mode {}->{}, retry {}->{}, threshold {:08X}, clock {}, timer {}",tick,ctx.r3.u32,mode,PPC_LOAD_U32(manager+352),counter,PPC_LOAD_U32(manager+684),PPC_LOAD_U32(manager+688),clock,start);
}
}
PPC_FUNC_IMPL(sub_831C13F0){JoinElapsed(ctx,base,__imp__sub_831C13F0);}
PPC_FUNC_IMPL(sub_831C1360){JoinReset(ctx,base,__imp__sub_831C1360);}
void em2::ResetNativeSynchronizedJoinTiming(){native_join_clock={};native_join_lease={};}
namespace {
bool LeaseMatches(const NativeJoinLease& lease,uint64_t generation,uint64_t published_tick,uint64_t agreed_tick,DWORD thread){
    return lease.generation && lease.generation==generation && lease.tick==published_tick && lease.tick==agreed_tick && lease.thread==thread;
}
bool ExplicitJoinOwnsCurrentTick(){
    uint64_t tick=0,generation=0;em2::net::Frame frame;
    return native_join_lease.generation && em2::CurrentAgreedSimulationTick(tick) &&
        em2::ReadPublishedSynchronizedControls(generation,frame) && LeaseMatches(native_join_lease,generation,frame.tick,tick,GetCurrentThreadId());
}
em2::NativeJoinResult JoinTickAdmission(const em2::NativeJoinTick& request,uint64_t generation,const em2::net::Frame& published,uint64_t agreed_tick){
    if(!request.joining || !(request.raw_retail.players[1].buttons&0x10))return em2::NativeJoinResult::Inactive;
    if(!request.owner_generation || request.owner_generation!=generation || request.raw_retail!=published || request.raw_retail.tick!=agreed_tick)
        return em2::NativeJoinResult::Unpublished;
    return em2::NativeJoinResult::Dispatched;
}
}
em2::NativeJoinResult em2::RunNativeSynchronizedJoin(PPCContext& ctx,uint8_t* base,const NativeJoinTick& request){
    const auto csr=_mm_getcsr();const auto error=GetLastError();
    struct Environment {unsigned csr;DWORD error;~Environment(){_mm_setcsr(csr);SetLastError(error);}}environment{csr,error};
    if(!request.joining || !(request.raw_retail.players[1].buttons&0x10))return NativeJoinResult::Inactive;
    if(canonical_join_dispatch)return NativeJoinResult::Nested;
    uint64_t agreed_tick=0,generation=0;net::Frame published;
    if(!CurrentAgreedSimulationTick(agreed_tick))return NativeJoinResult::OutsideAgreedTick;
    if(!ReadPublishedSynchronizedControls(generation,published))return NativeJoinResult::Unpublished;
    const auto admission=JoinTickAdmission(request,generation,published,agreed_tick);
    if(admission!=NativeJoinResult::Dispatched)return admission;
    if(!base || ctx.r1.u32<1024 || !NativeOnlineJoinAllowed(base))return NativeJoinResult::NotReady;
    const auto manager=PPC_LOAD_U32(0x833DAB60);
    native_join_clock.Begin(manager,agreed_tick,generation);
    if(!native_join_clock.ClaimDispatch())return NativeJoinResult::Duplicate;
    native_join_lease={generation,agreed_tick,GetCurrentThreadId()};
    DispatchCanonicalJoin(ctx,base,manager,sub_8276EF50,agreed_tick,true);
    return NativeJoinResult::Dispatched;
}
void em2::RunNativeSynchronizedJoin(PPCContext& ctx,uint8_t* base,uint16_t buttons){
    if(!(buttons&0x10) || net::NativeOnlineStatus().state!=net::NativeSessionState::Joining)return;
    NativeJoinTick request;request.joining=true;
    if(!ReadPublishedSynchronizedControls(request.owner_generation,request.raw_retail) ||
       request.raw_retail.tick!=net::GameplayInputGate().committed_tick() || request.raw_retail.players[1].buttons!=buttons)return;
    RunNativeSynchronizedJoin(ctx,base,request);
}
bool em2::TestCanonicalNativeJoin(){
    const auto fail=[](unsigned line){REXLOG_ERROR("Canonical native join fixture failed at players.cpp:{}",line);return false;};
    const auto saved_clock=native_join_clock;
    struct RestoreClock{NativeJoinClock saved;NativeJoinLease lease=native_join_lease;bool dispatch=canonical_join_dispatch;unsigned csr=_mm_getcsr();DWORD error=GetLastError();
        ~RestoreClock(){native_join_clock=saved;native_join_lease=lease;canonical_join_dispatch=dispatch;_mm_setcsr(csr);SetLastError(error);}}restore_clock{saved_clock};
    if(!TestRetailJoinHelpers())return fail(__LINE__);
    // Real published input proof is read-only and bound to Begin's thread.
    // This headless fixture owns a fresh controls generation, never a Session.
    if(!BeginSynchronizedControls(730))return fail(__LINE__);
    struct ControlsScope {~ControlsScope(){EndSynchronizedControls();}}controls_scope;
    uint64_t generation=99;net::Frame published;published.tick=99;
    if(ReadPublishedSynchronizedControls(generation,published) || generation!=99 || published.tick!=99)return fail(__LINE__);
    net::Frame frame;frame.tick=730;frame.players[0].left_x=-123;frame.players[1].buttons=0x10;frame.players[1].right_y=321;
    if(!PublishSynchronizedControls(frame) || !ReadPublishedSynchronizedControls(generation,published) || !generation || published!=frame)return fail(__LINE__);
    bool wrong_thread_read=true,wrong_thread_publish=true;
    std::thread other([&]{uint64_t epoch=0;net::Frame out;wrong_thread_read=ReadPublishedSynchronizedControls(epoch,out);
        auto next=frame;++next.tick;next.players[0].left_x=999;wrong_thread_publish=PublishSynchronizedControls(next);});other.join();
    uint64_t retained_generation=0;net::Frame retained;
    if(wrong_thread_read || wrong_thread_publish || !ReadPublishedSynchronizedControls(retained_generation,retained) ||
       retained_generation!=generation || retained!=frame)return fail(__LINE__);
    NativeJoinTick request{generation,frame,true};
    if(JoinTickAdmission(request,generation,frame,730)!=NativeJoinResult::Dispatched)return fail(__LINE__);
    auto bad=request;bad.owner_generation=0;
    if(JoinTickAdmission(bad,generation,frame,730)!=NativeJoinResult::Unpublished)return fail(__LINE__);
    bad=request;++bad.owner_generation;
    if(JoinTickAdmission(bad,generation,frame,730)!=NativeJoinResult::Unpublished)return fail(__LINE__);
    bad=request;++bad.raw_retail.players[0].left_x;
    if(JoinTickAdmission(bad,generation,frame,730)!=NativeJoinResult::Unpublished ||
       JoinTickAdmission(request,generation,frame,731)!=NativeJoinResult::Unpublished)return fail(__LINE__);
    uint64_t unchanged_tick=0xDEADBEEF;
    if(CurrentAgreedSimulationTick(unchanged_tick) || unchanged_tick!=0xDEADBEEF)return fail(__LINE__);
    PPCContext admission_ctx{};admission_ctx.r1.u64=0xF000;admission_ctx.fpscr.InitHost();
    std::array<uint8_t,sizeof(PPCContext)> admission_before;std::memcpy(admission_before.data(),&admission_ctx,sizeof(admission_ctx));
    if(RunNativeSynchronizedJoin(admission_ctx,nullptr,request)!=NativeJoinResult::OutsideAgreedTick ||
       std::memcmp(&admission_ctx,admission_before.data(),sizeof(admission_ctx)))return fail(__LINE__);
    canonical_join_dispatch=true;
    const auto nested=RunNativeSynchronizedJoin(admission_ctx,nullptr,request);canonical_join_dispatch=false;
    if(nested!=NativeJoinResult::Nested)return fail(__LINE__);
    native_join_lease={generation,730,GetCurrentThreadId()};
    const auto owned=LeaseMatches(native_join_lease,generation,730,730,GetCurrentThreadId());
    if(!owned || !SuppressOrdinaryJoin(false,false,owned) || SuppressOrdinaryJoin(true,false,owned) ||
       LeaseMatches(native_join_lease,generation,730,731,GetCurrentThreadId()) ||
       LeaseMatches(native_join_lease,generation+1,730,730,GetCurrentThreadId()) ||
       LeaseMatches(native_join_lease,generation,730,730,GetCurrentThreadId()+1))return fail(__LINE__);
    ResetNativeSynchronizedJoinTiming();if(native_join_lease.generation || SuppressOrdinaryJoin(false,false,ExplicitJoinOwnsCurrentTick()))return fail(__LINE__);
    NativeJoinClock claims;claims.Begin(0x2000,730,generation);
    if(!claims.ClaimDispatch() || claims.ClaimDispatch())return fail(__LINE__);
    claims.Reset(.5);claims.Begin(0x2000,731,generation);
    if(!claims.ClaimDispatch() || !(claims.Elapsed()<0))return fail(__LINE__);
    claims.Begin(0x2000,731,generation+1);
    if(!claims.ClaimDispatch() || claims.Elapsed()!=0)return fail(__LINE__);
    // Retire and reacquire the same controls API: an old owner's exact frame
    // must fail even if manager and agreed tick would otherwise be identical.
    EndSynchronizedControls();if(!BeginSynchronizedControls(730) || !PublishSynchronizedControls(frame))return fail(__LINE__);
    uint64_t replacement=0;if(!ReadPublishedSynchronizedControls(replacement,published) || replacement==generation ||
       JoinTickAdmission(request,replacement,published,730)!=NativeJoinResult::Unpublished)return fail(__LINE__);

    // Check the actual native reset's future-delay semantics at tick boundaries,
    // including manager replacement and timeline restart.
    NativeJoinClock a;a.Begin(0x2000,40);if(a.Elapsed()!=0)return fail(__LINE__);
    a.Begin(0x2000,55);a.Reset(0.5);if(a.Elapsed()!=-0.5)return fail(__LINE__);
    a.Begin(0x2000,69);if(!(a.Elapsed()<0))return fail(__LINE__);
    a.Begin(0x2000,70);if(a.Elapsed()!=0)return fail(__LINE__);
    a.Begin(0x2000,71);if(!(a.Elapsed()>0))return fail(__LINE__);
    a.Begin(0x4000,1040);if(a.Elapsed()!=0)return fail(__LINE__);
    a.Begin(0x4000,0);if(a.Elapsed()!=0)return fail(__LINE__);
    a.Reset(-1);if(a.Elapsed()!=0)return fail(__LINE__);
    std::array<uint8_t,65536> memory{};auto* base=memory.data();
    PPCContext ctx{};ctx.r1.u64=0xF000;ctx.r3.u64=0xABCDEF;ctx.r31.u64=0x99887766;ctx.lr=0x12345678;ctx.fpscr.InitHost();
    const auto saved=ctx;const auto csr=_mm_getcsr();
    const auto handler=+[](PPCContext& call,uint8_t* base){
        const bool ok=canonical_join_dispatch && call.r3.u32==0x2000 && PPC_LOAD_U32(call.r1.u32)==0xF000 &&
            PPC_LOAD_U32(call.r4.u32)==0x8211F6B4 && PPC_LOAD_U32(call.r5.u32)==0x8208CB2C;
        PPC_STORE_U32(0x2000,ok?1:0);call.r31.u64=0xBADC0DE;call.r3.u64=42;_mm_setcsr(_mm_getcsr()|0x20);
    };
    native_join_clock={};native_join_clock.Begin(0x2000,4);native_join_clock.Begin(0x2000,19);
    canonical_join_dispatch=true;
    const auto timer=+[](PPCContext& call,uint8_t* base){call.r3.u64=0x1234;call.f1.f64=double(PPC_LOAD_U32(0x104));PPC_STORE_U32(0x100,0x11223344);};
    for(const uint32_t local_elapsed:{123u,9123u})for(const bool dispatch:{false,true})for(const bool caller:{false,true})for(const bool address:{false,true}){
        PPC_STORE_U32(0x104,local_elapsed);
        ctx=saved;ctx.lr=caller?0x8276EFC8:0x8276EFCC;ctx.r3.u64=address?0x2000+696:0x2000+700;
        auto expected=ctx;timer(expected,base);canonical_join_dispatch=dispatch;
        JoinElapsed(ctx,base,timer);if(dispatch && caller && address)expected.f1.f64=0.5;
        if(std::memcmp(&ctx,&expected,sizeof(ctx)) || PPC_LOAD_U32(0x100)!=0x11223344)return fail(__LINE__);
    }
    ctx=saved;ctx.lr=0x8276F070;ctx.r3.u64=0x2000+696;ctx.f1.f64=0.25;canonical_join_dispatch=true;
    JoinReset(ctx,base,timer);if(native_join_clock.Elapsed()!=-0.25)return fail(__LINE__);
    native_join_clock.Begin(0x2000,49);if(native_join_clock.Elapsed()!=0.75)return fail(__LINE__);
    ResetNativeSynchronizedJoinTiming();if(native_join_clock.initialized)return fail(__LINE__);
    ctx=saved;canonical_join_dispatch=restore_clock.dispatch;
    DispatchCanonicalJoin(ctx,base,0x2000,handler);
    if(!PPC_LOAD_U32(0x2000) || canonical_join_dispatch || std::memcmp(&ctx,&saved,sizeof(ctx)) || _mm_getcsr()!=csr)return fail(__LINE__);
    RunNativeSynchronizedJoin(ctx,nullptr,0x10);
    if(std::memcmp(&ctx,&saved,sizeof(ctx)) || _mm_getcsr()!=csr)return fail(__LINE__);
    constexpr auto padding_begin=offsetof(PPCContext,vscr_sat)+sizeof(ctx.vscr_sat),padding_end=offsetof(PPCContext,f0);
    static_assert(padding_end>padding_begin);
    std::memset(reinterpret_cast<uint8_t*>(&ctx)+padding_begin,0xA7,padding_end-padding_begin);
    std::array<uint8_t,sizeof(PPCContext)> raw_before;std::memcpy(raw_before.data(),&ctx,sizeof(ctx));
    SetLastError(0xE230);const auto before_error=GetLastError();bool threw=false;
    const auto throwing=+[](PPCContext& call,uint8_t*){call.r1.u64=0x1234;call.r5.u64=0x5566;SetLastError(42);_mm_setcsr(_mm_getcsr()|0x20);throw std::runtime_error("join fixture unwind");};
    try{DispatchCanonicalJoin(ctx,base,0x2000,throwing);}catch(const std::runtime_error&){threw=true;}
    if(!threw || canonical_join_dispatch || std::memcmp(&ctx,raw_before.data(),sizeof(ctx)) || _mm_getcsr()!=csr || GetLastError()!=before_error)return fail(__LINE__);
    REXLOG_INFO("Canonical native join passed published-frame owner/thread/generation and agreed-tick admission, duplicate/nested isolation, clock/reset boundaries, distinct local elapsed results and exact caller/scope isolation, borrowed stack/fixed-string arguments, complete caller/FP restoration and inactive tick rejection; native campaign acceptance separate");return true;
}

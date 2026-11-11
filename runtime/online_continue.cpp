#include "online_continue.h"
#include "online_runtime.h"
#include "online_native_handoff.h"
#include "online_native_menu.h"
#include "controls.h"
#include "demo_setup.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/input/input_driver.h>
#include <mutex>
#include <thread>
#include <cstring>
#include <windows.h>
#include <array>
#include <atomic>

PPC_EXTERN_FUNC(__imp__sub_82A41AD0);
PPC_EXTERN_FUNC(__imp__sub_82A41B38);
PPC_EXTERN_FUNC(__imp__sub_82A41B78);
PPC_EXTERN_FUNC(__imp__sub_82A41A80);
PPC_EXTERN_FUNC(__imp__sub_82A41A30);
PPC_EXTERN_FUNC(sub_82A41A80);
PPC_EXTERN_FUNC(__imp__sub_82B1B1B0);
PPC_EXTERN_FUNC(sub_831BB758);
PPC_EXTERN_FUNC(__imp__sub_82A40190);
PPC_EXTERN_FUNC(__imp__sub_82A41260);
PPC_EXTERN_FUNC(__imp__sub_828340F0);
PPC_EXTERN_FUNC(__imp__sub_8295FC58);
REXCVAR_DECLARE(bool,em2_trace_online_observations);
REXCVAR_DECLARE(bool,em2_demo_em1);
REXCVAR_DEFINE_BOOL(em2_online_auto_continue,true,"Online","Select the session local controller and Continue the transferred checkpoint through native menus");
REXCVAR_DEFINE_BOOL(em2_auto_continue,false,"Debug","Select the native local profile and Continue an existing checkpoint once, without movement or camera input");
REXCVAR_DEFINE_BOOL(em2_trace_auto_continue,false,"Debug","Trace the automatic offline checkpoint startup through native menus");

namespace {
struct OfflineStartup {bool profile_claimed=false,profile_requested=false,notice_requested=false,continue_claimed=false,done=false;};
OfflineStartup offline_startup;
std::mutex offline_startup_mutex;
bool OfflineStartupEnabled(){return (REXCVAR_GET(em2_auto_continue) || em2::Em1DemoSession() || em2::net::NativeOnlineMenuQaActive()) && !em2::net::SynchronizedSessionActive();}
bool StartupEnabled(){return OfflineStartupEnabled() || (REXCVAR_GET(em2_online_auto_continue) && em2::net::SynchronizedSessionActive());}
bool StartupTraceEnabled(){return (REXCVAR_GET(em2_trace_auto_continue) && OfflineStartupEnabled()) || (REXCVAR_GET(em2_trace_online_observations) && em2::net::SynchronizedSessionActive());}
em2::net::NativeSessionStatus StartupStatus(){
    auto status=em2::net::NativeOnlineStatus();
    if(status.state!=em2::net::NativeSessionState::None || !OfflineStartupEnabled())return status;
    std::lock_guard lock(offline_startup_mutex);
    if(!offline_startup.done){status.state=em2::net::NativeSessionState::WaitingForWorld;status.profile_requested=offline_startup.profile_requested;status.save_notice_requested=offline_startup.notice_requested;}
    return status;
}
bool ClaimStartupProfile(unsigned& device,bool& offline){
    offline=OfflineStartupEnabled();
    if(!offline)return em2::net::ClaimNativeOnlineProfile(device);
    std::lock_guard lock(offline_startup_mutex);
    if(offline_startup.done || offline_startup.profile_claimed)return false;
    offline_startup.profile_claimed=true;device=0;return true;
}
void FinishStartupProfile(bool accepted,bool offline){
    if(!offline){em2::net::FinishNativeOnlineProfile(accepted);return;}
    std::lock_guard lock(offline_startup_mutex);
    if(accepted)offline_startup.profile_requested=true;else offline_startup.profile_claimed=false;
}
bool ClaimStartupContinue(bool& offline){
    offline=OfflineStartupEnabled();
    // Native lobby QA selects the real local profile but must reach the actual
    // authored online menu, not let ordinary offline auto-Continue skip it.
    if(offline && em2::net::NativeOnlineMenuQaActive())return false;
    if(!offline)return em2::net::ClaimNativeOnlineContinue();
    std::lock_guard lock(offline_startup_mutex);
    if(offline_startup.done || offline_startup.continue_claimed)return false;
    offline_startup.continue_claimed=true;return true;
}
void FinishStartupContinue(bool accepted,bool offline){
    if(!offline){em2::net::FinishNativeOnlineContinue(accepted);return;}
    std::lock_guard lock(offline_startup_mutex);
    if(accepted)offline_startup.done=true;else offline_startup.continue_claimed=false;
}
struct ResumeBinding {uint32_t owner=0,cookie=0;};
ResumeBinding binding;
std::mutex binding_mutex;
std::atomic<uint32_t> trace_bits{0};
void Trace(unsigned bit,const char* message,uint32_t value=0,uint32_t extra=0){
    if(!StartupTraceEnabled() ||
        (trace_bits.fetch_or(1u<<bit,std::memory_order_relaxed)&(1u<<bit)))return;
    const auto csr=_mm_getcsr();
    REXLOG_INFO("Native online menu trace: {} {:08X} {:08X}",message,value,extra);_mm_setcsr(csr);
}
void Bind(uint32_t owner,uint8_t* base) {
    std::lock_guard lock(binding_mutex);
    binding={owner,owner?PPC_LOAD_U32(owner):0};
    Trace(0,"ResumeGame/StartNewGame registered",owner,owner?PPC_LOAD_U32(owner+12):0);
}
void Unbind() {std::lock_guard lock(binding_mutex);binding={};}
// Called only on the live base-menu update, before its original dispatch. No
// object is retained across transitions and no hardware/button state is forged.
void AutoSelectProfile(PPCContext& ctx,uint8_t* base,PPCFunc* handler=__imp__sub_82A41260){
    using namespace em2::net;
    if(!StartupEnabled() || StartupStatus().state!=NativeSessionState::WaitingForWorld)return;
    const auto owner=ctx.r3.u32;
    if(!owner || PPC_LOAD_U32(owner)!=0x8211F700 || PPC_LOAD_U32(owner+4)!=1 ||
       PPC_LOAD_U32(owner+12)!=UINT32_MAX || PPC_LOAD_U32(owner+16)!=UINT32_MAX || ctx.r1.u32<1024)return;
    const auto ui=PPC_LOAD_U32(0x833E1DEC);
    if(!ui || PPC_LOAD_U8(ui+22) || PPC_LOAD_U8(ui+23))return;
    if(StartupTraceEnabled()){
        const auto dispatcher=PPC_LOAD_U32(0x833DCB9C),movie=dispatcher?PPC_LOAD_U32(dispatcher+4):0;
        Trace(7,"Profile selection Flash readiness",movie,movie?PPC_LOAD_U32(movie+156):0);
    }
    unsigned device;
    bool offline=false;if(!ClaimStartupProfile(device,offline))return;
    struct Claim {bool offline,accepted=false;~Claim(){FinishStartupProfile(accepted,offline);}}claim{offline};
    // The public native session currently supports local XInput slots 0 and 1.
    // Both values use the original JoinPad names recognized by 827C40A8.
    if(device>=2)return;
    struct Restore {PPCContext& ctx;PPCContext saved;unsigned csr=_mm_getcsr();~Restore(){ctx=saved;_mm_setcsr(csr);}}restore{ctx,ctx};
    const auto stack=ctx.r1.u32-512,event=stack+96,argument=stack+100;
    PPC_STORE_U32(stack,ctx.r1.u32);
    PPC_STORE_U32(event,0x8211F6B4); // Original NiFixedString: Menu
    PPC_STORE_U32(argument,device?0x8208CB2C:0x8208CB20); // JoinPadTwo / JoinPadOne
    ctx.r1.u64=stack;ctx.r3.u64=owner;ctx.r4.u64=event;ctx.r5.u64=argument;
    handler(ctx,base);
    claim.accepted=ctx.r3.u32==1 && PPC_LOAD_U32(owner+16)==device;
    if(claim.accepted){
        if(!offline)em2::SetOnlineJoinControls(true,false);
        REXLOG_INFO("{} startup selected native local controller {} for checkpoint profile loading",offline?"Offline":"Online",device);
    }
}
void RegisterSaveNoticeBridge(PPCContext& ctx,uint8_t* base,PPCFunc* registrar=__imp__sub_8295FC58){
    if(!StartupEnabled())return;
    const auto status=StartupStatus();
    if(status.state!=em2::net::NativeSessionState::WaitingForWorld || !status.profile_requested || status.save_notice_requested)return;
    const auto owner=ctx.r3.u32;
    if(!owner || PPC_LOAD_U32(owner)!=0x82120150 || PPC_LOAD_U32(owner+4)>1 || PPC_LOAD_U32(owner+16)!=0 || ctx.r1.u32<1024)return;
    const auto ui=PPC_LOAD_U32(0x833E1DEC),dispatcher=PPC_LOAD_U32(0x833DCB9C);
    const auto movie=dispatcher?PPC_LOAD_U32(dispatcher+4):0;
    if(!ui || PPC_LOAD_U8(ui+22) || PPC_LOAD_U8(ui+23) || !movie || !PPC_LOAD_U32(movie+156))return;
    // Same callback/name/owner/flags as the retail Options interface registration
    // at 82B0D8A8. Bind before its first notice update, while this movie is live.
    struct Restore {PPCContext& ctx;PPCContext saved;unsigned csr=_mm_getcsr();~Restore(){ctx=saved;_mm_setcsr(csr);}}restore{ctx,ctx};
    const auto stack=ctx.r1.u32-512;PPC_STORE_U32(stack,ctx.r1.u32);ctx.r1.u64=stack;
    ctx.r3.u64=0x82B0D118;ctx.r4.u64=0x82154340;ctx.r5.u64=0;ctx.r6.u64=1;
    registrar(ctx,base);
    Trace(9,"Native startup SetOption bridge registered");
}
void AutoContinue(PPCContext& ctx,uint8_t* base,uint32_t invocation) {
    using namespace em2::net;
    if(!StartupEnabled())return;
    bool offline=false;if(!ClaimStartupContinue(offline)){Trace(1,"Continue ownership not available",unsigned(StartupStatus().state));return;}
    // Claim before following any retained owner: after an accepted request the
    // native state may already have exited when another menu query arrives.
    struct Claim {bool offline,accepted=false;~Claim(){FinishStartupContinue(accepted,offline);}}claim{offline};
    const auto result=PPC_LOAD_U32(invocation+8);
    if(!result || PPC_LOAD_U32(result+4)!=2)return;
    const bool saved=PPC_LOAD_U8(result+8)!=0;
    if(!saved && !em2::Em1DemoSession()){Trace(2,"Native save query not valid",result,result?PPC_LOAD_U32(result+4):0);return;}
    Trace(3,"Native save query valid");
    // Exit hooks acquire this lock before unregistering/freeing the owner.
    // Resume itself only reads invocation+12 and changes that owner's state.
    std::lock_guard lock(binding_mutex);
    if(!binding.owner){Trace(4,"No active ResumeGame menu binding");return;}
    if(PPC_LOAD_U32(binding.owner)!=binding.cookie){binding={};return;}
    const auto state=PPC_LOAD_U32(binding.owner+12);
    if(state==0 || state==1 || state==3){Trace(5,"ResumeGame state defers request",state);return;}
    struct Restore {PPCContext& ctx;PPCContext saved;unsigned csr=_mm_getcsr();~Restore(){ctx=saved;_mm_setcsr(csr);}}restore{ctx,ctx};
    const auto stack=ctx.r1.u32-256,arguments=stack+96;
    PPC_STORE_U32(stack,ctx.r1.u32);std::memset(base+arguments,0,16);
    PPC_STORE_U32(arguments+12,binding.owner);
    ctx.r1.u64=stack;ctx.r3.u64=arguments;
    if(saved)__imp__sub_82A41A80(ctx,base);else __imp__sub_82A41A30(ctx,base);
    claim.accepted=PPC_LOAD_U32(binding.owner+12)==(saved?1u:0u);
    if(claim.accepted)REXLOG_INFO("{} startup requested native {}",offline?"Offline":"Online",saved?"Continue for the existing checkpoint":"New Game for the isolated EM1 preview profile");
}
void ProbeBoundContinue(PPCContext& ctx,uint8_t* base){
    if(!StartupEnabled() || StartupStatus().state!=em2::net::NativeSessionState::WaitingForWorld || ctx.r1.u32<2048)return;
    // TopMenu queries can finish before FrontMenu registers ResumeGame. Repeat
    // that same read-only retail query after registration, using a private
    // native invocation/result. AutoContinue retains its claim and state guards.
    struct Restore {PPCContext& ctx;PPCContext saved;unsigned csr=_mm_getcsr();~Restore(){ctx=saved;_mm_setcsr(csr);}}restore{ctx,ctx};
    const auto stack=ctx.r1.u32-512,arguments=stack+96,result=stack+128;
    PPC_STORE_U32(stack,ctx.r1.u32);std::memset(base+arguments,0,48);PPC_STORE_U32(arguments+8,result);
    ctx.r1.u64=stack;ctx.r3.u64=arguments;
    __imp__sub_82B1B1B0(ctx,base);AutoContinue(ctx,base,arguments);
}
void RegisterFrontMenuOptions(PPCContext& ctx,uint8_t* base,uint32_t owner){
    // A normal offline title menu never enters Options and may never run the
    // auto-start profile notice bridge. Bind the same audited SetOption native
    // callback alongside FrontMenu's own Resume/StartNewGame registrations.
    if(!owner || PPC_LOAD_U32(owner)!=0x8211FF18 || ctx.r1.u32<1024)return;
    const auto dispatcher=PPC_LOAD_U32(0x833DCB9C),movie=dispatcher?PPC_LOAD_U32(dispatcher+4):0;
    if(!movie || !PPC_LOAD_U32(movie+156))return;
    struct Restore{PPCContext& ctx;PPCContext saved;unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Restore(){ctx=saved;_mm_setcsr(csr);SetLastError(error);}}restore{ctx,ctx};
    const auto stack=ctx.r1.u32-512;PPC_STORE_U32(stack,ctx.r1.u32);ctx.r1.u64=stack;
    ctx.r3.u64=0x82B0D118;ctx.r4.u64=0x82154340;ctx.r5.u64=0;ctx.r6.u64=1;
    __imp__sub_8295FC58(ctx,base);
    REXLOG_INFO("Native FrontMenu online SetOption bridge registered");
}
}
bool em2::OfflineStartupContinueAccepted(){std::lock_guard lock(offline_startup_mutex);return offline_startup.done;}
bool em2::ClaimOfflineStartupSaveNotice(){
    if(!OfflineStartupEnabled())return false;
    std::lock_guard lock(offline_startup_mutex);
    if(offline_startup.done || !offline_startup.profile_requested || offline_startup.notice_requested)return false;
    offline_startup.notice_requested=true;return true;
}
bool em2::ClaimNativeLobbyQaFreshSaveNotice(){
    if((!em2::net::NativeOnlineMenuQaActive() && !em2::Em1DemoSession()) || !OfflineStartupEnabled())return false;
    std::lock_guard lock(offline_startup_mutex);
    if(offline_startup.done || !offline_startup.profile_requested || offline_startup.notice_requested)return false;
    offline_startup.notice_requested=true;
    REXLOG_INFO("Private native-lobby QA accepted the authored fresh-profile autosave notice");return true;
}
bool em2::NativeLobbyQaFreshSaveNoticeAccepted(){
    if((!em2::net::NativeOnlineMenuQaActive() && !em2::Em1DemoSession()) || !OfflineStartupEnabled())return false;
    std::lock_guard lock(offline_startup_mutex);
    return !offline_startup.done && offline_startup.profile_requested && offline_startup.notice_requested;
}
PPC_FUNC_IMPL(sub_828340F0){
    if(StartupTraceEnabled() && ctx.r4.u32==0x8211F204){
        const auto movie=ctx.r3.u32?PPC_LOAD_U32(ctx.r3.u32+4):0;
        Trace(8,"PressStartSelected Flash invocation",movie,movie?PPC_LOAD_U32(movie+156):0);
    }
    __imp__sub_828340F0(ctx,base);
}
PPC_FUNC_IMPL(sub_82A40190) {
    // Inspect audited live menu objects before original dispatch, including
    // profile selection and the Resume menu. Never follow an object after its
    // original update, which may transition away and destroy that state.
    const auto csr=_mm_getcsr();
    if(StartupTraceEnabled() && ctx.r3.u32){
        const auto table=PPC_LOAD_U32(ctx.r3.u32),phase=PPC_LOAD_U32(ctx.r3.u32+4);
        // Retail base-menu tables audited from image.bin (update slot +20).
        // Each phase is logged once per table, at most 36 records per process.
        // Do not read subclass fields on unknown/base-only state objects.
        constexpr std::array<uint32_t,12> tables={0x8211F0B0,0x8211F530,0x8211F700,0x8211F84C,0x8211F998,0x8211FAE4,0x8211FC30,0x8211FF18,0x82120150,0x8212029C,0x82120490,0x821205DC};
        static std::array<std::atomic<uint32_t>,tables.size()> seen{};
        if(phase<3)for(size_t i=0;i<tables.size();++i)if(table==tables[i]){
            const auto bit=1u<<phase;
            if(!(seen[i].fetch_or(bit,std::memory_order_relaxed)&bit))REXLOG_INFO("Native online menu phase: table {:08X}, owner {:08X}, phase {}",table,ctx.r3.u32,phase);
            break;
        }
        if(table==0x82120150 || table==0x8211F530){
            const auto ui=PPC_LOAD_U32(0x833E1DEC),dispatcher=PPC_LOAD_U32(0x833DCB9C);
            const auto movie=dispatcher?PPC_LOAD_U32(dispatcher+4):0;
            const std::array<uint32_t,4> snapshot={PPC_LOAD_U32(ctx.r3.u32+(table==0x82120150?16:12)),ui?PPC_LOAD_U32(ui+8):UINT32_MAX,movie?PPC_LOAD_U32(movie+156):0,ui?uint32_t(PPC_LOAD_U8(ui+22))|(uint32_t(PPC_LOAD_U8(ui+23))<<8):0};
            thread_local std::array<std::array<uint32_t,4>,2> previous={{{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX},{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX}}};
            thread_local unsigned count=0;const auto index=table==0x82120150?0:1;
            if(snapshot!=previous[index] && count<32){previous[index]=snapshot;++count;REXLOG_INFO("Native online menu detail: table {:08X}, state {}, UI {}, Flash {:08X}, busy {:04X}",table,snapshot[0],snapshot[1],snapshot[2],snapshot[3]);}
        }
        if(table==0x8211FF18){
            const auto request=PPC_LOAD_U32(ctx.r3.u32+12);
            thread_local uint64_t prior=UINT64_MAX;const auto key=(uint64_t(phase)<<32)|request;
            if(key!=prior){prior=key;REXLOG_INFO("Native online FrontMenu state: owner {:08X}, phase {}, request {}",ctx.r3.u32,phase,request);}
        }
    }
    _mm_setcsr(csr);em2::net::UpdateNativeOnlineHandoff(ctx,base);
    AutoSelectProfile(ctx,base);RegisterSaveNoticeBridge(ctx,base);__imp__sub_82A40190(ctx,base);
}
PPC_FUNC_IMPL(sub_82A41A80) {
    using namespace em2::net;
    const auto entry_csr=_mm_getcsr();
    if(!ClaimNativeOnlineContinue()){_mm_setcsr(entry_csr);__imp__sub_82A41A80(ctx,base);return;}
    struct Claim {
        bool accepted=false;unsigned csr;
        ~Claim(){FinishNativeOnlineContinue(accepted);_mm_setcsr(csr);}
    } claim{false,entry_csr};
    // The original handler resolves invocation+12 and requests native state1.
    // Follow only its current target, never a retired retained menu pointer.
    std::lock_guard lock(binding_mutex);
    const auto target=PPC_LOAD_U32(ctx.r3.u32+12);
    const bool bound=target && target==binding.owner && PPC_LOAD_U32(target)==binding.cookie;
    const auto before=bound?PPC_LOAD_U32(target+12):0;
    _mm_setcsr(entry_csr);__imp__sub_82A41A80(ctx,base);claim.csr=_mm_getcsr();
    claim.accepted=bound && before!=0 && before!=1 && before!=3 && PPC_LOAD_U32(target+12)==1;
    if(claim.accepted)REXLOG_INFO("Online startup accepted manual native Continue for the transferred checkpoint");
}
PPC_FUNC_IMPL(sub_82A41AD0) {
    const auto owner=ctx.r3.u32;__imp__sub_82A41AD0(ctx,base);Bind(owner,base);
    RegisterFrontMenuOptions(ctx,base,owner);ProbeBoundContinue(ctx,base);
}
PPC_FUNC_IMPL(sub_82A41B38) {Unbind();__imp__sub_82A41B38(ctx,base);}
PPC_FUNC_IMPL(sub_82A41B78) {Unbind();__imp__sub_82A41B78(ctx,base);}
PPC_FUNC_IMPL(sub_82B1B1B0) {
    Trace(6,"Native save query invoked",ctx.r3.u32);
    const auto invocation=ctx.r3.u32;__imp__sub_82B1B1B0(ctx,base);AutoContinue(ctx,base,invocation);
}

bool em2::TestOfflineStartup(){
    if(net::SynchronizedSessionActive())return false;
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));if(!base)return false;
    struct Restore {
        uint8_t* base;OfflineStartup startup;ResumeBinding resume;
        bool automatic=REXCVAR_GET(em2_auto_continue),online=REXCVAR_GET(em2_online_auto_continue),em1=REXCVAR_GET(em2_demo_em1);unsigned csr=_mm_getcsr();
        ~Restore(){offline_startup=startup;binding=resume;REXCVAR_SET(em2_auto_continue,automatic);REXCVAR_SET(em2_online_auto_continue,online);REXCVAR_SET(em2_demo_em1,em1);_mm_setcsr(csr);VirtualFree(base,0,MEM_RELEASE);}
    }restore{base,offline_startup,binding};
    if(!VirtualAlloc(base,0x10000,MEM_COMMIT,PAGE_READWRITE) || !VirtualAlloc(base+0x833D0000,0x20000,MEM_COMMIT,PAGE_READWRITE))return false;
    offline_startup={};binding={};
    constexpr uint32_t owner=0x6000,ui=0x7000,profile=0x2000,resume=0x8000,invocation=0x4000,result=0x5000;
    PPC_STORE_U32(owner,0x8211F700);PPC_STORE_U32(owner+4,1);PPC_STORE_U32(owner+12,UINT32_MAX);PPC_STORE_U32(owner+16,UINT32_MAX);PPC_STORE_U32(0x833E1DEC,ui);
    static unsigned profile_calls;profile_calls=0;
    const auto handler=+[](PPCContext& c,uint8_t* base){++profile_calls;PPC_STORE_U32(c.r3.u32+16,0);c.r3.u64=1;c.r31.u64=123;_mm_setcsr(_mm_getcsr()^_MM_FLUSH_ZERO_MASK);};
    PPCContext context{};context.r1.u64=0xF000;context.r3.u64=owner;context.lr=0x12345678;context.fpscr.InitHost();const auto saved=context;const auto csr=_mm_getcsr();
    REXCVAR_SET(em2_auto_continue,false);AutoSelectProfile(context,base,handler);if(profile_calls || ClaimOfflineStartupSaveNotice())return false;
    REXCVAR_SET(em2_auto_continue,true);REXCVAR_SET(em2_online_auto_continue,false);
    AutoSelectProfile(context,base,handler);
    if(profile_calls!=1 || !offline_startup.profile_requested || std::memcmp(&context,&saved,sizeof context) || _mm_getcsr()!=csr)return false;
    // Ordinary offline auto-Continue must not gain first-use/new-save consent
    // authorization merely because it has selected a real native profile.
    if(ClaimNativeLobbyQaFreshSaveNotice() || NativeLobbyQaFreshSaveNoticeAccepted())return false;
    if(!ClaimOfflineStartupSaveNotice() || ClaimOfflineStartupSaveNotice())return false;
    PPC_STORE_U32(owner+16,UINT32_MAX);AutoSelectProfile(context,base,handler);if(profile_calls!=1)return false;
    PPC_STORE_U32(0x833DAB54,profile);PPC_STORE_U32(profile+296,2);PPC_STORE_U8(profile+20,1);
    PPC_STORE_U32(resume,0x82112233);PPC_STORE_U32(invocation+8,result);PPC_STORE_U32(invocation+12,0x9000);Bind(resume,base);
    auto query=[&](uint8_t save_status,uint32_t state,bool expected){
        PPC_STORE_U8(profile+20+2*8,save_status);PPC_STORE_U32(resume+12,state);PPC_STORE_U32(result+4,0);
        PPCContext actual{};actual.r1.u64=0xF000;actual.r3.u64=invocation;actual.r31.u64=0xABCD;actual.lr=0x12345678;actual.fpscr.InitHost();
        auto reference=actual;__imp__sub_82B1B1B0(reference,base);const auto original_csr=_mm_getcsr();
        PPC_STORE_U32(result+4,0);sub_82B1B1B0(actual,base);
        return !std::memcmp(&actual,&reference,sizeof actual) && _mm_getcsr()==original_csr && PPC_LOAD_U32(resume+12)==(expected?1:state) && PPC_LOAD_U32(invocation+12)==0x9000;
    };
    if(!query(1,4,false) || !query(2,4,false))return false;
    for(uint32_t state:{0u,1u,3u})if(!query(0,state,false))return false;
    if(!query(0,4,true) || !offline_startup.done || !query(0,4,false) || ClaimOfflineStartupSaveNotice())return false;
    // Only the explicitly selected, isolated EM1 demo may start a fresh game.
    // Exercise its original StartNewGame callback and preserve the caller.
    offline_startup={};REXCVAR_SET(em2_demo_em1,true);REXCVAR_SET(em2_auto_continue,false);
    PPC_STORE_U32(resume+12,4);PPC_STORE_U32(resume+16,123);PPC_STORE_U32(result+4,0);
    AutoContinue(context,base,invocation);
    if(offline_startup.done || PPC_LOAD_U32(resume+12)!=4)return false;
    PPC_STORE_U32(result+4,2);PPC_STORE_U8(result+8,0);
    AutoContinue(context,base,invocation);
    if(!offline_startup.done || PPC_LOAD_U32(resume+12)!=0 || PPC_LOAD_U32(resume+16)!=0 ||
       std::memcmp(&context,&saved,sizeof context) || _mm_getcsr()!=csr || PPC_LOAD_U32(invocation+12)!=0x9000)return false;
    PPC_STORE_U32(resume+12,4);AutoContinue(context,base,invocation);
    if(PPC_LOAD_U32(resume+12)!=4)return false;
    REXCVAR_SET(em2_demo_em1,false);
    if(net::SynchronizedSessionActive() || net::NativeOnlineStatus().state!=net::NativeSessionState::None)return false;
    REXLOG_INFO("Offline startup passed: opt-out, independent offline profile selection, one-shot existing-save notice and native Continue, missing/invalid save rejection, deferred states, no repeated resume, full query/caller/FP preservation; no online session or movement/camera input");
    return true;
}
bool em2::net::TestNativeOnlineProfile(){
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));if(!base)return false;
    struct Restore {uint8_t* base;bool enabled=REXCVAR_GET(em2_online_auto_continue);unsigned csr=_mm_getcsr();~Restore(){REXCVAR_SET(em2_online_auto_continue,enabled);_mm_setcsr(csr);VirtualFree(base,0,MEM_RELEASE);}}restore{base};
    if(!VirtualAlloc(base,0x10000,MEM_COMMIT,PAGE_READWRITE) || !VirtualAlloc(base+0x833D0000,0x20000,MEM_COMMIT,PAGE_READWRITE))return false;
    constexpr uint32_t owner=0x6000,ui=0x7000;
    PPC_STORE_U32(owner,0x8211F700);PPC_STORE_U32(owner+4,1);
    PPC_STORE_U32(owner+12,UINT32_MAX);PPC_STORE_U32(owner+16,UINT32_MAX);
    PPC_STORE_U32(0x833E1DEC,ui);
    static unsigned calls=0;static bool valid=true;calls=0;valid=true;
    const auto handler=+[](PPCContext& ctx,uint8_t* base){
        ++calls;const auto selected=ctx.r3.u32;
        valid&=selected==0x6000 && PPC_LOAD_U32(ctx.r4.u32)==0x8211F6B4 && PPC_LOAD_U32(ctx.r5.u32)==0x8208CB20 && PPC_LOAD_U32(ctx.r1.u32)==0xF000;
        PPC_STORE_U32(selected+16,0);ctx.r3.u64=1;ctx.r31.u64=0xABCDEF;
        _mm_setcsr(_mm_getcsr()^_MM_FLUSH_ZERO_MASK);
    };
    PPCContext ctx{};ctx.r1.u64=0xF000;ctx.r3.u64=owner;ctx.lr=0x12345678;ctx.fpscr.InitHost();const auto csr=_mm_getcsr();
    auto check=[&](bool expect){const auto saved=ctx;const auto before=calls;AutoSelectProfile(ctx,base,handler);return calls==before+unsigned(expect) && valid && !std::memcmp(&ctx,&saved,sizeof(ctx)) && _mm_getcsr()==csr;};
    REXCVAR_SET(em2_online_auto_continue,false);if(!check(false))return false;REXCVAR_SET(em2_online_auto_continue,true);
    PPC_STORE_U32(owner,0x8211FF18);if(!check(false))return false;PPC_STORE_U32(owner,0x8211F700);
    PPC_STORE_U32(owner+4,0);if(!check(false))return false;PPC_STORE_U32(owner+4,1);
    for(const auto offset:{12u,16u}){PPC_STORE_U32(owner+offset,0);if(!check(false))return false;PPC_STORE_U32(owner+offset,UINT32_MAX);}
    for(const auto offset:{22u,23u}){PPC_STORE_U8(ui+offset,1);if(!check(false))return false;PPC_STORE_U8(ui+offset,0);}
    ctx.r1.u64=512;if(!check(false))return false;ctx.r1.u64=0xF000;
    PPC_STORE_U32(0x833E1DEC,0);if(!check(false))return false;PPC_STORE_U32(0x833E1DEC,ui);
    if(NativeOnlineStatus().state==NativeSessionState::WaitingForWorld){
        // Exercise the real retail handler's already-assigned rejection. Its
        // interned JoinPadOne path needs no native sign-in import in this case.
        PPC_STORE_U32(0x833DBAD8,0x2000);PPC_STORE_U32(0x833DC87C,0x8000);
        PPC_STORE_U32(0x8004,0);PPC_STORE_U32(0x8008,UINT32_MAX);
        const auto before=ctx;AutoSelectProfile(ctx,base);
        if(std::memcmp(&ctx,&before,sizeof(ctx)) || _mm_getcsr()!=csr || PPC_LOAD_U32(owner+16)!=UINT32_MAX || NativeOnlineStatus().profile_requested)return false;
    }
    if(!check(NativeOnlineStatus().state==NativeSessionState::WaitingForWorld))return false;
    if(!check(false))return false;
    if(calls && (!NativeOnlineStatus().profile_requested || NativeOnlineStatus().continue_requested))return false;
    if(calls){
        PPC_STORE_U32(owner,0x82120150);PPC_STORE_U32(owner+4,0);PPC_STORE_U32(owner+16,0);
        PPC_STORE_U32(0x833DCB9C,0x8000);PPC_STORE_U32(0x8004,0x9000);PPC_STORE_U32(0x9000+156,0xA000);
        static unsigned registrations=0;registrations=0;
        const auto registrar=+[](PPCContext& c,uint8_t* base){
            if(c.r3.u32==0x82B0D118 && c.r4.u32==0x82154340 && !c.r5.u32 && c.r6.u32==1 && PPC_LOAD_U32(c.r1.u32)==0xF000)++registrations;
            c.r31.u64=123;_mm_setcsr(_mm_getcsr()^_MM_FLUSH_ZERO_MASK);
        };
        const auto before=ctx;RegisterSaveNoticeBridge(ctx,base,registrar);
        if(registrations!=1 || std::memcmp(&ctx,&before,sizeof(ctx)) || _mm_getcsr()!=csr)return false;
        PPC_STORE_U32(owner+16,3);RegisterSaveNoticeBridge(ctx,base,registrar);if(registrations!=1)return false;
    }

    REXLOG_INFO("Native online profile bridge passed: opt-out, state/type/pending/busy guards, exact Menu/JoinPadOne event, full context/FP preservation and one accepted request; real native already-assigned rejection and synthetic acceptance callback; native handler acceptance requires paired runtime validation");return true;
}

bool em2::net::TestNativeOnlineContinue(bool manual) {
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));if(!base)return false;
    struct Restore {
        uint8_t* base;ResumeBinding previous;bool enabled=REXCVAR_GET(em2_online_auto_continue);unsigned csr=_mm_getcsr();
        ~Restore(){{std::lock_guard lock(binding_mutex);binding=previous;}REXCVAR_SET(em2_online_auto_continue,enabled);_mm_setcsr(csr);VirtualFree(base,0,MEM_RELEASE);}
    }restore{base,binding};
    if(!VirtualAlloc(base,0x10000,MEM_COMMIT,PAGE_READWRITE) || !VirtualAlloc(base+0x833D0000,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
    constexpr uint32_t profile=0x2000,invocation=0x4000,result=0x5000,owner=0x6000;
    PPC_STORE_U32(0x833DAB54,profile);PPC_STORE_U32(profile+296,2);
    PPC_STORE_U32(invocation+8,result);PPC_STORE_U32(invocation+12,0x7000);
    PPC_STORE_U32(owner,0x82112233);
    auto require=[](bool ok,const char* text){if(!ok)REXLOG_ERROR("Native online Continue test: {}",text);return ok;};
    unsigned cases=0;
    auto query=[&](uint8_t save_status,uint32_t state,bool expected_start) {
        // Status0 means valid in the native active-save record. The inactive
        // record is deliberately invalid, so the real profile selector runs.
        PPC_STORE_U8(profile+20,1);PPC_STORE_U8(profile+20+2*8,save_status);
        PPC_STORE_U32(owner+12,state);PPC_STORE_U32(result+4,0);
        PPCContext original{};original.r1.u64=0xF000;original.r3.u64=invocation;original.r31.u64=0xABCD;original.lr=0x12345678;original.fpscr.InitHost();
        auto reference=original;__imp__sub_82B1B1B0(reference,base);
        const auto bool_value=PPC_LOAD_U8(result+8);const auto csr=_mm_getcsr();
        PPC_STORE_U32(result+4,0);sub_82B1B1B0(original,base);
        ++cases;
        return require(!std::memcmp(&original,&reference,sizeof(original)) && _mm_getcsr()==csr &&
            PPC_LOAD_U32(result+4)==2 && PPC_LOAD_U8(result+8)==bool_value &&
            bool_value==uint8_t(save_status==0) && PPC_LOAD_U32(owner+12)==(expected_start?1:state) &&
            PPC_LOAD_U32(invocation+12)==0x7000,"real save/resume callbacks must honor readiness and preserve query result, caller context, FP state and owner field");
    };
    auto resume=[&](uint32_t state,bool tracked,bool null_target=false) {
        PPC_STORE_U32(owner+12,state);PPC_STORE_U32(invocation+12,null_target?0:owner);
        PPCContext actual{};actual.r1.u64=0xF000;actual.r3.u64=invocation;actual.r31.u64=0x76543210;actual.lr=0x11223344;actual.fpscr.InitHost();
        auto reference=actual;__imp__sub_82A41A80(reference,base);const auto expected_state=PPC_LOAD_U32(owner+12);const auto csr=_mm_getcsr();
        PPC_STORE_U32(owner+12,state);sub_82A41A80(actual,base);++cases;
        const bool ok=!std::memcmp(&actual,&reference,sizeof actual) && _mm_getcsr()==csr &&
            PPC_LOAD_U32(owner+12)==expected_state && NativeOnlineStatus().continue_requested==tracked;
        PPC_STORE_U32(invocation+12,0x7000);
        return require(ok,"manual Continue preserves original full caller/FP/state behavior and tracks only an accepted live online request");
    };
    REXCVAR_SET(em2_online_auto_continue,true);Unbind();
    if(NativeOnlineStatus().state!=NativeSessionState::WaitingForWorld) {
        Bind(owner,base);if(!query(0,2,false))return false;
        const bool requested=NativeOnlineStatus().continue_requested;
        for(uint32_t state:{0u,1u,2u,3u})if(!resume(state,requested))return false;
        if(!resume(2,requested,true))return false;
        REXLOG_INFO("Native online Continue leaves a valid native save query unchanged without a waiting session");return true;
    }
    if(!query(0,2,false))return false;
    Bind(owner,base);
    if(!query(1,2,false) || !query(2,2,false))return false;
    for(uint32_t state:{0u,1u,3u})if(!query(0,state,false))return false;
    REXCVAR_SET(em2_online_auto_continue,false);if(!query(0,2,false))return false;REXCVAR_SET(em2_online_auto_continue,true);
    PPC_STORE_U32(owner,0x82112234);if(!query(0,2,false))return false;
    PPC_STORE_U32(owner,0x82112233);Bind(owner,base);
    if(!require(ClaimNativeOnlineContinue(),"negative cases must not consume startup ownership"))return false;
    if(!require(!NativeOnlineStatus().continue_requested,"a temporary claim must not announce an accepted Continue"))return false;
    FinishNativeOnlineContinue(false);
    // Native script queries may run on a different host thread from command
    // registration. Resume only posts a state request; the binding lock owns
    // lifetime and the current callback supplies its own valid guest stack.
    if(manual){
        REXCVAR_SET(em2_online_auto_continue,false);
        for(uint32_t state:{0u,1u,3u})if(!resume(state,false))return false;
        if(!resume(2,false,true))return false;
        PPC_STORE_U32(owner,0x82112234);if(!resume(2,false))return false;
        PPC_STORE_U32(owner,0x82112233);Bind(owner,base);
    }
    bool started=false;std::thread script_query([&]{
        if(manual){started=resume(2,true);return;}
        PPC_STORE_U32(owner+12,4);
        PPCContext late{};late.r1.u64=0xF000;late.r3.u64=0xBEEF;late.r31.u64=0x99887766;late.lr=0x12345678;late.fpscr.InitHost();
        const auto saved=late;const auto csr=_mm_getcsr();ProbeBoundContinue(late,base);++cases;
        started=require(!std::memcmp(&late,&saved,sizeof(late)) && _mm_getcsr()==csr && PPC_LOAD_U32(owner+12)==1,"late registration replays original save query and Resume while preserving the complete registration caller/FP state");
    });script_query.join();
    if(!started || !require(NativeOnlineStatus().continue_requested,"accepted Continue must publish loading status"))return false;
    REXCVAR_SET(em2_online_auto_continue,true); // A later automatic query must not repeat a manual request.
    {
        em2::ControlKey('W',true);
        struct ReleaseKey {~ReleaseKey(){em2::ControlKey('W',false);}} release;
        rex::input::X_INPUT_GAMEPAD neutral{};
        for(unsigned player=0;player<2;++player){
            PPCContext input{};input.r3.u64=player;input.r4.u64=0x8000;input.fpscr.InitHost();
            std::memset(base+0x8000,0xFF,sizeof(rex::input::X_INPUT_STATE));sub_831BB758(input,base);
            const auto* state=reinterpret_cast<const rex::input::X_INPUT_STATE*>(base+0x8000);
            if(!require(input.r3.u32==0 && !std::memcmp(&state->gamepad,&neutral,sizeof(neutral)),
                "accepted online Continue parks both native input ports before world preparation, even with movement held"))return false;
        }
    }
    // Model a retired callback owner while another valid save query arrives.
    // A consumed request must return before dereferencing its owner at all.
    PPC_STORE_U32(0x9000,0x82112233);Bind(0x9000,base);
    struct Protect {uint8_t* address;DWORD previous=0;bool active=false;
        ~Protect(){if(active){DWORD ignored;VirtualProtect(address,4096,previous,&ignored);}}}retired{base+0x9000};
    retired.active=VirtualProtect(retired.address,4096,PAGE_NOACCESS,&retired.previous)!=0;
    if(!require(retired.active,"retired owner protection must be active") || !query(0,2,false) ||
        !require(!ClaimNativeOnlineContinue(),"accepted Continue is single-flight"))return false;
    REXLOG_INFO("Native online Continue passed {} real callback cases (manual {}): active save, native state guards, owner lifetime, cross-thread callback, opt-out, rejected claim retry, exact query context and one request",cases,manual);
    return true;
}

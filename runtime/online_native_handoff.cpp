#include "online_native_handoff.h"
#include "online_native_menu.h"
#include "online_runtime.h"
#include "online_content_root.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/runtime.h>
#include <rex/kernel/kernel_state.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <optional>
#include <windows.h>

PPC_EXTERN_FUNC(__imp__sub_82768A40);
PPC_EXTERN_FUNC(__imp__sub_827685C8);
PPC_EXTERN_FUNC(__imp__sub_827680D0);
PPC_EXTERN_FUNC(__imp__sub_827659D0);
PPC_EXTERN_FUNC(__imp__sub_82A41A80);
PPC_EXTERN_FUNC(__imp__sub_82B1B1B0);
REXCVAR_DECLARE(bool,em2_online_world_random);
REXCVAR_DEFINE_BOOL(em2_native_online_handoff,false,"Online","Experimental authored native lobby save reload and Resume handoff");

namespace em2::net {
namespace {
using Clock=std::chrono::steady_clock;
std::atomic<bool> registered{false};
struct Watch {
    std::atomic<uint64_t> generation{0};
    std::atomic<uint32_t> manager{0};
    // 0 pending, 1 original full-load callback succeeded, 2 rejected/no save.
    std::atomic<unsigned> result{0};
    std::atomic<unsigned> decoded{0};
} watch;
enum class Phase {Idle,WaitingForIdle,Reloading,WaitingForCompletion,Finished,Failed};
struct Handoff {
    Phase phase=Phase::Idle;
    std::optional<BootResult> prepared;
    std::unique_ptr<HostCampaignLease> campaign_lease;
    uint32_t owner=0,manager=0,backend=0;
    uint64_t generation=0;
    bool routed=false,adopted=false;
    Clock::time_point deadline{};
} handoff;
struct NativeSave {
    uint32_t manager=0,backend=0,state=0;
    bool disabled=false;
};
bool Readable(uint8_t* base,uint32_t address,size_t size){
    return base && address>=4096 && uint64_t(address)+size<=0x100000000ull &&
        em2::GuestReadBatch::Readable(PPC_RAW_ADDR(address),size);
}
bool ReadNativeSave(uint8_t* base,NativeSave& save){
    em2::GuestReadBatch inspection;
    if(!Readable(base,0x833DAB54,4))return false;
    save.manager=PPC_LOAD_U32(0x833DAB54);
    if(!Readable(base,save.manager,380) || PPC_LOAD_U32(save.manager)!=0x820785F0)return false;
    save.backend=PPC_LOAD_U32(save.manager+360);
    if(!Readable(base,save.backend,1632) || PPC_LOAD_U32(save.backend)!=0x820A2F00)return false;
    save.state=PPC_LOAD_U32(save.backend+4);
    // 82768A40 takes an empty-save fallback when +301 or +308 is set.
    // Reject that path; never clear native SaveManager flags ourselves.
    save.disabled=PPC_LOAD_U8(save.manager+300) || PPC_LOAD_U8(save.manager+301) || PPC_LOAD_U8(save.manager+308);
    return true;
}
bool LiveMenu(uint8_t* base,uint32_t owner){
    em2::GuestReadBatch inspection;
    if(!Readable(base,owner,24) || PPC_LOAD_U32(owner)!=0x8211FF18 || PPC_LOAD_U32(owner+4)!=1)return false;
    // FrontMenu state4 is idle (82A424D0 dispatches only states0..3).
    // No storage dialog, outgoing transition or initial load may swap roots.
    return PPC_LOAD_U32(owner+12)==4;
}
struct Preserve {
    PPCContext& ctx;PPCContext saved;unsigned csr=_mm_getcsr();DWORD error=GetLastError();
    explicit Preserve(PPCContext& value):ctx(value),saved(value){}
    ~Preserve(){ctx=saved;_mm_setcsr(csr);SetLastError(error);}
};
void Fail(std::string reason){
    watch.generation.store(0,std::memory_order_release);
    handoff.phase=handoff.routed?Phase::Failed:Phase::Idle;
    if(handoff.routed){
        // Routing remains private. Never resume against a half-loaded save or
        // switch back while native asynchronous IO may still be in progress.
        reason+=" Close and reopen the game before trying again. Your normal save was not replaced.";
        if(handoff.adopted)FailNativeOnlineMenuPreparation(reason);
        SetNativeOnlineMenuHandoffAvailable(false);
    }
    handoff.prepared.reset();
    FailNativeOnlineMenuHandoff(reason);
    REXLOG_ERROR("Native online lobby handoff failed: {}",reason);
}
bool QueueRefresh(PPCContext& ctx,uint8_t* base,uint32_t manager){
    Preserve restore(ctx);
    ctx.r3.u64=manager;
    __imp__sub_82768A40(ctx,base);
    return true;
}
bool Resume(PPCContext& ctx,uint8_t* base,uint32_t owner){
    Preserve restore(ctx);
    if(!LiveMenu(base,owner) || ctx.r1.u32<512)return false;
    const auto stack=ctx.r1.u32-256,arguments=stack+96;
    if(!Readable(base,stack,128))return false;
    PPC_STORE_U32(stack,ctx.r1.u32);std::memset(PPC_RAW_ADDR(arguments),0,16);
    PPC_STORE_U32(arguments+12,owner);
    ctx.r1.u64=stack;ctx.r3.u64=arguments;
    __imp__sub_82A41A80(ctx,base);
    return PPC_LOAD_U32(owner+12)==1;
}
bool RefreshedCampaignPresent(PPCContext& ctx,uint8_t* base){
    Preserve restore(ctx);
    if(ctx.r1.u32<512)return false;
    const auto stack=ctx.r1.u32-256,arguments=stack+96,result=stack+128;
    if(!Readable(base,stack,160))return false;
    PPC_STORE_U32(stack,ctx.r1.u32);std::memset(PPC_RAW_ADDR(arguments),0,48);
    PPC_STORE_U32(arguments+8,result);ctx.r1.u64=stack;ctx.r3.u64=arguments;
    // Use the original game's query, not a fabricated save-summary status.
    // FrontMenu82A42018 otherwise accepts Resume and silently chooses New Game
    // when this summary remains invalid after a failed/mismatched refresh.
    __imp__sub_82B1B1B0(ctx,base);
    return PPC_LOAD_U32(result+4)==2 && PPC_LOAD_U8(result+8)!=0;
}
void PublishCompletion(uint64_t generation,uint32_t manager,bool full_load,bool accepted,bool save_exists,uint32_t result,uint32_t archive_state){
    if(!generation || watch.generation.load(std::memory_order_acquire)!=generation ||
        watch.manager.load(std::memory_order_relaxed)!=manager)return;
    if(full_load)watch.result.store(accepted && result==11 && archive_state==6?1:2,std::memory_order_release);
    else if(!accepted || !save_exists)watch.result.store(2,std::memory_order_release);
}
void ObserveCompletion(PPCContext& ctx,uint8_t* base,PPCFunc* original,bool full_load){
    const auto generation=watch.generation.load(std::memory_order_acquire);
    const bool accepted=ctx.r3.u32!=0;
    NativeSave before;bool inspected=false,save_exists=false;uint32_t result_code=0,archive_state=0,buffer=0;
    if(generation){
        Preserve restore(ctx);
        try{
            inspected=ReadNativeSave(base,before);
            if(inspected){save_exists=PPC_LOAD_U8(before.backend+103)!=0;result_code=PPC_LOAD_U32(before.backend+620);archive_state=PPC_LOAD_U32(before.manager+28);buffer=PPC_LOAD_U32(before.manager+36);}
        }catch(...){}
    }
    original(ctx,base);
    if(!generation)return;
    Preserve restore(ctx);
    // Keep the native completion outside the catch: only bounded observation
    // can be suppressed, never an original native exception or return value.
    try{
        NativeSave save;
        if(!inspected || !ReadNativeSave(base,save) || save.manager!=before.manager || save.backend!=before.backend)return;
        // Refresh can synchronously queue the next load and replace its status
        // buffer. Its save-exists/result values belong to callback entry.
        const bool decoded=watch.decoded.load(std::memory_order_acquire)==1;
        const bool retained=PPC_LOAD_U32(save.manager+36)==buffer && Readable(base,buffer,4);
        const bool completed=!PPC_LOAD_U8(save.backend+100) && !PPC_LOAD_U8(save.manager+303);
        // 827680D0 temporarily consumes the ready archive, applies its global
        // metadata, destroys the temporary archive object, then deliberately
        // restores state6 at82768224. The retained manager buffer is consumed
        // later by authored Resume. A real successful decode at82768180 is the
        // evidence that distinguishes that lifecycle from a stale state word.
        PublishCompletion(generation,save.manager,full_load,accepted && (!full_load || (decoded && retained && completed)),save_exists,result_code,PPC_LOAD_U32(save.manager+28));
        REXLOG_INFO("Native lobby {} completion: accepted {}, save exists {}, result {}, archive state {}->{}, decoded {}, retained {}, completed {}",
            full_load?"load":"refresh",accepted,save_exists,result_code,archive_state,PPC_LOAD_U32(save.manager+28),decoded,retained,completed);
    }catch(...){}
}
void ObserveArchiveRead(PPCContext& ctx,uint8_t* base,PPCFunc* original){
    const auto generation=watch.generation.load(std::memory_order_acquire);
    const auto manager=ctx.r3.u32,caller=uint32_t(ctx.lr);
    original(ctx,base);
    // Only the original full-load callback's second, global-data decode is
    // evidence for this handoff. Header probes and later Resume reads cannot
    // accidentally satisfy it. No original register/memory/FP result changes.
    if(generation && caller==0x82768180 && watch.generation.load(std::memory_order_acquire)==generation &&
        manager==watch.manager.load(std::memory_order_relaxed))
        watch.decoded.store(ctx.r3.u32?1:2,std::memory_order_release);
}
}
void RegisterNativeOnlineHandoff(bool available){
    registered.store(available,std::memory_order_release);
    SetNativeOnlineMenuHandoffAvailable(available && REXCVAR_GET(em2_native_online_handoff) && REXCVAR_GET(em2_online_world_random));
}
void UpdateNativeOnlineHandoff(PPCContext& ctx,uint8_t* base){
    // The registered native page capability authorizes Start once. A console
    // preference change must not abandon an already accepted reload midway.
    if(!registered.load(std::memory_order_acquire))return;
    if(handoff.phase==Phase::Finished || handoff.phase==Phase::Failed)return;
    Preserve restore(ctx);
    const auto owner=ctx.r3.u32;
    // Outer frontend states also use this base update; only the actual live
    // FrontMenu may consume a pending Start or service its own reload. Native
    // 8211F530 is the preceding save-selection substate, not this owner.
    if(!LiveMenu(base,owner))return;
    if(handoff.phase==Phase::Idle){
        auto prepared=TakeNativeOnlineMenuReady();if(!prepared)return;
        if(prepared->session->identity().startup_policy!=StartupPolicy::NativeMenu ||
            prepared->session->identity().initialization_policy!=InitializationPolicy::SharedNativeV1){
            FailNativeOnlineMenuHandoff("This session was prepared with different native initialization settings. Enable deterministic online initialization, cancel, and host or join again.");return;
        }
        handoff.prepared=std::move(prepared);handoff.owner=owner;
        handoff.routed=handoff.adopted=false;
        handoff.deadline=Clock::now()+std::chrono::seconds(45);
        handoff.phase=Phase::WaitingForIdle;
    }
    if(!LiveMenu(base,owner) || owner!=handoff.owner){Fail("The title menu changed while preparing online play.");return;}
    if(Clock::now()>=handoff.deadline){Fail("The native save loader did not finish preparing the shared save.");return;}
    if(!handoff.adopted){
        handoff.prepared->session->Poll();
        if(handoff.prepared->session->state()!=State::Ready){Fail("The other player disconnected while preparing the shared game.");return;}
    }else if(NativeOnlineStatus().state==NativeSessionState::Failed){Fail("The online connection was lost while preparing the shared game.");return;}
    NativeSave save;
    if(!ReadNativeSave(base,save)){Fail("The native save manager is unavailable at this menu.");return;}
    if(handoff.phase==Phase::WaitingForIdle){
        if(save.state!=0)return;
        if(save.disabled){Fail("Native save loading is disabled. Return to the title menu and choose your save first.");return;}
        auto* runtime=rex::Runtime::instance();auto* kernel=runtime?runtime->kernel_state():nullptr;
        if(!kernel || !kernel->user_profile()){Fail("Choose your local player profile before starting online.");return;}
        std::string error;
        if(!SwitchNativeContentRoot(kernel->content_manager(),handoff.prepared->user_data,error,kernel->user_profile())){
            // Live packages are not forcibly closed. If an authored task is
            // draining, a later bounded menu update may retry the same route.
            if(error=="Waiting for the game to finish using its current save.")return;
            Fail("Could not select the shared online save: "+error);return;
        }
        handoff.routed=true;
        if(!BeginNativeOnlineMenuPreparation(handoff.prepared->session)){
            Fail("Could not transfer the ready connection to the native game.");return;
        }
        handoff.adopted=true;handoff.manager=save.manager;handoff.backend=save.backend;
        ++handoff.generation;watch.result.store(0,std::memory_order_relaxed);watch.decoded.store(0,std::memory_order_relaxed);
        watch.manager.store(save.manager,std::memory_order_relaxed);watch.generation.store(handoff.generation,std::memory_order_release);
        handoff.phase=Phase::Reloading;
        REXLOG_INFO("Native lobby selected private content root; requesting original SaveManager refresh");
        QueueRefresh(ctx,base,save.manager);return;
    }
    if(save.manager!=handoff.manager || save.backend!=handoff.backend){Fail("The native save manager changed during the shared-save load.");return;}
    if(handoff.phase==Phase::Reloading){
        const auto result=watch.result.load(std::memory_order_acquire);
        if(result==2){Fail("The native engine could not load the host's shared save.");return;}
        if(result!=1)return;
        handoff.phase=Phase::WaitingForCompletion;
    }
    if(handoff.phase==Phase::WaitingForCompletion){
        if(save.state!=0)return;
        if(!RefreshedCampaignPresent(ctx,base)){Fail("The shared save loaded, but the native campaign summary is not valid. Continue was not requested.");return;}
        REXLOG_INFO("Native lobby verified refreshed campaign through original IsSavedGamePresent");
        auto* runtime=rex::Runtime::instance();
        if(!runtime){Fail("The native runtime is unavailable while retaining the online campaign.");return;}
        auto campaign=RecordNativeOnlineHostCampaign(runtime->user_data_root(),handoff.prepared->user_data);
        if(!campaign.error.empty()){Fail("Could not retain the host's online campaign: "+campaign.error);return;}
        handoff.campaign_lease=std::move(campaign.lease);
        if(handoff.campaign_lease)REXLOG_INFO("Native lobby retained validated host campaign with an exclusive active-session lease");
        if(!Resume(ctx,base,owner)){Fail("The native menu did not accept Continue for the shared save.");return;}
        if(!FinishNativeOnlineMenuPreparation()){Fail("The prepared native online session could not enter gameplay.");return;}
        CompleteNativeOnlineMenuHandoff();
        watch.generation.store(0,std::memory_order_release);
        handoff.phase=Phase::Finished;handoff.prepared.reset();handoff.owner=0;
        // Resume's original 829ECB70 transition destroys/recreates both native
        // player records before LoadLevel. No duplicate spawn, GUID rewrite,
        // direct SaveManager flag write, or premature input timeline is needed.
        REXLOG_INFO("Native lobby completed authored save reload and accepted native Continue");
    }
}
bool TestNativeOnlineHandoff(){
    const auto old_generation=watch.generation.load();const auto old_manager=watch.manager.load();const auto old_result=watch.result.load();const auto old_decoded=watch.decoded.load();
    struct Restore{uint64_t generation;uint32_t manager;unsigned result,decoded;~Restore(){watch.manager.store(manager);watch.result.store(result);watch.decoded.store(decoded);watch.generation.store(generation);}}restore{old_generation,old_manager,old_result,old_decoded};
    watch.manager.store(123);watch.generation.store(7);watch.result.store(0);
    PublishCompletion(6,123,true,true,true,11,3);if(watch.result.load())return false;
    PublishCompletion(7,124,true,true,true,11,3);if(watch.result.load())return false;
    PublishCompletion(7,123,false,true,true,11,3);if(watch.result.load())return false;
    PublishCompletion(7,123,true,true,true,11,3);if(watch.result.load()!=2)return false;
    watch.result.store(0);PublishCompletion(7,123,true,false,true,11,3);if(watch.result.load()!=2)return false;
    watch.result.store(0);PublishCompletion(7,123,false,true,false,11,3);if(watch.result.load()!=2)return false;
    watch.result.store(0);PublishCompletion(7,123,true,true,true,11,6);if(watch.result.load()!=1)return false;
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));if(!base)return false;
    struct Free{uint8_t* base;unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Free(){VirtualFree(base,0,MEM_RELEASE);_mm_setcsr(csr);SetLastError(error);}}free{base};
    if(!VirtualAlloc(base,0x10000,MEM_COMMIT,PAGE_READWRITE) || !VirtualAlloc(base+0x833D0000,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
    constexpr uint32_t manager=0x2000,backend=0x4000,menu=0x6000,buffer=0x8000;
    PPC_STORE_U32(0x833DAB54,manager);PPC_STORE_U32(manager,0x820785F0);PPC_STORE_U32(manager+360,backend);
    PPC_STORE_U32(manager+36,buffer);PPC_STORE_U32(buffer,0x12345678);
    PPC_STORE_U32(backend,0x820A2F00);PPC_STORE_U32(menu,0x8211FF18);PPC_STORE_U32(menu+4,1);
    for(unsigned state=0;state<9;++state){PPC_STORE_U32(menu+12,state);if(LiveMenu(base,menu)!=(state==4))return false;}
    PPC_STORE_U32(menu+4,2);if(LiveMenu(base,menu))return false;
    NativeSave save;if(!ReadNativeSave(base,save) || save.manager!=manager || save.backend!=backend || save.disabled)return false;
    for(unsigned flag:{300u,301u,308u}){PPC_STORE_U8(manager+flag,1);if(!ReadNativeSave(base,save) || !save.disabled)return false;PPC_STORE_U8(manager+flag,0);}
    {
        PPCContext query{};query.r1.u64=0xF000;query.r31.u64=0xAABBCCDDEEFF0011ull;query.lr=0x827680D0;query.fpscr.InitHost();
        const auto expected=query;const auto csr=_mm_getcsr();const auto error=GetLastError();
        PPC_STORE_U32(manager+296,0);PPC_STORE_U8(manager+20,0);
        if(!RefreshedCampaignPresent(query,base) || std::memcmp(&query,&expected,sizeof(query)) || _mm_getcsr()!=csr || GetLastError()!=error)return false;
        PPC_STORE_U8(manager+20,1);
        if(RefreshedCampaignPresent(query,base) || PPC_LOAD_U8(manager+20)!=1 || std::memcmp(&query,&expected,sizeof(query)) || _mm_getcsr()!=csr || GetLastError()!=error)return false;
        PPC_STORE_U8(manager+20,0);
    }
    // An original callback can replace its status buffer while queuing the
    // next operation. Preserve the entry result and the original's exact CPU,
    // FP, error and guest-memory results; observe only after original success.
    const auto original=+[](PPCContext& ctx,uint8_t* base){
        ctx.r3.u64=manager;ctx.lr=0x82768180;
        const auto decoder=+[](PPCContext& ctx,uint8_t* base){PPC_STORE_U32(manager+28,3);ctx.r3.u64=1;};
        ObserveArchiveRead(ctx,base,decoder);
        PPC_STORE_U8(backend+103,0);PPC_STORE_U32(backend+620,0);PPC_STORE_U32(manager+28,6);
        PPC_STORE_U8(backend+100,0);PPC_STORE_U8(manager+303,0);
        ctx.r31.u64=0x1020304050607080ull;ctx.r3.u64=8192;ctx.lr=0x827680D0;
        _mm_setcsr(0x3F80);SetLastError(71);
    };
    auto prepare=[&]{
        PPC_STORE_U8(backend+103,1);PPC_STORE_U32(backend+620,11);PPC_STORE_U32(manager+28,3);
        PPC_STORE_U8(backend+100,1);PPC_STORE_U8(manager+303,1);
        watch.manager.store(manager);watch.generation.store(7);watch.result.store(0);watch.decoded.store(0);
    };
    PPCContext actual{};actual.r3.u64=1;actual.fpscr.InitHost();auto expected=actual;
    prepare();original(expected,base);prepare();ObserveCompletion(actual,base,original,true);
    if(watch.result.load()!=1 || watch.decoded.load()!=1 || std::memcmp(&actual,&expected,sizeof(actual)) || _mm_getcsr()!=0x3F80 || GetLastError()!=71 || PPC_LOAD_U32(manager+28)!=6)return false;
    actual={};actual.r3.u64=1;prepare();ObserveCompletion(actual,base,original,false);
    if(watch.result.load()!=0)return false;
    // A ready state without the original callback's successful decoder call
    // is not accepted, nor can an unrelated native header probe satisfy it.
    const auto no_decode=+[](PPCContext&,uint8_t* base){PPC_STORE_U32(manager+28,6);PPC_STORE_U8(backend+100,0);PPC_STORE_U8(manager+303,0);};
    actual={};actual.r3.u64=1;prepare();ObserveCompletion(actual,base,no_decode,true);
    if(watch.result.load()!=2 || watch.decoded.load())return false;
    actual={};actual.r3.u64=manager;actual.lr=0x827661A8;prepare();
    const auto decoder=+[](PPCContext& ctx,uint8_t*){ctx.r3.u64=1;};
    ObserveArchiveRead(actual,base,decoder);if(watch.decoded.load())return false;
    actual.r3.u64=manager;actual.lr=0x82768180;ObserveArchiveRead(actual,base,decoder);if(watch.decoded.load()!=1)return false;
    // No observer registration means no speculative read of invalid memory.
    watch.generation.store(0);actual={};actual.r3.u64=1;ObserveCompletion(actual,base,original,true);
    if(actual.r31.u64!=0x1020304050607080ull || GetLastError()!=71)return false;
    return true;
}
}
PPC_FUNC_IMPL(sub_827685C8){em2::net::ObserveCompletion(ctx,base,__imp__sub_827685C8,false);}
PPC_FUNC_IMPL(sub_827680D0){em2::net::ObserveCompletion(ctx,base,__imp__sub_827680D0,true);}
PPC_FUNC_IMPL(sub_827659D0){em2::net::ObserveArchiveRead(ctx,base,__imp__sub_827659D0);}

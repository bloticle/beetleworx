#include "online_room_leave.h"
#include "online_room_runtime.h"
#include "online_room_gameplay.h"
#include "online_native_roster.h"
#include "online_native_handoff.h"
#include "online_native_menu.h"
#include "guest_read_batch.h"
#include "game_console.h"
#include "allocation_trace.h"
#include "players.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>

PPC_EXTERN_FUNC(__imp__sub_829DA2C0);
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DEFINE_BOOL(em2_probe_room_return_title,false,"Debug","Private native lobby QA only: return a stopped completed world to title and refresh original storage");
REXCVAR_DEFINE_BOOL(em2_probe_room_recovery_prompt,false,"Debug","Validated private native lobby only: request recovery through the authored HUD button instead of automatic return");
REXCVAR_DEFINE_UINT32(em2_probe_room_disconnect_tick,0xFFFFFFFFu,"Debug","Private native lobby QA only: disconnect at a completed outer boundary once at/after this tick");
namespace em2::net {
namespace {
using Clock=std::chrono::steady_clock;
enum class Phase {Idle,Loading,Refreshing,Ready,Failed};
struct Leave {
    Phase phase=Phase::Idle;DWORD thread=0;uint64_t load=0;
    bool loaded=false;Clock::time_point deadline{};
    std::unique_ptr<NativeRoomActors> actors;
} leave;
bool disconnect_injected=false;
std::atomic<bool> halted{false};
std::atomic<bool> render_retired{false};
enum class Recovery:int {Inactive,Available,Requested,Returning,RestartRequired};
std::atomic<Recovery> recovery{Recovery::Inactive};
std::atomic<bool> recovery_requested{false};
std::mutex recovery_mutex;
std::string recovery_message;
void RecoveryHint(Recovery state,std::string message){
    {std::lock_guard lock(recovery_mutex);recovery_message=std::move(message);}
    recovery.store(state,std::memory_order_release);
}
bool RequestRecovery(){auto expected=Recovery::Available;if(!recovery.compare_exchange_strong(expected,Recovery::Requested))return false;recovery_requested.store(true,std::memory_order_release);return true;}
struct Environment {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~Environment(){_mm_setcsr(csr);SetLastError(error);}};
bool Readable(uint8_t* base,uint32_t p,size_t n){return base && p>=4096 && uint64_t(p)+n<=0x100000000ull && GuestReadBatch::Readable(base+p,n);}
struct Caller {
    PPCContext& ctx;PPCContext saved;Environment environment;
    std::array<uint8_t,32768> bytes{};uint8_t* memory=nullptr;uint32_t text=0;
    Caller(PPCContext& c,uint8_t* base):ctx(c),saved(c){const auto top=c.r1.u32&~15u;
        if(top>=65536 && Readable(base,top-32768,bytes.size())){
            memory=base+top-32768;std::memcpy(bytes.data(),memory,bytes.size());
            text=top-512;ctx.r1.u64=top-4096;}}
    ~Caller(){if(memory)std::memcpy(memory,bytes.data(),bytes.size());ctx=saved;}
};
void Fail(const char* why){leave.phase=Phase::Failed;halted.store(true,std::memory_order_release);render_retired.store(true,std::memory_order_release);RecoveryHint(Recovery::RestartRequired,std::string("Could not return safely: ")+why+". Restart Beetleworx; your offline save is retained.");REXLOG_ERROR("Native Room title return failed: {}; world remains halted",why);}
}
bool NativeRoomRecoveryOption(int id,int choice,int& result){
    if(id!=2790 && id!=2791)return false;
    result=0;
    if(id==2790){
        if(choice==1){result=int(RequestRecovery());if(result)REXLOG_INFO("Native recovery HUD return request accepted; queued for checked native boundary");}
        else if(choice==2)result=REXCVAR_GET(em2_probe_room_recovery_prompt) && REXCVAR_GET(em2_private_profile) && NativeOnlineMenuQaActive();
        else if(choice==-1 || choice==0)result=int(recovery.load(std::memory_order_acquire));
        return true;
    }
    static thread_local std::string text;
    if(choice==0){std::lock_guard lock(recovery_mutex);text=recovery_message;}
    if(choice>=0 && size_t(choice)<text.size())result=uint8_t(text[size_t(choice)]);
    return true;
}
void PumpNativeRoomRecoveryHud(PPCContext& ctx,uint8_t* base,unsigned container){
    if(recovery.load(std::memory_order_acquire)==Recovery::Inactive)return;
    Environment environment;
    // The current dispatcher owns the registered PCOnlineChat callback. Only
    // invoke its movie when that movie belongs to this locked UI container.
    if(!Readable(base,0x833DCB9C,4) || !Readable(base,container,368))return;
    const auto dispatcher=PPC_LOAD_U32(0x833DCB9C);
    if(!Readable(base,dispatcher,8))return;
    const auto movie=PPC_LOAD_U32(dispatcher+4);
    if(!Readable(base,movie,160) || !PPC_LOAD_U8(movie+16))return;
    bool owned=false;
    for(unsigned offset:{348u,360u}){
        const auto array=PPC_LOAD_U32(container+offset),count=PPC_LOAD_U32(container+offset+4);
        if(count>1024 || !Readable(base,array,size_t(count)*4))continue;
        for(unsigned i=0;i<count;++i)if(PPC_LOAD_U32(array+i*4)==movie){owned=true;break;}
    }
    if(!owned)return;
    const auto handle=PPC_LOAD_U32(movie+156);
    if(!Readable(base,handle,4))return;
    const auto vtable=PPC_LOAD_U32(handle);
    if(!Readable(base,vtable,96))return;
    const auto method=PPC_LOAD_U32(vtable+92);
    if(method<0x82000000 || method>=0x83400000 || (method&3))return;
    static thread_local Clock::time_point next{};
    const auto now=Clock::now();if(now<next)return;next=now+std::chrono::milliseconds(100);
    Caller caller(ctx,base);if(!caller.memory)return;
    constexpr char name[]="PCOnlineRecoveryTick";
    std::memcpy(base+caller.text,name,sizeof(name));
    // Original dispatcher 82834EA8 uses this exact Invoke ABI: movie view,
    // method name, optional result, argument array, argument count. Calling
    // only this authored method dispatches no onEnterFrame or world update.
    ctx.r3.u64=handle;ctx.r4.u64=caller.text;ctx.r5.u64=0;ctx.r6.u64=0;ctx.r7.u64=0;
    PPC_CALL_INDIRECT_FUNC(method);
}
bool NativeRoomRecoveryKey(unsigned key,bool down){
    if(key!=VK_RETURN)return false;
    const auto state=recovery.load(std::memory_order_acquire);
    if(state!=Recovery::Available && state!=Recovery::Requested && state!=Recovery::Returning)return false;
    if(down && state==Recovery::Available)RequestRecovery();return true;
}
bool TestNativeRoomRecoveryRequests(){
    if(NativeRoomActive() || leave.phase!=Phase::Idle || recovery.load()!=Recovery::Inactive)return false;
    struct Restore{~Restore(){recovery_requested=false;RecoveryHint(Recovery::Inactive,{});}}restore;
    int result=99;
    if(!NativeRoomRecoveryOption(2790,1,result) || result || NativeRoomRecoveryKey(VK_RETURN,true))return false;
    RecoveryHint(Recovery::Available,"Offline save retained.");
    if(!NativeRoomRecoveryOption(2790,-1,result) || result!=1 || !NativeRoomRecoveryKey(VK_RETURN,true) || !recovery_requested.load())return false;
    if(!NativeRoomRecoveryOption(2790,-1,result) || result!=2 || !NativeRoomRecoveryOption(2790,1,result) || result)return false;
    // Publishing an interim unsafe-boundary hint must not lose an already
    // accepted request. Only the native owner consumes or rejects that queue.
    RecoveryHint(Recovery::RestartRequired,"Waiting for native boundary.");
    if(!recovery_requested.load() || leave.phase!=Phase::Idle || NativeRoomRecoveryKey('A',true))return false;
    recovery_requested=false;
    if(!NativeRoomRecoveryOption(2790,1,result) || result || NativeRoomRecoveryKey(VK_RETURN,true))return false;
    if(NativeRoomRecoveryOption(2789,0,result) || NativeRoomRecoveryOption(2792,0,result))return false;
    RecoveryHint(Recovery::Inactive,{});if(NativeRoomRecoveryKey(VK_RETURN,false))return false;
    REXLOG_INFO("Native recovery requests passed: inactive/unsafe rejection, real keyboard/GFx admission, duplicate coalescing, pending-boundary preservation; no world transition dispatched");
    return true;
}
bool NativeRoomTitleLeaveHalted()noexcept{return halted.load(std::memory_order_acquire);}
bool NativeRoomTitleLeaveRenderRetired()noexcept{return render_retired.load(std::memory_order_acquire);}
void NotifyNativeRoomTitleLeaveLoaded(std::string_view level){
    if(leave.phase==Phase::Loading && leave.thread==GetCurrentThreadId() && level=="mainmenu.start" &&
       em2::CurrentNativeLoadGeneration()!=leave.load){leave.loaded=true;REXLOG_INFO("Native Room title return: original title load completed");}
}
void TickNativeRoomTitleLeave(PPCContext& ctx,uint8_t* base,bool completed){
    Environment environment;
    const bool private_mode=REXCVAR_GET(em2_probe_room_return_title) && REXCVAR_GET(em2_private_profile) && NativeOnlineMenuQaActive();
    const bool private_auto=private_mode && !REXCVAR_GET(em2_probe_room_recovery_prompt);
    if(NativeRoomLoadsCancelled())return;
    if(leave.phase==Phase::Ready && NativeRoomActive() && NativeRoomStatus().state!=NativeSessionState::Failed){leave=Leave{};RecoveryHint(Recovery::Inactive,{});}
    if(leave.phase==Phase::Ready || leave.phase==Phase::Failed)return;
    if(leave.phase==Phase::Idle){
        const auto tick=REXCVAR_GET(em2_probe_room_disconnect_tick);
        if(private_mode && !disconnect_injected && tick!=0xFFFFFFFFu && completed && NativeOnlineMenuQaActive() &&
           NativeRoomActive() && NativeRoomStatus().state==NativeSessionState::Attached){
            auto* game=CurrentNativeRoomGameplay();auto* room=PrivateNativeMenuRoom();
            const auto finished=game?game->last_native_completed_tick():std::nullopt;
            if(room && finished && *finished>=tick && !game->LeaveBoundaryBlocker(base)){
                disconnect_injected=true;room->Disconnect();
                REXLOG_INFO("Private native Room completed disconnect injected: tick {}; no process closed",*finished);
                return;
            }
        }
        if(!NativeRoomActive() || NativeRoomStatus().state!=NativeSessionState::Failed){
            recovery_requested.store(false,std::memory_order_release);
            if(recovery.load()!=Recovery::Inactive)RecoveryHint(Recovery::Inactive,{});return;
        }
        if(!completed){RecoveryHint(Recovery::RestartRequired,"Online connection stopped during an unfinished update. Restart Beetleworx if a safe return does not become available.");return;}
        auto* gameplay=CurrentNativeRoomGameplay();if(!gameplay)return;
        if(const auto reason=gameplay->LeaveBoundaryBlocker(base)){
            recovery_requested.store(false,std::memory_order_release);
            RecoveryHint(Recovery::RestartRequired,std::string("Cannot safely return: ")+reason+". Restart Beetleworx; your offline save is retained.");
            if(private_auto){REXLOG_ERROR("Native Room title return rejected: {}",reason);leave.phase=Phase::Failed;}return;
        }
        if(!private_auto && !recovery_requested.load(std::memory_order_acquire)){
            RecoveryHint(Recovery::Available,"Online connection lost. Return to the title menu to restore your offline save and start another lobby.");return;
        }
        recovery_requested.store(false,std::memory_order_release);
        RecoveryHint(Recovery::Returning,"Returning to title and restoring your offline save...");
        // This is the original application's outer virtual update, not a Lua
        // callback or a physics/task event. Queue the normal native transition.
        if(ctx.lr!=0x83075AD4 || !Readable(base,0x833EFA38,4)){Fail("unexpected native application boundary");return;}
        const auto app=PPC_LOAD_U32(0x833EFA38);
        if(ctx.r3.u32!=app || !Readable(base,app,0x209B0) || PPC_LOAD_U8(app+0x1066C)){
            Fail("native application is unavailable or already changing levels");return;}
        Caller caller(ctx,base);if(!caller.memory){Fail("native scratch stack unavailable");return;}
        leave.actors=TakeNativeRoomActorsForTitleLeave(ctx,base);
        if(!leave.actors){Fail("could not prepare original camera/reference unload");return;}
        render_retired.store(true,std::memory_order_release);
        leave.thread=GetCurrentThreadId();leave.load=em2::CurrentNativeLoadGeneration();
        leave.deadline=Clock::now()+std::chrono::seconds(60);leave.phase=Phase::Loading;
        em2::CancelQueuedGameLua();ReleaseNativeRoomSession();
        constexpr char destination[]="mainmenu.start";
        std::memcpy(base+caller.text,destination,sizeof(destination));
        ctx.r3.u64=app;ctx.r4.u64=caller.text;__imp__sub_829DA2C0(ctx,base);
        if(!ctx.r3.u32 || !PPC_LOAD_U8(app+0x1066C)){Fail("original title transition was rejected");return;}
        REXLOG_INFO("Native Room title return: normal original transition queued; old cameras retained until completed load");
        return;
    }
    if(leave.thread!=GetCurrentThreadId())return;
    if(Clock::now()>=leave.deadline){Fail("native title return deadline exceeded");return;}
    if(!completed)return;
    if(leave.phase==Phase::Loading && leave.loaded){
        if(!leave.actors->FinishWorldUnload(ctx,base)){Fail("could not finalize retired cameras");return;}
        leave.actors.reset();
        if(!BeginNativeOnlineTitleRestore()){Fail("original storage restore could not start");return;}
        leave.phase=Phase::Refreshing;
        REXLOG_INFO("Native Room title return: retired cameras released; waiting for original save refresh");
    }
    if(leave.phase==Phase::Refreshing){
        const auto status=NativeOnlineTitleRestoreStatus();
        if(status==NativeTitleRestoreState::Failed){Fail("original save refresh failed");return;}
        if(status==NativeTitleRestoreState::Ready){leave.phase=Phase::Ready;render_retired.store(false,std::memory_order_release);RecoveryHint(Recovery::Inactive,{});REXLOG_INFO("Native Room title return complete: original world unloaded, controls retired, original storage refreshed");}
    }
}
}

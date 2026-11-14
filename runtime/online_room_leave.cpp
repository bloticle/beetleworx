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

PPC_EXTERN_FUNC(__imp__sub_829DA2C0);
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DEFINE_BOOL(em2_probe_room_return_title,false,"Debug","Private native lobby QA only: return a stopped completed world to title and refresh original storage");
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
void Fail(const char* why){leave.phase=Phase::Failed;halted.store(true,std::memory_order_release);render_retired.store(true,std::memory_order_release);REXLOG_ERROR("Native Room title return failed: {}; world remains halted",why);}
}
bool NativeRoomTitleLeaveHalted()noexcept{return halted.load(std::memory_order_acquire);}
bool NativeRoomTitleLeaveRenderRetired()noexcept{return render_retired.load(std::memory_order_acquire);}
void NotifyNativeRoomTitleLeaveLoaded(std::string_view level){
    if(leave.phase==Phase::Loading && leave.thread==GetCurrentThreadId() && level=="mainmenu.start" &&
       em2::CurrentNativeLoadGeneration()!=leave.load){leave.loaded=true;REXLOG_INFO("Native Room title return: original title load completed");}
}
void TickNativeRoomTitleLeave(PPCContext& ctx,uint8_t* base,bool completed){
    Environment environment;
    if(!REXCVAR_GET(em2_probe_room_return_title) || !REXCVAR_GET(em2_private_profile) || NativeRoomLoadsCancelled())return;
    if(leave.phase==Phase::Idle && !NativeOnlineMenuQaActive())return;
    if(leave.phase==Phase::Ready && NativeRoomActive() && NativeRoomStatus().state!=NativeSessionState::Failed)leave=Leave{};
    if(leave.phase==Phase::Ready || leave.phase==Phase::Failed)return;
    if(leave.phase==Phase::Idle){
        const auto tick=REXCVAR_GET(em2_probe_room_disconnect_tick);
        if(!disconnect_injected && tick!=0xFFFFFFFFu && completed && NativeOnlineMenuQaActive() &&
           NativeRoomActive() && NativeRoomStatus().state==NativeSessionState::Attached){
            auto* game=CurrentNativeRoomGameplay();auto* room=PrivateNativeMenuRoom();
            const auto finished=game?game->last_native_completed_tick():std::nullopt;
            if(room && finished && *finished>=tick && !game->LeaveBoundaryBlocker(base)){
                disconnect_injected=true;room->Disconnect();
                REXLOG_INFO("Private native Room completed disconnect injected: tick {}; no process closed",*finished);
                return;
            }
        }
        if(!NativeRoomActive() || NativeRoomStatus().state!=NativeSessionState::Failed || !completed)return;
        auto* gameplay=CurrentNativeRoomGameplay();if(!gameplay)return;
        if(const auto reason=gameplay->LeaveBoundaryBlocker(base)){REXLOG_ERROR("Native Room title return rejected: {}",reason);leave.phase=Phase::Failed;return;}
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
        if(status==NativeTitleRestoreState::Ready){leave.phase=Phase::Ready;render_retired.store(false,std::memory_order_release);REXLOG_INFO("Native Room title return complete: original world unloaded, controls retired, original storage refreshed");}
    }
}
}

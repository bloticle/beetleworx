#include "online_room_gameplay.h"
#include "online_cutscene_input.h"
#include "online_cutscene_skip.h"
#include "simulation.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>

PPC_EXTERN_FUNC(__imp__sub_8283BB58);
PPC_EXTERN_FUNC(__imp__sub_8283C210);
PPC_EXTERN_FUNC(__imp__sub_8333AC20);
PPC_EXTERN_FUNC(__imp__sub_8283B5C0);

namespace {
struct MovieClock {
    em2::net::NativeRoomGameplay* game=nullptr;
    PPCContext* context=nullptr;
    uint8_t* base=nullptr;
    uint32_t manager=0;
    uint64_t tick=0;
};
thread_local MovieClock* active=nullptr;
bool Readable(uint8_t* base,uint32_t address,size_t size){
    return base && address>=4096 && uint64_t(address)+size<=0x100000000ull && em2::GuestReadBatch::Readable(base+address,size);
}
// Original callers using the Bink playback start/pause/frame scheduling fields
// 1136/1140/1144/1176. Decode/open/performance timers retain their native clock.
bool PlaybackCaller(uint32_t caller){
    switch(caller){
    case 0x83335D88:case 0x83335DB0:case 0x833360AC:
    case 0x833365A4:case 0x833365E4:case 0x83336714:
    case 0x833367D4:case 0x833368DC:case 0x83339908:return true;
    default:return false;
    }
}
uint32_t Milliseconds(uint64_t tick){
    // Native Bink time is a wrapping uint32 millisecond counter. A fixed
    // nonzero origin retains ordinary start sentinels and fractional 30 Hz.
    return 0x10000000u+uint32_t(tick/30)*1000u+uint32_t(tick%30)*1000u/30u;
}

void Dispatch(PPCContext& ctx,uint8_t* base,PPCFunc* original){
    auto* game=em2::net::CurrentNativeRoomGameplay();uint64_t tick=0;
    const auto manager=ctx.r3.u32;
    if(!game || !game->ReadMovieClock(manager,ctx,base,tick)){original(ctx,base);return;}
    MovieClock clock{game,&ctx,base,manager,tick};auto* previous=active;
    struct Restore {MovieClock* previous;~Restore(){active=previous;}}restore{previous};
    active=&clock;original(ctx,base);
}
}

bool em2::net::TestNativeMovieClock(){
    unsigned count=0;auto check=[&](bool ok,const char* why){++count;if(!ok)REXLOG_ERROR("Native movie clock: {}",why);return ok;};
    for(const auto tick:{uint64_t(0),uint64_t(1),uint64_t(2),uint64_t(3),uint64_t(29),uint64_t(30),uint64_t(31),
                        uint64_t(128849018),uint64_t(128849019),UINT64_MAX-1,UINT64_MAX}){
        const auto oracle=0x10000000u+uint32_t(tick/3)*100u+uint32_t(tick%3)*100u/3u;
        if(!check(Milliseconds(tick)==oracle,"canonical time agrees with independently reduced fraction across 64-bit tick and 32-bit timer wraps"))return false;
    }
    for(unsigned tick=0;tick<90;++tick){const auto delta=Milliseconds(tick+1)-Milliseconds(tick);
        if(!check((delta==33 || delta==34) && Milliseconds(tick+30)-Milliseconds(tick)==1000,"fractional scheduling retains exactly one second per 30 ticks"))return false;}
    for(const auto caller:{0x83335D88u,0x83335DB0u,0x833360ACu,0x833365A4u,0x833365E4u,0x83336714u,0x833367D4u,0x833368DCu,0x83339908u})
        if(!check(PlaybackCaller(caller),"audited native playback timing caller admitted"))return false;
    for(const auto caller:{0u,0x833365A0u,0x83335A80u,0x83337920u,0x83339530u,0x8333AC30u})
        if(!check(!PlaybackCaller(caller),"unrelated decode/performance/busy-wait and forged return sites keep native time"))return false;
    REXLOG_INFO("Native movie clock passed {} fraction/wrap/caller checks; real multi-client playback remains a separate test",count);return true;
}

bool em2::net::NativeMovieSkipTimerOwned(uint32_t timer,uint32_t caller,PPCContext& ctx,uint8_t* base){
    if(caller!=0x82B1DD88 && caller!=0x82B1E070)return false;
    if(caller==0x82B1E070)return NativeMovieAcceptTimerOwned(timer,caller,ctx,base);
    auto* game=CurrentNativeRoomGameplay();uint64_t tick=0;
    if(!game || !Readable(base,0x833DCCB8,4) || !game->ReadMovieClock(PPC_LOAD_U32(0x833DCCB8),ctx,base,tick))return false;
    NativeGameOverState state;uint32_t mode=0,profile=0,object=0,handler=0;
    if(!ReadNativeControlState(ctx,base,state,mode) || mode!=2 ||
       !ReadNativeControlTarget(ctx,base,state.registry,2,profile,object,handler) || handler!=0x82B1DF70 || timer!=object+32)return false;
    uint64_t agreed=0;return caller==0x82B1E070 || (em2::CurrentAgreedSimulationTick(agreed) && agreed==tick);
}

bool em2::net::ServiceNativeDirectMovieSkip(uint32_t registry,PPCContext& ctx,uint8_t* base){
    uint32_t profile=0,object=0,handler=0;
    if(!ReadNativeControlTarget(ctx,base,registry,1,profile,object,handler) || handler!=0x82B1EDF8)return false;
    float elapsed=0;if(!NativeDirectMovieSkipReady(object,ctx,base,elapsed))return true;
    auto* game=CurrentNativeRoomGameplay();uint64_t tick=0;
    const auto manager=Readable(base,0x833DCCB8,4)?PPC_LOAD_U32(0x833DCCB8):0;
    if(!game || !Readable(base,manager,132) || !game->ReadMovieClock(manager,ctx,base,tick))return false;
    // This runs once at the owned agreed-delivery boundary. Original movie
    // update/exit will observe the stop and restore controls themselves.
    const auto saved=ctx;const auto csr=_mm_getcsr();const auto error=GetLastError();ctx.r3.u64=manager;
    __imp__sub_8283B5C0(ctx,base);ctx=saved;_mm_setcsr(csr);SetLastError(error);
    REXLOG_INFO("Native Room direct movie held skip: tick {} object {:08X} elapsed {}; original movie stop, native completion follows",tick,object,elapsed);
    return true;
}
PPC_FUNC_IMPL(sub_8283BB58){Dispatch(ctx,base,__imp__sub_8283BB58);}
PPC_FUNC_IMPL(sub_8283C210){Dispatch(ctx,base,__imp__sub_8283C210);}
PPC_FUNC_IMPL(sub_8333AC20){
    const auto caller=uint32_t(ctx.lr),handle=ctx.r31.u32;
    __imp__sub_8333AC20(ctx,base);
    struct Host {DWORD error=GetLastError();unsigned csr=_mm_getcsr();~Host(){SetLastError(error);_mm_setcsr(csr);}}host;
    auto* clock=active;
    if(!clock || clock->context!=&ctx || clock->base!=base || !PlaybackCaller(caller))return;
    if(em2::net::CurrentNativeRoomGameplay()!=clock->game)return;
    const auto manager=clock->manager;
    if(!Readable(base,manager,100))return;
    const auto slots=PPC_LOAD_U32(manager+96);
    if(!Readable(base,slots,568))return;
    const auto index=PPC_LOAD_U32(slots+560);
    if(index>1 || !Readable(base,handle,1232) || PPC_LOAD_U32(slots+index*280)!=handle)return;
    uint64_t tick=0;
    if(!clock->game->ReadMovieClock(manager,ctx,base,tick) || tick!=clock->tick){
        REXLOG_ERROR("Native Room movie playback clock lost its owned native tick");
        clock->game->ObservationFailed();return;
    }
    ctx.r3.u64=Milliseconds(tick);
    static thread_local uint8_t* logged_base=nullptr;
    static thread_local uint32_t logged_handle=0;
    if(logged_base!=base || logged_handle!=handle){
        logged_base=base;logged_handle=handle;
        REXLOG_INFO("Native Room movie playback clock admitted: tick {} Bink {:08X}; agreed media time, original decoder and performance timers retained",tick,handle);
    }
}

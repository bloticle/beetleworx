#include "online_room_gameplay.h"
#include "online_cutscene_input.h"
#include "guest_read_batch.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>

PPC_EXTERN_FUNC(__imp__sub_8283BB58);
PPC_EXTERN_FUNC(__imp__sub_8283C210);
PPC_EXTERN_FUNC(__imp__sub_8333AC20);

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

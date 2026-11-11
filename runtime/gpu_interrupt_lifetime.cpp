#include "gpu_interrupt_lifetime.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <immintrin.h>
#include <mutex>
#include <semaphore>
#include <shared_mutex>
#include <stdexcept>
#include <thread>

namespace {
class InterruptLifetime {
    std::shared_mutex mutex;
    bool closed=true;
public:
    void Open(){std::unique_lock lock(mutex);closed=false;}
    void Close(){std::unique_lock lock(mutex);closed=true;}
    template<class Callback> bool Invoke(PPCContext& ctx,uint8_t* base,Callback&& callback){
        std::shared_lock lock(mutex);if(closed)return false;
        callback(ctx,base);return true;
    }
};
InterruptLifetime lifetime;
}
void em2::OpenGuestGPUInterrupts(){lifetime.Open();}
void em2::CloseGuestGPUInterrupts(){lifetime.Close();}
void em2::DispatchGuestGPUInterrupt(PPCContext& ctx,uint8_t* base,void(*callback)(PPCContext&,uint8_t*)){lifetime.Invoke(ctx,base,callback);}

bool em2::TestGPUInterruptLifetime(){
    InterruptLifetime gate;PPCContext ctx{};ctx.r3.u64=11;
    std::array<uint8_t,16> memory{};const auto untouched=ctx;
    unsigned calls=0;
    auto callback=[&](PPCContext& c,uint8_t* bytes){++calls;c.r3.u64=23;bytes[4]=7;};
    if(gate.Invoke(ctx,memory.data(),callback) || calls || std::memcmp(&ctx,&untouched,sizeof(ctx)))return false;
    gate.Open();if(!gate.Invoke(ctx,memory.data(),callback) || calls!=1 || ctx.r3.u64!=23 || memory[4]!=7)return false;
    std::binary_semaphore entered(0),release(0),closing(0),closed(0);
    std::thread worker([&]{gate.Invoke(ctx,memory.data(),[&](PPCContext&,uint8_t*){entered.release();release.acquire();});});
    entered.acquire();
    std::thread closer([&]{closing.release();gate.Close();closed.release();});closing.acquire();
    const bool prematurely_closed=closed.try_acquire_for(std::chrono::milliseconds(40));
    release.release();worker.join();closer.join();
    if(prematurely_closed || !closed.try_acquire())return false;
    const auto after=ctx;const auto bytes=memory;const auto csr=_mm_getcsr();
    if(gate.Invoke(ctx,memory.data(),callback) || calls!=1 || std::memcmp(&ctx,&after,sizeof(ctx)) || memory!=bytes || _mm_getcsr()!=csr)return false;
    gate.Open();bool threw=false;
    try{gate.Invoke(ctx,memory.data(),[](PPCContext&,uint8_t*){throw std::runtime_error("interrupt fixture");});}catch(const std::runtime_error&){threw=true;}
    gate.Close();if(!threw)return false;
    REXLOG_INFO("GPU interrupt lifetime passed: closed entry bypass, forwarding, in-flight drain before title teardown, late callback suppression and exception unlock; synthetic callbacks, no GPU execution");
    return true;
}

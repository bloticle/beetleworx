#include "gpu_command_profile.h"
#include "gpu_constants.h"
#include "gpu_draw_profile.h"
#include "stencil_transfer.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <immintrin.h>
#include <thread>

namespace {
struct FpGuard {unsigned csr=_mm_getcsr();~FpGuard(){_mm_setcsr(csr);}};
uint64_t Elapsed(uint64_t end,uint64_t start){return end>=start?end-start:0;}
uint64_t ClockNow(void*) noexcept {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
uint64_t Batches(void*) noexcept{return em2::GpuConstantBatchCount();}
void Report(const em2::GpuCommandWindow& window,void*) {
    REXLOG_INFO("Graphics command CPU wall profile: {:.3f}s window, {:.3f} ms root dispatch time, {} bulk constant packets, counter reset {} (includes waits/profiling overhead; not GPU execution time, draw count or total worker CPU)",
        window.elapsed_ns/1e9,window.root_ns/1e6,window.bulk_packets,window.counter_reset);
    constexpr std::array names={"draw dispatch","copy dispatch","swap dispatch"};
    for(unsigned i=0;i<window.commands.size();++i) {
        const auto& s=window.commands[i];if(!s.calls)continue;
        REXLOG_INFO("Graphics {}: {} calls, {} false returns, {} aborted, {:.3f} ms inclusive, {:.3f} ms self excluding measured children; {:.3f}/{:.3f} ms/call mean/max",
            names[i],s.calls,s.false_results,s.aborted,s.inclusive_ns/1e6,s.self_ns/1e6,s.inclusive_ns/double(s.calls)/1e6,s.max_ns/1e6);
    }
}
}
em2::GpuCommandProfiler::GpuCommandProfiler():GpuCommandProfiler(ClockNow,::Batches,::Report,nullptr){}
uint64_t em2::GpuCommandProfiler::Now()const noexcept {FpGuard fp;return clock_(state_);}
uint64_t em2::GpuCommandProfiler::Batches()const noexcept {FpGuard fp;return counter_(state_);}
em2::GpuCommandProfiler::Call::Call(GpuCommandProfiler& owner,GpuCommandKind kind):owner_(owner),kind_(kind),parent_(owner.active_),started_(owner.Now()) {
    if(!owner_.window_.open){owner_.window_.open=true;owner_.window_.started=started_;owner_.window_.batch_start=owner_.Batches();}
    owner_.active_=this;
}
uint64_t em2::GpuCommandProfiler::Call::Finish(bool result,bool aborted) {
    const auto now=owner_.Now(),duration=Elapsed(now,started_);
    owner_.active_=parent_;finished_=true;
    auto& sample=owner_.window_.commands[unsigned(kind_)];++sample.calls;sample.aborted+=aborted;sample.false_results+=!aborted && !result;
    sample.inclusive_ns+=duration;sample.self_ns+=duration>children_?duration-children_:0;sample.max_ns=std::max(sample.max_ns,duration);
    if(parent_)parent_->children_+=duration;else owner_.window_.root_ns+=duration;
    return now;
}
em2::GpuCommandProfiler::Call::~Call(){if(!finished_)Finish(false,true);}
void em2::GpuCommandProfiler::Call::End(bool result) noexcept {
    if(finished_)return;
    const auto now=Finish(result,false);owner_.MaybeReport(now);
}
void em2::GpuCommandProfiler::MaybeReport(uint64_t now) noexcept {
    if(active_ || !window_.open || Elapsed(now,window_.started)<5000000000ull)return;
    FpGuard fp;auto snapshot=window_;const auto batches=Batches();
    snapshot.elapsed_ns=Elapsed(now,snapshot.started);snapshot.counter_reset=batches<snapshot.batch_start;
    snapshot.bulk_packets=snapshot.counter_reset?0:batches-snapshot.batch_start;
    // Clear before invoking an external sink; nested capture starts a new window.
    window_={};window_.open=true;window_.started=now;window_.batch_start=batches;
    // Diagnostic sink failures must not replace a native return or exception.
    try{report_(snapshot,state_);}catch(...){}
}

bool em2::TestGpuCommandProfiling() {
    if(!TestGpuCopyClassification())return false;
    if(!TestStencilCopyCoalescing())return false;
    FpGuard restore;
    struct SinkFailure{};
    struct Probe {uint64_t now=0,batches=10,reports=0;bool throw_report=false;GpuCommandWindow captured;};Probe probe;
    auto clock=+[](void* state) noexcept {auto& p=*static_cast<Probe*>(state);_mm_setcsr(_mm_getcsr()^0x2000);return p.now;};
    auto counter=+[](void* state) noexcept {auto& p=*static_cast<Probe*>(state);_mm_setcsr(_mm_getcsr()^0x4000);return p.batches;};
    auto report=+[](const GpuCommandWindow& window,void* state){auto& p=*static_cast<Probe*>(state);++p.reports;p.captured=window;_mm_setcsr(_mm_getcsr()^0x4020);if(p.throw_report)throw SinkFailure{};};
    auto check=[](bool ok,const char* text){if(!ok)REXLOG_ERROR("Graphics command profiling test failed: {}",text);return ok;};
    GpuCommandProfiler profiler(clock,counter,report,&probe);
    PPCContext expected{},actual{};std::array<uint8_t,32> expected_memory{},memory{};
    auto work=[](PPCContext& c,uint8_t* bytes){c.r3.u64=0x0123456789ABCDEFull;c.r7.u64=0x8000000080000000ull;bytes[5]=0xA5;_mm_setcsr((_mm_getcsr()&~0x6000u)|0x6000u|0x20u);return false;};
    work(expected,expected_memory.data());const auto expected_csr=_mm_getcsr();
    bool result=true;
    {
        GpuCommandProfiler::Call draw(profiler,GpuCommandKind::Draw);probe.now=20;
        {GpuCommandProfiler::Call copy(profiler,GpuCommandKind::Copy);result=work(actual,memory.data());probe.now=50;copy.End(result);}
        probe.now=100;draw.End(true);
    }
    if(!check(!result && !std::memcmp(&expected,&actual,sizeof(actual)) && memory==expected_memory && _mm_getcsr()==expected_csr,"native callback return, full context/memory and post-callback MXCSR") ||
       !check(profiler.window().commands[0].inclusive_ns==100 && profiler.window().commands[0].self_ns==70 && profiler.window().commands[1].inclusive_ns==30 &&
              profiler.window().commands[1].self_ns==30 && profiler.window().commands[1].false_results==1 && profiler.window().root_ns==100 && !profiler.active(),"nested copy time is not double-counted as root/self time"))return false;
    struct ProbeException{};probe.now=200;
    try {GpuCommandProfiler::Call draw(profiler,GpuCommandKind::Draw);probe.now=210;throw ProbeException{};}catch(const ProbeException&){}
    if(!check(!profiler.active() && profiler.window().commands[0].aborted==1 && !probe.reports,"exception cleanup records an aborted call without destructor reporting"))return false;
    probe.now=5000000000ull;
    {GpuCommandProfiler::Call swap(profiler,GpuCommandKind::Swap);probe.now+=80;probe.batches=37;swap.End();}
    if(!check(probe.reports==1 && probe.captured.elapsed_ns==5000000080ull && probe.captured.bulk_packets==27 && probe.captured.root_ns==190 &&
              probe.captured.commands[0].calls==2 && probe.captured.commands[1].calls==1 && probe.captured.commands[2].calls==1 &&
              profiler.window().root_ns==0 && profiler.window().commands==GpuCommandWindow{}.commands && _mm_getcsr()==expected_csr,"report cadence, packet delta, full reset and FP-changing report sink"))return false;
    // A reset/recreated GPU worker must not yield an enormous unsigned delta.
    probe.now+=5000000000ull;probe.batches=2;
    {GpuCommandProfiler::Call copy(profiler,GpuCommandKind::Copy);probe.now+=10;copy.End(false);}
    if(!check(probe.reports==2 && probe.captured.counter_reset && !probe.captured.bulk_packets && probe.captured.commands[1].false_results==1,"packet-counter reset handling"))return false;
    probe.throw_report=true;probe.now+=5000000000ull;
    {GpuCommandProfiler::Call draw(profiler,GpuCommandKind::Draw);probe.now+=15;draw.End(false);}
    if(!check(probe.reports==3 && !profiler.active() && !profiler.window().root_ns && _mm_getcsr()==expected_csr,"throwing diagnostic sink is contained, resets counters and preserves native FP state"))return false;
    const auto main_window=profiler.window();bool isolated=false;
    std::thread worker([&] {
        Probe own;GpuCommandProfiler another(clock,counter,report,&own);
        {GpuCommandProfiler::Call draw(another,GpuCommandKind::Draw);own.now=75;draw.End();}
        isolated=another.window().commands[0].calls==1 && another.window().root_ns==75 && !another.active();
    });worker.join();
    if(!check(isolated && profiler.window()==main_window,"processor/worker instances have independent counters"))return false;
    REXLOG_INFO("Graphics command profiling tests passed: nested draw/copy/swap accounting, false returns, context/memory/MXCSR parity, exception cleanup, report reset, packet deltas/reset and instance isolation (synthetic callbacks; no GPU execution timing)");
    return true;
}

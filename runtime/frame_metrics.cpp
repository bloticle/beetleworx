#include "ppc_context.h"
#include "frame_metrics.h"
#include "pc_settings.h"
#include "simulation.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <bit>
#include <immintrin.h>
#include <limits>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <windows.h>

namespace em2 {
std::atomic<uint64_t> present_count{0};
std::atomic<uint64_t> maximum_present_gap_us{0};
std::atomic<uint64_t> maximum_native_present_us{0};
}

PPC_EXTERN_FUNC(__imp__sub_826B9890);
PPC_EXTERN_FUNC(__imp__sub_83074A88);
REXCVAR_DECLARE(bool, em2_trace_motion);

namespace {
struct PresentIntervalWindow {
    uint64_t count=0,total=0,minimum=UINT64_MAX,maximum=0,overflow=0;
    std::array<uint32_t,512> bins{};
    void Add(uint64_t us) {
        ++count;total+=us;minimum=std::min(minimum,us);maximum=std::max(maximum,us);
        const auto bin=us/250;overflow+=bin>=bins.size();++bins[std::min<uint64_t>(bin,bins.size()-1)];
    }
    uint64_t Percentile(unsigned percent) const {
        const auto target=(count*percent+99)/100;uint64_t sum=0;
        for(size_t i=0;i<bins.size();++i)if((sum+=bins[i])>=target)return i*250;
        return 0;
    }
};
void ObservePresentTiming(uint64_t entry,uint64_t completion,uint64_t ticks) noexcept {
    struct Window {
        uint64_t entry=0,completion=0,ticks=0,started=0;
        int cap=-1;
        std::array<PresentIntervalWindow,2> submissions{},completions{},native_calls{};
    };
    static thread_local Window window;
    if(!REXCVAR_GET(em2_trace_motion)){if(window.started)window={};return;}
    const auto cap=em2::FrameLimit();
    if(!window.started || window.cap!=cap || ticks<window.ticks || entry<window.entry || completion<window.completion) {
        window={};window.entry=entry;window.completion=completion;window.ticks=ticks;window.started=completion;window.cap=cap;return;
    }
    const unsigned stepped=ticks!=window.ticks;
    window.submissions[stepped].Add(entry-window.entry);
    window.completions[stepped].Add(completion-window.completion);
    window.native_calls[stepped].Add(completion-entry);
    window.entry=entry;window.completion=completion;window.ticks=ticks;
    if(completion-window.started<5000000)return;
    // Accumulation is integer-only; preserve the guest FP environment while
    // formatting the report. This instrumentation never changes pacing waits.
    struct RestoreMath {unsigned csr=_mm_getcsr();~RestoreMath(){_mm_setcsr(csr);}} restore;
    try {
        auto report=[&](const char* phase,unsigned tick_group,const PresentIntervalWindow& sample) {
            if(!sample.count)return;
            REXLOG_INFO("CPU present cadence {} cap {} ticks-since-prior-present {}: {} samples, mean/min/max {} / {} / {} us, p50/p95 bins {} / {} us, >=128ms {} (250us histogram bins; CPU submission/completion only, not GPU display delivery)",
                phase,cap,tick_group?">=1":"0",sample.count,sample.total/sample.count,sample.minimum,sample.maximum,
                sample.Percentile(50),sample.Percentile(95),sample.overflow);
        };
        for(unsigned group=0;group<2;++group) {
            report("submission interval",group,window.submissions[group]);
            report("completion interval",group,window.completions[group]);
            report("native call duration",group,window.native_calls[group]);
        }
    }catch(...){}
    window.submissions={};window.completions={};window.native_calls={};window.started=completion;
}
}

PPC_FUNC_IMPL(sub_83074A88) {
    // This application's frame-gate vtable slot (0x8210A0B8 + 140) points here.
    // Its +116 float is the interval multiplied by QueryPerformanceFrequency
    // before the elapsed-counter comparison. Disable only that interval test;
    // retain the original elapsed-time accumulation and simulation delta.
    const auto application=ctx.r3.u32;
    const auto interval=PPC_LOAD_U32(application+116);
    thread_local bool reported=false;
    if(!reported) {
        REXLOG_INFO("Removing native frame interval: {:.6f} seconds; PC presentation limiter is active",std::bit_cast<float>(interval));
        reported=true;
    }
    PPC_STORE_U32(application+116,0);
    __imp__sub_83074A88(ctx,base);
}

PPC_FUNC_IMPL(sub_826B9890) {
    thread_local int reported_cap = -1;
    thread_local uint64_t last_report = 0;
    const auto cap = em2::FrameLimit();
    const auto report_tick = GetTickCount64();
    if (reported_cap != cap || report_tick-last_report>=5000) {
        reported_cap = cap;
        last_report = report_tick;
        std::string callers;
        auto sp=ctx.r1.u32;
        for(int i=0;i<12 && sp;++i) {
            MEMORY_BASIC_INFORMATION memory{};
            if(!VirtualQuery(PPC_RAW_ADDR(sp),&memory,sizeof(memory)) || memory.State!=MEM_COMMIT || (memory.Protect&(PAGE_NOACCESS|PAGE_GUARD))) break;
            const auto next=PPC_LOAD_U32(sp);
            if(next<=sp || next-sp>4*1024*1024) break;
            if(!VirtualQuery(PPC_RAW_ADDR(next-8),&memory,sizeof(memory)) || memory.State!=MEM_COMMIT || (memory.Protect&(PAGE_NOACCESS|PAGE_GUARD))) break;
            callers+=fmt::format(" {:08X}",PPC_LOAD_U32(next-8));
            sp=next;
        }
        REXLOG_INFO("Guest present callers, cap {}:{}", cap, callers);
    }
    em2::PaceGuestPresentation();
    const auto started=std::chrono::steady_clock::now();
    __imp__sub_826B9890(ctx, base);
    const auto native_us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count();
    auto native_max=em2::maximum_native_present_us.load(std::memory_order_relaxed);
    while(uint64_t(native_us)>native_max && !em2::maximum_native_present_us.compare_exchange_weak(native_max,native_us,std::memory_order_relaxed)) {}
    using clock = std::chrono::steady_clock;
    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        clock::now().time_since_epoch()).count();
    ObservePresentTiming(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(started.time_since_epoch()).count()),
                         uint64_t(now),em2::simulation_steps.load(std::memory_order_relaxed));
    static std::atomic<uint64_t> previous{0};
    const auto last = previous.exchange(now, std::memory_order_relaxed);
    if (last && uint64_t(now) >= last) {
        const auto gap = uint64_t(now) - last;
        auto maximum = em2::maximum_present_gap_us.load(std::memory_order_relaxed);
        while (gap > maximum && !em2::maximum_present_gap_us.compare_exchange_weak(
            maximum, gap, std::memory_order_relaxed)) {}
    }
    em2::present_count.fetch_add(1, std::memory_order_relaxed);
}

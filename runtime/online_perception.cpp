#include "online_perception.h"
#include "online_input_clock.h"
#include "online_selector.h"
#include "online_runtime.h"
#include "simulation.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <immintrin.h>
#include <windows.h>
#include <bit>
#include <cstring>
#include <vector>

PPC_EXTERN_FUNC(__imp__sub_831C1558);
PPC_EXTERN_FUNC(__imp__sub_828D1408);
PPC_EXTERN_FUNC(__imp__sub_829505C0);
namespace {
constexpr uint32_t kPerceptionElapsedReturn=0x829507A4;
constexpr uint32_t kFrameBudget=0x8339A4D8;
void StartupPerception(PPCContext& ctx,uint8_t* base,bool hold,PPCFunc* original){
    if(!hold){original(ctx,base);return;}
    // Use the audited retail disabled path while the transferred world is
    // loading. Do not consume sensing work/RNG before a shared input tick exists.
    const auto owner=ctx.r3.u32;const auto enabled=PPC_LOAD_U8(owner+36);
    struct Restore {uint8_t* base;uint32_t owner;uint8_t value;~Restore(){PPC_STORE_U8(owner+36,value);}}restore{base,owner,enabled};
    PPC_STORE_U8(owner+36,0);original(ctx,base);
}

void StartupSelector(PPCContext& ctx,uint8_t* base,bool hold,PPCFunc* original){
    if(!hold){original(ctx,base);return;}
    // Registration (828D1018) also invokes this selector directly. Holding only
    // the manager update leaves those pre-world RNG draws uncontrolled.
    const auto budget=ctx.r4.u32;const auto bits=PPC_LOAD_U32(budget);
    struct Restore {uint8_t* base;uint32_t budget,value;~Restore(){PPC_STORE_U32(budget,value);}}restore{base,budget,bits};
    PPC_STORE_U32(budget,0);original(ctx,base);
}
void Elapsed(PPCContext& ctx,uint8_t* base,bool synchronized,PPCFunc* original){
    const auto caller=ctx.lr;original(ctx,base);
    if(!synchronized || caller!=kPerceptionElapsedReturn)return;
    const auto bits=PPC_LOAD_U32(kFrameBudget),exponent=bits&0x7F800000;
    if((bits&0x80000000) || !exponent || exponent==0x7F800000)return;
    // 828D1408 supplies this authored frame budget. 829505C0 clamps it by
    // each observer's budget, then charges 831C1558 after one selector callback.
    // Real elapsed CPU time makes its persistent iterator diverge across peers.
    // Charge 1/16 frame budget per callback instead, retaining the original
    // iterator, timer resets, per-observer caps, due intervals and actual work.
    // Scaling a finite normal float by this power of two is exact in double.
    ctx.f1.f64=double(std::bit_cast<float>(bits))*0x1p-4;
}
}
PPC_FUNC_IMPL(sub_828D1408){StartupPerception(ctx,base,em2::net::SynchronizedSessionActive() && !em2::InAgreedSimulationTick(),__imp__sub_828D1408);}
PPC_FUNC_IMPL(sub_829505C0){const bool shared=em2::net::SynchronizedSessionActive();if(shared && em2::InAgreedSimulationTick())em2::net::RunCanonicalPerception(ctx,base);else StartupSelector(ctx,base,shared,__imp__sub_829505C0);}
PPC_FUNC_IMPL(sub_831C1558){
    if(em2::net::RunAgreedNativeInputClockHelper(ctx,base,__imp__sub_831C1558))return;
    Elapsed(ctx,base,em2::net::SynchronizedSessionActive(),__imp__sub_831C1558);
}

bool em2::net::TestOnlinePerceptionBudget(){
    if(!TestCanonicalPerception())return false;
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));if(!base)return false;
    struct Restore {uint8_t* base;unsigned csr=_mm_getcsr();~Restore(){_mm_setcsr(csr);VirtualFree(base,0,MEM_RELEASE);}}restore{base};
    if(!VirtualAlloc(base,65536,MEM_COMMIT,PAGE_READWRITE) || !VirtualAlloc(base+0x82000000,65536,MEM_COMMIT,PAGE_READWRITE) || !VirtualAlloc(base+0x83390000,65536,MEM_COMMIT,PAGE_READWRITE))return false;
    const auto original=+[](PPCContext& ctx,uint8_t*){ctx.r3.u64=0xABCDE;ctx.r9.u64=0x12345;ctx.f0.f64=55;ctx.f1.f64=123;ctx.f13.f64=99;_mm_setcsr(_mm_getcsr()|0x20);};
    PPCContext input{};input.r1.u64=0x123400;input.lr=kPerceptionElapsedReturn;input.r31.u64=0x778899;input.fpscr.InitHost();
    unsigned cases=0;
    for(bool active:{false,true})for(uint32_t caller:{kPerceptionElapsedReturn,kPerceptionElapsedReturn+4,0u})for(uint32_t bits:{0u,0x80000000u,0x7FC00000u,0x7F800000u,0xBF800000u,1u,0x3A83126Fu,0x3F800000u,0x00800000u,0x7F7FFFFFu}){
        PPC_STORE_U32(kFrameBudget,bits);const auto csr=_mm_getcsr();auto expected=input;expected.lr=caller;auto actual=expected;
        original(expected,base);const auto post_csr=_mm_getcsr();_mm_setcsr(csr);
        const auto exponent=bits&0x7F800000;
        if(active && caller==kPerceptionElapsedReturn && !(bits&0x80000000) && exponent && exponent!=0x7F800000)expected.f1.f64=double(std::bit_cast<float>(bits))*0x1p-4;
        Elapsed(actual,base,active,original);++cases;
        if(std::memcmp(&actual,&expected,sizeof(actual)) || _mm_getcsr()!=post_csr || PPC_LOAD_U32(kFrameBudget)!=bits){REXLOG_ERROR("Online perception budget context/FP/callsite mismatch");return false;}
    }
    // With the same authored budget, variable host execution cost cannot change
    // the stopping point. This exercises real float accumulation/comparison.
    PPC_STORE_U32(kFrameBudget,0x3B03126F); // 0.002 seconds
    const auto budget=std::bit_cast<float>(uint32_t(0x3B03126F));unsigned counts[3]{};
    for(unsigned run=0;run<3;++run){float spent=0;
        while(spent<budget && counts[run]<1000){auto ctx=input;ctx.f1.f64=run+1;
            Elapsed(ctx,base,true,+[](PPCContext& c,uint8_t*){c.f1.f64=c.f1.f64*1000;});
            spent=float(double(spent)+ctx.f1.f64);++counts[run];}
    }
    if(counts[0]<16 || counts[0]>17 || counts[0]!=counts[1] || counts[0]!=counts[2])return false;
    // Real retail disabled implementation, including its stack writes and ABI.
    std::vector<uint8_t> initial(65536,0x69),expected_memory(65536);
    std::memcpy(base,initial.data(),initial.size());PPC_STORE_U8(0x2000+36,0);
    PPCContext expected{};expected.r1.u64=0xF000;expected.r3.u64=0x2000;expected.lr=0x82929F48;expected.r31.u64=0x11223344;expected.f31.f64=9;expected.fpscr.InitHost();
    auto actual=expected;const auto csr=_mm_getcsr();__imp__sub_828D1408(expected,base);const auto expected_csr=_mm_getcsr();
    PPC_STORE_U8(0x2000+36,1);std::memcpy(expected_memory.data(),base,65536);
    std::memcpy(base,initial.data(),initial.size());PPC_STORE_U8(0x2000+36,1);_mm_setcsr(csr);
    StartupPerception(actual,base,true,__imp__sub_828D1408);
    if(std::memcmp(&actual,&expected,sizeof(actual)) || std::memcmp(base,expected_memory.data(),65536) || _mm_getcsr()!=expected_csr){REXLOG_ERROR("Online pre-world perception differs from retail disabled context/memory/FP contract");return false;}
    std::memcpy(base,initial.data(),initial.size());PPC_STORE_U32(0x82002060,0);PPC_STORE_U32(0x3000,0);
    expected={};expected.r1.u64=0xF000;expected.r3.u64=0x2000;expected.r4.u64=0x3000;expected.lr=0x828D114C;expected.fpscr.InitHost();actual=expected;
    const auto selector_csr=_mm_getcsr();__imp__sub_829505C0(expected,base);const auto selector_expected_csr=_mm_getcsr();
    PPC_STORE_U32(0x3000,0x3A83126F);std::memcpy(expected_memory.data(),base,65536);
    std::memcpy(base,initial.data(),initial.size());PPC_STORE_U32(0x3000,0x3A83126F);_mm_setcsr(selector_csr);
    StartupSelector(actual,base,true,__imp__sub_829505C0);
    if(std::memcmp(&actual,&expected,sizeof(actual)) || std::memcmp(base,expected_memory.data(),65536) || _mm_getcsr()!=selector_expected_csr){REXLOG_ERROR("Online pre-world registration selector differs from retail zero-budget context/memory/FP contract");return false;}
    std::memcpy(base,initial.data(),initial.size());PPC_STORE_U32(0x3000,0);
    expected={};expected.r1.u64=0xF000;expected.r3.u64=0x2000;expected.r4.u64=0x3000;expected.lr=0x828D114C;expected.fpscr.InitHost();actual=expected;
    const auto zero_csr=_mm_getcsr();__imp__sub_829505C0(expected,base);const auto zero_expected_csr=_mm_getcsr();std::memcpy(expected_memory.data(),base,65536);
    std::memcpy(base,initial.data(),initial.size());PPC_STORE_U32(0x3000,0);_mm_setcsr(zero_csr);RunCanonicalPerception(actual,base);
    if(std::memcmp(&actual,&expected,sizeof(actual)) || std::memcmp(base,expected_memory.data(),65536) || _mm_getcsr()!=zero_expected_csr){REXLOG_ERROR("Canonical perception zero-budget fast path differs from retail caller/memory/FP contract");return false;}
    REXLOG_INFO("Online perception budget passed {} full-context/FP/callsite/config cases and identical bounded selector quotas across variable synthetic host costs; real native world acceptance separate",cases);return true;
}

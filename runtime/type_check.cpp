#include "ppc_context.h"
#include "type_check.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <immintrin.h>
#include <vector>
#include <windows.h>

PPC_EXTERN_FUNC(__imp__sub_82551560);
PPC_FUNC(EM2LocalTypeCheck);
REXCVAR_DEFINE_BOOL(em2_local_type_checks,true,"CPU","Keep native type-check walk in host registers");
REXCVAR_DEFINE_BOOL(em2_benchmark_type_checks,false,"Debug","Benchmark complete native type checks without launching the game");
PPC_FUNC_IMPL(sub_82551560) {
    if(REXCVAR_GET(em2_local_type_checks)) EM2LocalTypeCheck(ctx,base);
    else __imp__sub_82551560(ctx,base);
}
namespace {
uint32_t thread_local fixture_type=0;
PPCContext thread_local getter_context{};
unsigned thread_local getter_calls=0;
void FixtureGetter(PPCContext& ctx,uint8_t* base) {
    getter_context=ctx;++getter_calls;
    PPC_STORE_U32(0x800,ctx.r3.u32);
    ctx.r3.u64=uint64_t(0xABCD1234)<<32|fixture_type;
    ctx.r9.u64=0xBADC0FFEE;
    ctx.xer.so=!ctx.xer.so;
    ctx.fpscr.enableFlushMode();
}
void BenchmarkGetter(PPCContext& ctx,uint8_t*) {ctx.r3.u64=0x3000;}
}
bool em2::TestNativeTypeCheck() {
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    struct Release {
        uint8_t* base;unsigned csr=_mm_getcsr();bool enabled=REXCVAR_GET(em2_local_type_checks);
        ~Release(){REXCVAR_SET(em2_local_type_checks,enabled);_mm_setcsr(csr);if(base)VirtualFree(base,0,MEM_RELEASE);}
    } release{base};
    constexpr uint32_t bytes=0x10000,object=0x1000,vtable=0x2000,classes=0x3000,stack=0xF000;
    if(!base || !VirtualAlloc(base,bytes,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+PPC_IMAGE_BASE+PPC_IMAGE_SIZE,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
    PPC_LOOKUP_FUNC(base,uint32_t(PPC_CODE_BASE))=FixtureGetter;
    std::vector<uint8_t> before(bytes),expected_memory(bytes);
    uint32_t random=0x82551560;
    auto next=[&](){random=random*1664525+1013904223;return random;};
    unsigned comparisons=0;
    for(unsigned depth:{0u,1u,2u,4u,8u,16u,32u,64u})for(unsigned target=0;target<depth+2;++target)for(unsigned flags=0;flags<4;++flags) {
        for(auto& byte:before)byte=uint8_t(next());
        std::memcpy(base,before.data(),bytes);
        PPC_STORE_U32(object,vtable);PPC_STORE_U32(vtable+8,uint32_t(PPC_CODE_BASE));
        fixture_type=depth?classes:0;
        for(unsigned i=0;i<depth;++i)PPC_STORE_U32(classes+i*8+4,i+1<depth?classes+(i+1)*8:0);
        std::memcpy(before.data(),base,bytes);
        PPCContext initial{};
        for(unsigned i=0;i<32;++i){uint64_t word=uint64_t(next())<<32|next();std::memcpy(reinterpret_cast<uint8_t*>(&initial.r0)+i*8,&word,8);}
        initial.r1.u64=stack;initial.r3.u64=object;initial.r4.u64=target==depth?0:classes+target*8;
        initial.lr=uint64_t(next())<<32|next();initial.xer.so=flags&1;initial.xer.ca=(flags>>1)&1;
        initial.fpscr.InitHost();if(flags&2)initial.fpscr.enableFlushMode();
        const auto csr=_mm_getcsr();auto expected=initial;getter_calls=0;
        __imp__sub_82551560(expected,base);
        const auto expected_csr=_mm_getcsr();const auto expected_getter=getter_context;
        if(getter_calls!=1)return false;
        std::memcpy(expected_memory.data(),base,bytes);
        for(unsigned mode=0;mode<3;++mode) {
            std::memcpy(base,before.data(),bytes);_mm_setcsr(csr);getter_calls=0;
            REXCVAR_SET(em2_local_type_checks,mode!=2);
            auto actual=initial;auto* function=mode?sub_82551560:EM2LocalTypeCheck;function(actual,base);
            if(std::memcmp(&actual,&expected,sizeof(actual)) || std::memcmp(base,expected_memory.data(),bytes) ||
               _mm_getcsr()!=expected_csr || getter_calls!=1 || std::memcmp(&getter_context,&expected_getter,sizeof(getter_context))) {
                REXLOG_ERROR("Native type-check mismatch: depth {}, target {}, flags {}, mode {}",depth,target,flags,mode);return false;
            }
            ++comparisons;
        }
    }
    REXLOG_INFO("Native type checks matched original full context, guest memory, getter entry, single callback and MXCSR in {} comparisons",comparisons);
    if(REXCVAR_GET(em2_benchmark_type_checks)) {
        using Clock=std::chrono::steady_clock;
        struct Affinity {DWORD_PTR old=0;~Affinity(){if(old)SetThreadAffinityMask(GetCurrentThread(),old);}} affinity;
        affinity.old=SetThreadAffinityMask(GetCurrentThread(),DWORD_PTR(1)<<GetCurrentProcessorNumber());
        constexpr unsigned rounds=9,iterations=20000;
        // Match the game's leaf getters: one constant RTTI pointer, no tracing
        // or extra state changes that could obscure the cost of the walk.
        PPC_LOOKUP_FUNC(base,uint32_t(PPC_CODE_BASE))=BenchmarkGetter;
        REXCVAR_SET(em2_local_type_checks,true);
        PPCContext warm{};warm.r1.u64=stack;warm.fpscr.InitHost();
        const auto deadline=Clock::now()+std::chrono::milliseconds(250);
        do{for(unsigned i=0;i<1000;++i){warm.r3.u64=object;warm.r4.u64=classes;sub_82551560(warm,base);}}while(Clock::now()<deadline);
        for(unsigned depth:{1u,2u,4u,8u,16u,32u,64u})for(bool found:{false,true}) {
            fixture_type=classes;
            for(unsigned i=0;i<depth;++i)PPC_STORE_U32(classes+i*8+4,i+1<depth?classes+(i+1)*8:0);
            std::array<std::array<double,rounds>,3> times{};
            const std::array<PPCFunc*,3> functions={__imp__sub_82551560,EM2LocalTypeCheck,sub_82551560};
            for(unsigned round=0;round<rounds;++round)for(unsigned order=0;order<3;++order) {
                const unsigned mode=(round+order)%3;PPCFunc* volatile function=functions[mode];
                PPCContext ctx{};ctx.r1.u64=stack;ctx.fpscr.InitHost();
                const auto start=Clock::now();
                for(unsigned i=0;i<iterations;++i){ctx.r3.u64=object;ctx.r4.u64=found?classes+(depth-1)*8:0;function(ctx,base);}
                times[mode][round]=std::chrono::duration<double,std::nano>(Clock::now()-start).count()/iterations;
            }
            for(auto& mode:times)std::sort(mode.begin(),mode.end());
            REXLOG_INFO("Native type-check benchmark depth {}, found {}: original {:.2f}, local {:.2f}, production {:.2f} ns/call; {:.3f}x complete call (9 rotating rounds, not gameplay FPS)",depth,found,times[0][4],times[1][4],times[2][4],times[0][4]/times[2][4]);
        }
    }
    return true;
}

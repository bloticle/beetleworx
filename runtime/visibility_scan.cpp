#include "visibility_scan.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <immintrin.h>
#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <vector>

PPC_EXTERN_FUNC(__imp__sub_8256F948);
PPC_EXTERN_FUNC(sub_8256F948);
REXCVAR_DEFINE_BOOL(em2_native_visibility_scan,true,"CPU","Batch invisible scene flags without skipping eligible callbacks");
REXCVAR_DEFINE_BOOL(em2_benchmark_visibility_scan,false,"Debug","Benchmark native scene-flag batching in headless regressions");
bool em2::NativeVisibilityScanEnabled(){return REXCVAR_GET(em2_native_visibility_scan);}
int em2::SkipNativeInvisibleBatch(PPCContext& ctx,uint8_t* base) {
    if(ctx.r24.s64<16 || ctx.r23.u64) return false;
    const auto address=ctx.r29.u32;
    const auto end=uint64_t(address)+32;
    if(end>0x100000000ull || PPC_PHYS_HOST_OFFSET(address)!=PPC_PHYS_HOST_OFFSET(uint32_t(end-1)))return false;
    auto* data=reinterpret_cast<__m256i*>(PPC_RAW_ADDR(address));
    const auto flags=_mm256_loadu_si256(data);
    const auto eligible=_mm256_set1_epi16(int16_t(__builtin_bswap16(uint16_t(ctx.r22.u32))));
    // Never batch across an eligible entry. Its callback may change later
    // flags, so each subsequent batch observes memory again after it returns.
    if(!_mm256_testz_si256(flags,eligible)) {
        const auto absent=_mm256_cmpeq_epi16(_mm256_and_si256(flags,eligible),_mm256_setzero_si256());
        const auto present=~uint32_t(_mm256_movemask_epi8(absent));
        return __builtin_popcount(present)>=16?-1:0;
    }
    const auto last=PPC_LOAD_U16(address+30);
    const auto clear=_mm256_set1_epi16(int16_t(__builtin_bswap16(uint16_t(ctx.r21.u32))));
    _mm256_storeu_si256(data,_mm256_andnot_si256(clear,flags));
    // Exact state after sixteen original no-callback iterations.
    ctx.r28.u64=ctx.r23.u64;ctx.r11.u64=0;ctx.r10.u64=last;ctx.r9.u64=uint64_t(last)&~ctx.r21.u64;
    ctx.cr6.compare<uint32_t>(0,0,ctx.xer);
    ctx.xer.ca=true;ctx.r24.s64-=16;
    ctx.cr0.compare<int32_t>(ctx.r24.s32,0,ctx.xer);
    ctx.r30.s64+=64;ctx.r29.s64+=32;
    return true;
}

namespace {
constexpr uint32_t scene=0x10000,flags_address=scene+0x2EF24,output=0x9000;
struct Call {uint32_t kind,index;bool operator==(const Call&)const=default;};
struct Probe {std::vector<Call> calls;bool mutate=false;unsigned count=0;};
thread_local Probe* probe=nullptr;
void Setup(PPCContext& ctx,uint8_t* base) {
    probe->calls.push_back({0,0});
    for(unsigned i=0;i<4;++i)PPC_STORE_U32(ctx.r4.u32+i*4,0x12345670+i);
    ctx.r3.u64=0;
}
void Eligible(PPCContext& ctx,uint8_t* base) {
    const auto index=PPC_LOAD_U32(ctx.r3.u32+4);
    probe->calls.push_back({1,index});
    if(probe->mutate && index+3<probe->count)PPC_STORE_U16(flags_address+(index+3)*2,1);
    ctx.r3.u64=index%3!=0;
}
void Submit(PPCContext& ctx,uint8_t* base) {
    probe->calls.push_back({2,PPC_LOAD_U32(ctx.r3.u32+4)});ctx.r3.u64=0;
}
void End(PPCContext& ctx,uint8_t*) {probe->calls.push_back({3,ctx.r4.u32});ctx.r3.u64=0x1234;}
}
bool em2::TestNativeVisibilityScan() {
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100010000ull,MEM_RESERVE,PAGE_NOACCESS));
    const auto enabled=REXCVAR_GET(em2_native_visibility_scan);
    struct Restore {uint8_t* base;bool enabled;Probe* saved=probe;unsigned csr=_mm_getcsr();
        ~Restore(){probe=saved;REXCVAR_SET(em2_native_visibility_scan,enabled);_mm_setcsr(csr);if(base)VirtualFree(base,0,MEM_RELEASE);}
    } restore{base,enabled};
    if(!base || !VirtualAlloc(base,0x100000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0x83390000,0x10000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0x834B0000,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
    // Install only fixture virtual calls in the sparse guest function table.
    // The full original scene scan and original list append execute unchanged.
    const std::array<PPCFunc*,4> callbacks={Setup,Eligible,Submit,End};
    for(unsigned i=0;i<4;++i)PPC_LOOKUP_FUNC(base,0x822F0100+i*4)=callbacks[i];
    std::vector<uint8_t> before(0x100000),expected_memory(0x100000);
    Probe observation;probe=&observation;unsigned comparisons=0;
    auto initialize=[&](unsigned count,unsigned mode,unsigned channel,uint32_t clear) {
        std::memset(base,0,0x100000);
        PPC_STORE_U32(scene+12+channel*4,0x5000);PPC_STORE_U32(0x5000,0x6000);
        PPC_STORE_U32(0x6000+52,0x822F0100);PPC_STORE_U32(0x6000+12,0x822F010C);
        PPC_STORE_U32(0x7000+184,0x822F0104);PPC_STORE_U32(0x7000+192,0x822F0108);
        PPC_STORE_U32(output,0x60000);PPC_STORE_U32(output+8,6000);
        PPC_STORE_U32(scene+0x31E20,count?count-1:0xFFFFFFFFu);
        PPC_STORE_U32(0x83395098+channel*4,1);PPC_STORE_U32(0x833950AC+channel*4,clear);
        for(unsigned i=0;i<std::min(count,6000u);++i) {
            unsigned flags=(i*0x531u)&0xFAFEu;
            if(mode==1 && i%37==17)flags|=1;
            if(mode==2)flags|=1;
            if(mode==3)flags|=0x401;
            if(mode==4)flags|=1;
            if(mode==5 && i==17)flags|=1;
            PPC_STORE_U16(flags_address+i*2,flags);
            PPC_STORE_U32(scene+36+i*4,mode==4?0:0x80000+i*16);
            PPC_STORE_U32(0x80000+i*16,0x7000);PPC_STORE_U32(0x80004+i*16,i);
        }
        observation.mutate=mode==5;observation.count=std::min(count,6000u);
    };
    for(unsigned count:{0u,1u,15u,16u,17u,31u,32u,33u,127u,6000u,6001u})
      for(unsigned mode=0;mode<6;++mode)for(unsigned channel:{0u,4u})for(uint32_t clear:{8u,0xFFFFu}) {
        initialize(count,mode,channel,clear);std::memcpy(before.data(),base,before.size());
        PPCContext initial{};initial.r1.u64=0xF0000;initial.r3.u64=scene;initial.r4.u64=channel;initial.r5.u64=output;
        initial.r8.u64=initial.r10.u64=initial.r14.u64=initial.r31.u64=0xFEDCBA9876543210ull;
        initial.lr=0xFFFFFFFF829D8228ull;initial.xer.so=mode%2;initial.fpscr.InitHost();
        observation.calls.clear();auto reference=initial;__imp__sub_8256F948(reference,base);
        auto expected_calls=observation.calls;std::memcpy(expected_memory.data(),base,expected_memory.size());
        for(bool optimized:{false,true}) {
            std::memcpy(base,before.data(),before.size());observation.calls.clear();
            REXCVAR_SET(em2_native_visibility_scan,optimized);auto actual=initial;sub_8256F948(actual,base);
            if(std::memcmp(&reference,&actual,sizeof(actual)) || std::memcmp(base,expected_memory.data(),expected_memory.size()) || observation.calls!=expected_calls) {
                REXLOG_ERROR("Native visibility mismatch: count {}, mode {}, channel {}, clear {}, optimized {}",count,mode,channel,clear,optimized);return false;
            }
            ++comparisons;
        }
    }
    REXLOG_INFO("Native visibility scan matched original full context, guest memory and callback order in {} comparisons, including callback mutations",comparisons);
    if(REXCVAR_GET(em2_benchmark_visibility_scan)) {
        for(unsigned mode:{0u,1u,2u}) {
            initialize(6000,mode,0,8);
            std::array<double,2> elapsed{};std::array<uint64_t,2> sums{};
            for(unsigned trial=0;trial<6;++trial)for(unsigned phase=0;phase<2;++phase) {
                const unsigned path=(trial+phase)%2;REXCVAR_SET(em2_native_visibility_scan,path!=0);
                const auto start=std::chrono::steady_clock::now();
                for(unsigned i=0;i<1000;++i) {
                    observation.calls.clear();PPCContext context{};context.r1.u64=0xF0000;
                    context.r3.u64=scene;context.r4.u64=0;context.r5.u64=output;
                    sub_8256F948(context,base);sums[path]+=PPC_LOAD_U32(output+4);
                }
                elapsed[path]+=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count();
            }
            if(sums[0]!=sums[1])return false;
            REXLOG_INFO("Scene scan benchmark 6000 entries, mode {} (0 empty/1 sparse/2 dense): original {:.1f}, batched {:.1f} ns/call, {:.2f}x routine speedup; not game FPS",
                mode,elapsed[0]/6000,elapsed[1]/6000,elapsed[0]/elapsed[1]);
        }
    }
    return true;
}

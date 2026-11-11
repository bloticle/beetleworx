#include "material_binding.h"
#include "guest_word_read.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <immintrin.h>
#include <intrin.h>
#include <vector>
#include <windows.h>

PPC_EXTERN_FUNC(__imp__sub_82C8A498);
PPC_FUNC(EM2MaterialBitOps);
PPC_FUNC(EM2MaterialEmpty);
PPC_FUNC(EM2MaterialWordReads);
PPC_FUNC(EM2MaterialBitOpsWordReads);
PPC_FUNC(EM2MaterialBarrierReads);
PPC_FUNC(EM2MaterialBitOpsBarrierReads);
PPC_FUNC(EM2MaterialLikelyReads);
PPC_FUNC(EM2MaterialBitOpsLikelyReads);
REXCVAR_DEFINE_BOOL(em2_native_material_binding,true,"CPU","Skip empty native shader-binding stages and use supported host bit instructions");
REXCVAR_DEFINE_BOOL(em2_predicted_material_reads,true,"CPU","Prefer virtual-address reads while retaining physical-heap translation in material binding");
REXCVAR_DEFINE_BOOL(em2_benchmark_material_dispatch,false,"Debug","Compare lazy and startup-resolved material dispatch in same-executable CPU diagnostics");
REXCVAR_DEFINE_BOOL(em2_benchmark_material_reads,false,"Debug","Compare complete material-binding guest-word readers in same-executable CPU diagnostics");
REXCVAR_DEFINE_BOOL(em2_benchmark_material_sparse_reads,false,"Debug","Compare predicted reads on small binding lists with longer paired samples");
REXCVAR_DEFINE_BOOL(em2_benchmark_material_binding,false,"Debug","Benchmark complete native material-binding calls in the sole EXE's headless mode");
namespace {
// CPU detection only uses CPUID. Resolve before guest threads start, so the
// production entry does not carry a thread-safe local-static guard per call.
PPCFunc* const material_implementation=em2::NativeMaterialBitOpsSupported()?EM2MaterialBitOps:EM2MaterialEmpty;
PPCFunc* const predicted_material_implementation=em2::NativeMaterialBitOpsSupported()?EM2MaterialBitOpsLikelyReads:EM2MaterialLikelyReads;
__declspec(noinline) void ReferenceMaterialDispatch(PPCContext& __restrict ctx,uint8_t* base) {
    if(!REXCVAR_GET(em2_native_material_binding)){__imp__sub_82C8A498(ctx,base);return;}
    material_implementation(ctx,base);
}
__declspec(noinline) void LazyMaterialDispatch(PPCContext& __restrict ctx,uint8_t* base) {
    if(!REXCVAR_GET(em2_native_material_binding)){__imp__sub_82C8A498(ctx,base);return;}
    static PPCFunc* const implementation=em2::NativeMaterialBitOpsSupported()?EM2MaterialBitOpsLikelyReads:EM2MaterialLikelyReads;
    (REXCVAR_GET(em2_predicted_material_reads)?implementation:material_implementation)(ctx,base);
}
struct MaterialCallbackEvent {uint64_t lr,r3,r4,r5,r6,r7,csr;};
struct MaterialCallbackTrace {std::array<MaterialCallbackEvent,8> events{};unsigned count=0;bool mutate=false;};
thread_local MaterialCallbackTrace* callback_trace=nullptr;
void MaterialFixtureCallback(PPCContext& ctx,uint8_t* base) {
    auto& trace=*callback_trace;
    if(trace.count<trace.events.size())trace.events[trace.count]={ctx.lr,ctx.r3.u64,ctx.r4.u64,ctx.r5.u64,ctx.r6.u64,ctx.r7.u64,_mm_getcsr()};
    ++trace.count;
    PPC_STORE_U32(0x2F000+trace.count*4,ctx.r4.u32^ctx.r5.u32);
    if(trace.mutate){PPC_STORE_U64(0x1000,1);PPC_STORE_U64(0x2000,1);}
    ctx.r7.u64=0xA498000000000000ull|trace.count;
    ctx.fpscr.enableFlushMode();
}
unsigned InvokeMaterialFixture(PPCFunc* function,PPCContext& ctx,uint8_t* base) {
    __try {function(ctx,base);return 0;}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){return GetExceptionCode();}
}
}

PPC_FUNC_IMPL(sub_82C8A498) {
    if(!REXCVAR_GET(em2_native_material_binding)){__imp__sub_82C8A498(ctx,base);return;}
    (REXCVAR_GET(em2_predicted_material_reads)?predicted_material_implementation:material_implementation)(ctx,base);
}

bool em2::NativeMaterialBitOpsSupported() {
    static const bool supported=[] {
        int cpu[4];__cpuid(cpu,0);if(cpu[0]<7)return false;
        __cpuidex(cpu,7,0);if((cpu[1]&((1<<3)|(1<<8)))!=((1<<3)|(1<<8)))return false;
        __cpuid(cpu,0x80000000);if(uint32_t(cpu[0])<0x80000001u)return false;
        __cpuid(cpu,0x80000001);return (cpu[2]&(1<<5))!=0;
    }();
    return supported;
}

bool em2::TestNativeMaterialBinding() {
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100010000ull,MEM_RESERVE,PAGE_NOACCESS));
    struct Release {uint8_t* base;unsigned csr=_mm_getcsr();bool enabled=REXCVAR_GET(em2_native_material_binding);bool predicted=REXCVAR_GET(em2_predicted_material_reads);MaterialCallbackTrace* previous=callback_trace;~Release(){callback_trace=previous;REXCVAR_SET(em2_native_material_binding,enabled);REXCVAR_SET(em2_predicted_material_reads,predicted);_mm_setcsr(csr);if(base)VirtualFree(base,0,MEM_RELEASE);}} release{base};
    constexpr uint32_t global1=uint32_t(-2093350912)-7656,global2=uint32_t(-2093088768)+23268;
    if(!base || !VirtualAlloc(base,0x100000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+(global1&~0xFFFFu),0x10000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+(global2&~0xFFFFu),0x10000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+PPC_IMAGE_BASE+PPC_IMAGE_SIZE,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
    constexpr uint32_t callback_address=uint32_t(PPC_CODE_BASE);
    PPC_LOOKUP_FUNC(base,callback_address)=MaterialFixtureCallback;
    // Both sides of the physical threshold, byte-unaligned crossings, and
    // the last logical address need real backing, including the E0 bias.
    for(const auto [offset,size]:{std::pair(uint64_t(0xDFFF0000),size_t(0x20000)),
                                std::pair(uint64_t(0xFFFF0000),size_t(0x20000))}){
        auto* region=static_cast<uint8_t*>(VirtualAlloc(base+offset,size,MEM_COMMIT,PAGE_READWRITE));
        if(!region)return false;
        for(size_t i=0;i<size;++i)region[i]=uint8_t((i*37)^(i>>8));
    }
    unsigned reader_cases=0;
    for(uint32_t address:{0u,1u,3u,0xFFFDu,0xDFFF0000u,0xDFFFFFFCu,0xDFFFFFFDu,0xDFFFFFFEu,0xDFFFFFFFu,
                         0xE0000000u,0xE0000001u,0xE0000FFDu,0xFFFFEFFCu,0xFFFFFFFCu,0xFFFFFFFDu,0xFFFFFFFEu,0xFFFFFFFFu}){
        if(em2::LikelyGuestWord(address,base)!=PPC_LOAD_U32(address))return false;
        ++reader_cases;
    }
    REXLOG_INFO("Predicted guest-word reader matched original in {} physical/unaligned/wrap-boundary reads, including the last logical address",reader_cases);
    constexpr uint32_t material=0x1000,ps_dirty=0x2000,slots=0x3000,properties=0x4000,
        definition=0x6000,masks=0x7000,zero_masks=0x7100,records=0x8000,
        device=0x20000,source=0x40000,stack=0xF0000;
    std::vector<uint8_t> before(0x100000),expected_memory(before.size());
    uint32_t random=0xA498360;auto next=[&](){random=random*1664525u+1013904223u;return random;};
    unsigned comparisons=0;
    auto prepare=[&](unsigned groups_vs,unsigned groups_ps,uint64_t mask_vs,uint64_t mask_ps,unsigned pattern,unsigned type) {
        const auto groups=std::max(groups_vs,groups_ps);
        std::memset(base,0,before.size());
        PPC_STORE_U32(material+256,ps_dirty);PPC_STORE_U32(material+264,properties);
        PPC_STORE_U32(material+268,slots);PPC_STORE_U32(material+300,slots+4);
        PPC_STORE_U32(slots,properties);PPC_STORE_U32(slots+4,source+0x8000);
        PPC_STORE_U32(material+288,groups_vs);PPC_STORE_U32(material+292,groups_ps);
        PPC_STORE_U32(material+296,source);PPC_STORE_U32(material+536,definition);PPC_STORE_U32(material+700,device);
        const auto options=((pattern+type)%4)*0x10;
        PPC_STORE_U32(material+692,options);PPC_STORE_U32(material+532,0x18000);
        PPC_STORE_U32(0x18000+12,0x18100);PPC_STORE_U32(0x18000+16,0x18200);
        PPC_STORE_U32(0x18100+16,1);PPC_STORE_U32(0x18100+20,1);
        PPC_STORE_U32(0x18200+128,1);PPC_STORE_U32(0x18200+132,1);
        PPC_STORE_U32(device+64,callback_address);PPC_STORE_U32(device+468,callback_address);
        for(unsigned i=0;i<16;++i)PPC_STORE_U32(definition+i*4,zero_masks);
        PPC_STORE_U32(definition+type*4,masks);PPC_STORE_U32(definition+32+type*4,masks);
        PPC_STORE_U32(definition+64,records);PPC_STORE_U32(definition+68,records);
        PPC_STORE_U32(definition+80,source+0xC000);
        std::memcpy(base+definition+128,base+definition,96);
        PPC_STORE_U32(material+696,pattern?0x80000000u:0);
        PPC_STORE_U32(global1,pattern?0x100u:0);PPC_STORE_U32(global2,definition+128);
        for(unsigned group=0;group<groups;++group) {
            if(group<groups_vs)PPC_STORE_U64(material+group*8,std::rotr(mask_vs,int(group*7)));
            if(group<groups_ps)PPC_STORE_U64(ps_dirty+group*8,std::rotr(mask_ps,int(group*7)));
            PPC_STORE_U64(masks+group*8,~uint64_t(0));
        }
        for(unsigned i=0;i<groups*64;++i) {
            PPC_STORE_U32(records+i*16,i<<18);
            PPC_STORE_U32(records+i*16+4,(i%128)|((i%4)<<10));
            PPC_STORE_U32(properties+i*8+4,i*4);
        }
        constexpr std::array<uint32_t,8> special={0,0x80000000u,1,0x7F800000u,0xFF800000u,0x7FC12345u,0x7F812345u,0xCF000000u};
        for(unsigned i=0;i<0x20000;i+=4)PPC_STORE_U32(source+i,pattern==2?special[(i/4)%special.size()]:pattern?next():std::bit_cast<uint32_t>(float(int(i%128)-64)*0.25f));
        if(type>=6)for(unsigned i=0;i<groups*64;++i) {
            const auto texture=0x70000+i*64;
            for(unsigned offset=28;offset<=48;offset+=4)PPC_STORE_U32(texture+offset,next());
            PPC_STORE_U32(source+i*64,pattern?texture:0);
            PPC_STORE_U32(source+0x8000+i*64,pattern?texture:0);
        }
    };
    constexpr std::array<uint64_t,10> masks_to_test={0,1,0x8000000000000000ull,0xFFFFFFFFFFFFFFFFull,
        0xAAAAAAAAAAAAAAAAull,0x5555555555555555ull,0xF00000000000000Full,0x00FF00000000FF00ull,
        0x7FFFFFFFFFFFFFFFull,0x8000000000000001ull};
    constexpr std::array<std::array<unsigned,2>,9> group_pairs={std::array<unsigned,2>{0,0},{0,1},{1,0},{1,1},{2,4},{4,2},{1,16},{16,1},{16,16}};
    for(unsigned type=0;type<8;++type)for(auto pair:group_pairs)for(auto mask:masks_to_test)for(unsigned pattern=0;pattern<3;++pattern)for(unsigned ps_mode=0;ps_mode<3;++ps_mode) {
        if(ps_mode==1 && !mask)continue;
        prepare(pair[0],pair[1],mask,ps_mode==0?mask:ps_mode==1?0:~mask,pattern,type);std::memcpy(before.data(),base,before.size());
        PPCContext initial{};
        for(unsigned i=0;i<32;++i){const auto value=uint64_t(next())<<32|next();std::memcpy(reinterpret_cast<uint8_t*>(&initial.r0)+i*8,&value,8);}
        for(auto* byte=reinterpret_cast<uint8_t*>(&initial.v0);byte<reinterpret_cast<uint8_t*>(&initial.v127)+16;++byte)*byte=uint8_t(next());
        initial.r1.u64=stack;initial.r3.u64=material;initial.lr=uint64_t(next())<<32|next();initial.ctr.u64=uint64_t(next())<<32|next();initial.xer.so=pattern&1;initial.xer.ca=(pattern>>1)&1;
        initial.fpscr.InitHost();const auto csr=_mm_getcsr();
        MaterialCallbackTrace expected_trace{};expected_trace.mutate=pattern==2;callback_trace=&expected_trace;
        auto expected=initial;
        if(const auto exception=InvokeMaterialFixture(__imp__sub_82C8A498,expected,base)) {
            REXLOG_ERROR("Original material fixture fault {:08X}: type {}, groups {}/{}, mask {:016X}, pattern {}; r9/10/11 {:08X}/{:08X}/{:08X}, r18/20/21/22/23/24/25 {:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}/{:08X}",exception,type,pair[0],pair[1],mask,pattern,expected.r9.u32,expected.r10.u32,expected.r11.u32,expected.r18.u32,expected.r20.u32,expected.r21.u32,expected.r22.u32,expected.r23.u32,expected.r24.u32,expected.r25.u32);return false;
        }
        const auto expected_csr=_mm_getcsr();std::memcpy(expected_memory.data(),base,before.size());
        const std::array<PPCFunc*,12> candidates={EM2MaterialEmpty,NativeMaterialBitOpsSupported()?EM2MaterialBitOps:EM2MaterialEmpty,sub_82C8A498,sub_82C8A498,
            EM2MaterialWordReads,NativeMaterialBitOpsSupported()?EM2MaterialBitOpsWordReads:EM2MaterialWordReads,
            EM2MaterialBarrierReads,NativeMaterialBitOpsSupported()?EM2MaterialBitOpsBarrierReads:EM2MaterialBarrierReads,LazyMaterialDispatch,
            EM2MaterialLikelyReads,NativeMaterialBitOpsSupported()?EM2MaterialBitOpsLikelyReads:EM2MaterialLikelyReads,sub_82C8A498};
        for(unsigned mode=0;mode<candidates.size();++mode) {
        auto* candidate=candidates[mode];REXCVAR_SET(em2_native_material_binding,mode!=2);REXCVAR_SET(em2_predicted_material_reads,mode!=11);
        MaterialCallbackTrace actual_trace{};actual_trace.mutate=expected_trace.mutate;callback_trace=&actual_trace;
        std::memcpy(base,before.data(),before.size());auto actual=initial;_mm_setcsr(csr);
        if(InvokeMaterialFixture(candidate,actual,base)){REXLOG_ERROR("Candidate material fixture fault: type {}, groups {}/{}, mask {:016X}",type,pair[0],pair[1],mask);return false;}
        if(std::memcmp(&actual,&expected,sizeof(actual)) || std::memcmp(base,expected_memory.data(),before.size()) || _mm_getcsr()!=expected_csr ||
           actual_trace.count!=expected_trace.count || std::memcmp(actual_trace.events.data(),expected_trace.events.data(),sizeof(actual_trace.events))) {
            REXLOG_ERROR("Native material bit operations mismatch: type {}, groups {}/{}, mask {:016X}, pattern {}, PS mode {}",type,pair[0],pair[1],mask,pattern,ps_mode);return false;
        }
        ++comparisons;
        }
    }
    callback_trace=release.previous;REXCVAR_SET(em2_predicted_material_reads,release.predicted);
    unsigned guards=0;
    for(unsigned groups=1;groups<=16;++groups) {
        const auto address=0x100000-groups*8;
        std::memset(base+address,0,groups*8);
        if(!MaterialMaskEmpty(address,groups,base))return false;
        PPC_STORE_U64(0xFFFF8,1);
        if(MaterialMaskEmpty(address,groups,base))return false;
        ++guards;
    }
    for(auto candidate:std::array<std::array<uint32_t,2>,5>{std::array<uint32_t,2>{0x7F000000,1},{0xFFFFFFF8,2},{0xDFFFFFF8,2},{0x7F000000,0},{0x7F000000,17}}) {
        if(MaterialMaskEmpty(candidate[0],candidate[1],base))return false;
        ++guards;
    }
    REXLOG_INFO("Native material binding matched original context, low-memory fixture, MXCSR and callback trace in {} complete calls: eight VS/PS classes, independent groups/masks, native conversions/setters, texture bind/unbind, all optional-list flags and callbacks that dirty state; direct and production enabled/disabled paths; {} mask guard-page/unsafe-span cases",comparisons,guards);
    if(REXCVAR_GET(em2_benchmark_material_dispatch)) {
        using Clock=std::chrono::steady_clock;constexpr unsigned rounds=7,iterations=2000;
        struct Affinity {DWORD_PTR previous=0;~Affinity(){if(previous)SetThreadAffinityMask(GetCurrentThread(),previous);}} affinity;
        affinity.previous=SetThreadAffinityMask(GetCurrentThread(),DWORD_PTR(1)<<GetCurrentProcessorNumber());
        REXCVAR_SET(em2_native_material_binding,true);
        prepare(2,2,1,1,0,0);PPC_STORE_U32(material+692,0);
        PPCContext warm{};warm.r1.u64=stack;warm.fpscr.InitHost();
        const auto deadline=Clock::now()+std::chrono::milliseconds(250);
        do {for(unsigned i=0;i<1000;++i){PPC_STORE_U64(material,1);PPC_STORE_U64(ps_dirty,1);warm.r3.u64=material;LazyMaterialDispatch(warm,base);}}while(Clock::now()<deadline);
        REXLOG_INFO("Material dispatch benchmark setup: pinned {}, 250 ms warmup; startup CPU selection {}, predicted reads {}",affinity.previous!=0,material_implementation==EM2MaterialBitOps?"bitops":"portable",REXCVAR_GET(em2_predicted_material_reads));
        for(unsigned type=0;type<8;++type)for(uint64_t mask:{0ull,1ull,0xAAAAAAAAAAAAAAAAull,0xFFFFFFFFFFFFFFFFull}) {
            prepare(2,2,mask,mask,type>=6?1:0,type);PPC_STORE_U32(material+692,0);
            std::array<std::array<double,rounds>,2> times{};
            for(unsigned round=0;round<rounds;++round)for(unsigned order=0;order<2;++order) {
                const auto mode=(round+order)&1;PPCFunc* volatile function=mode?sub_82C8A498:LazyMaterialDispatch;
                PPCContext ctx{};ctx.r1.u64=stack;ctx.fpscr.InitHost();
                const auto start=Clock::now();
                for(unsigned i=0;i<iterations;++i) {
                    for(unsigned group=0;group<2;++group){PPC_STORE_U64(material+group*8,mask);PPC_STORE_U64(ps_dirty+group*8,mask);}
                    ctx.r3.u64=material;function(ctx,base);
                }
                times[mode][round]=std::chrono::duration<double,std::nano>(Clock::now()-start).count()/iterations;
            }
            for(auto& mode:times)std::sort(mode.begin(),mode.end());
            REXLOG_INFO("Material dispatch benchmark type {}, mask {:016X}: lazy {:.2f}, startup {:.2f} ns/call, {:.3f}x (seven alternating rounds; complete routine; not gameplay FPS)",type,mask,times[0][3],times[1][3],times[0][3]/times[1][3]);
        }
    }
    if(REXCVAR_GET(em2_benchmark_material_reads)) {
        using Clock=std::chrono::steady_clock;constexpr unsigned rounds=9,iterations=1000;
        struct Affinity {DWORD_PTR previous=0;~Affinity(){if(previous)SetThreadAffinityMask(GetCurrentThread(),previous);}} affinity;
        affinity.previous=SetThreadAffinityMask(GetCurrentThread(),DWORD_PTR(1)<<GetCurrentProcessorNumber());
        const bool bitops=NativeMaterialBitOpsSupported();
        const std::array<PPCFunc*,4> functions={bitops?EM2MaterialBitOps:EM2MaterialEmpty,
            bitops?EM2MaterialBitOpsWordReads:EM2MaterialWordReads,
            bitops?EM2MaterialBitOpsBarrierReads:EM2MaterialBarrierReads,
            bitops?EM2MaterialBitOpsLikelyReads:EM2MaterialLikelyReads};
        prepare(2,2,1,1,0,0);PPC_STORE_U32(material+692,0);
        PPCContext warm{};warm.r1.u64=stack;warm.fpscr.InitHost();
        const auto deadline=Clock::now()+std::chrono::milliseconds(250);
        do {for(unsigned i=0;i<1000;++i){PPC_STORE_U64(material,1);PPC_STORE_U64(ps_dirty,1);warm.r3.u64=material;functions[0](warm,base);}}while(Clock::now()<deadline);
        REXLOG_INFO("Material guest-read benchmark setup: pinned {}, bitops {}, 250 ms warmup",affinity.previous!=0,bitops);
        for(unsigned type=0;type<8;++type)for(uint64_t mask:{0ull,1ull,0xAAAAAAAAAAAAAAAAull,0xFFFFFFFFFFFFFFFFull})for(unsigned stage=0;stage<3;++stage) {
            if(!mask && stage)continue;
            const auto vs_mask=stage==1?0:mask,ps_mask=stage==2?0:mask;
            prepare(2,2,vs_mask,ps_mask,type>=6?1:0,type);PPC_STORE_U32(material+692,0);
            std::array<std::array<double,rounds>,4> times{};
            for(unsigned round=0;round<rounds;++round)for(unsigned order=0;order<4;++order) {
                const auto mode=(round+order)%4;PPCFunc* volatile function=functions[mode];
                PPCContext ctx{};ctx.r1.u64=stack;ctx.r3.u64=material;ctx.fpscr.InitHost();
                const auto start=Clock::now();
                for(unsigned i=0;i<iterations;++i) {
                    for(unsigned group=0;group<2;++group){PPC_STORE_U64(material+group*8,vs_mask);PPC_STORE_U64(ps_dirty+group*8,ps_mask);}
                    ctx.r3.u64=material;function(ctx,base);
                }
                times[mode][round]=std::chrono::duration<double,std::nano>(Clock::now()-start).count()/iterations;
            }
            for(auto& mode:times)std::sort(mode.begin(),mode.end());
            REXLOG_INFO("Material guest-read benchmark type {}, masks {:016X}/{:016X}: default {:.2f}, explicit {:.2f}, barrier {:.2f}, predicted {:.2f} ns/call; {:.3f}x/{:.3f}x/{:.3f}x (nine rotating rounds; complete routine; not gameplay FPS)",type,vs_mask,ps_mask,times[0][4],times[1][4],times[2][4],times[3][4],times[0][4]/times[1][4],times[0][4]/times[2][4],times[0][4]/times[3][4]);
        }
    }
    if(REXCVAR_GET(em2_benchmark_material_sparse_reads)) {
        using Clock=std::chrono::steady_clock;constexpr unsigned rounds=31,iterations=20000;
        struct Affinity {DWORD_PTR previous=0;~Affinity(){if(previous)SetThreadAffinityMask(GetCurrentThread(),previous);}} affinity;
        affinity.previous=SetThreadAffinityMask(GetCurrentThread(),DWORD_PTR(1)<<GetCurrentProcessorNumber());
        const bool bitops=NativeMaterialBitOpsSupported();
        REXCVAR_SET(em2_native_material_binding,true);REXCVAR_SET(em2_predicted_material_reads,true);
        const std::array<PPCFunc*,3> functions={ReferenceMaterialDispatch,
            bitops?EM2MaterialBitOpsLikelyReads:EM2MaterialLikelyReads,sub_82C8A498};
        for(unsigned type=0;type<8;++type)for(unsigned stage=0;stage<3;++stage){
            const uint64_t vs_mask=stage==1?0:1,ps_mask=stage==2?0:1;
            prepare(2,2,vs_mask,ps_mask,type>=6?1:0,type);PPC_STORE_U32(material+692,0);
            std::array<std::array<double,rounds>,3> times{};
            for(unsigned round=0;round<rounds;++round)for(unsigned order=0;order<3;++order){
                const unsigned mode=(round+order)%3;PPCFunc* volatile function=functions[mode];
                PPCContext ctx{};ctx.r1.u64=stack;ctx.fpscr.InitHost();
                // Warm this exact stage/path before each timed block.
                for(unsigned i=0;i<1000;++i){for(unsigned group=0;group<2;++group){PPC_STORE_U64(material+group*8,vs_mask);PPC_STORE_U64(ps_dirty+group*8,ps_mask);}ctx.r3.u64=material;function(ctx,base);}
                const auto start=Clock::now();
                for(unsigned i=0;i<iterations;++i){for(unsigned group=0;group<2;++group){PPC_STORE_U64(material+group*8,vs_mask);PPC_STORE_U64(ps_dirty+group*8,ps_mask);}ctx.r3.u64=material;function(ctx,base);}
                times[mode][round]=std::chrono::duration<double,std::nano>(Clock::now()-start).count()/iterations;
            }
            std::array<double,rounds> paired{};for(unsigned i=0;i<rounds;++i)paired[i]=times[0][i]/times[2][i];
            std::sort(paired.begin(),paired.end());for(auto& mode:times)std::sort(mode.begin(),mode.end());
            REXLOG_INFO("Sparse production-read benchmark type {}, stage {}: reference dispatch {:.2f}, direct predicted {:.2f}, production {:.2f} ns/call; paired median {:.3f}x, range {:.3f}..{:.3f} (31 rotating rounds x20000 calls, warmed per block; complete routine, not gameplay FPS)",type,stage,times[0][15],times[1][15],times[2][15],paired[15],paired.front(),paired.back());
        }
    }
    if(REXCVAR_GET(em2_benchmark_material_binding)) {
        using Clock=std::chrono::steady_clock;constexpr unsigned rounds=7,iterations=3000;
        for(unsigned type=0;type<8;++type)for(uint64_t mask:{0ull,1ull,0xAAAAAAAAAAAAAAAAull,0xFFFFFFFFFFFFFFFFull})for(unsigned stage=0;stage<3;++stage) {
            if(!mask && stage)continue;
            const auto vs_mask=stage==1?0:mask,ps_mask=stage==2?0:mask;
            prepare(2,2,vs_mask,ps_mask,type>=6?1:0,type);
            // Benchmark the ordinary binding path without optional test callbacks.
            PPC_STORE_U32(material+692,0);REXCVAR_SET(em2_native_material_binding,true);
            std::array<std::array<double,rounds>,2> times{};
            const std::array<PPCFunc*,2> functions={__imp__sub_82C8A498,sub_82C8A498};
            for(unsigned round=0;round<rounds;++round)for(unsigned order=0;order<2;++order) {
                const auto mode=(round+order)%2;auto* function=functions[mode];
                PPCContext ctx{};ctx.r1.u64=stack;ctx.r3.u64=material;ctx.fpscr.InitHost();
                const auto start=Clock::now();
                for(unsigned i=0;i<iterations;++i) {
                    for(unsigned group=0;group<2;++group){PPC_STORE_U64(material+group*8,vs_mask);PPC_STORE_U64(ps_dirty+group*8,ps_mask);}
                    ctx.r3.u64=material;function(ctx,base);
                }
                times[mode][round]=std::chrono::duration<double,std::nano>(Clock::now()-start).count()/iterations;
            }
            std::array<double,rounds> paired{};for(unsigned round=0;round<rounds;++round)paired[round]=times[0][round]/times[1][round];
            std::sort(paired.begin(),paired.end());for(auto& mode:times)std::sort(mode.begin(),mode.end());
            REXLOG_INFO("Native material benchmark type {}, two VS/PS groups, masks {:016X}/{:016X}: original {:.2f}, production {:.2f} ns/call, {:.3f}x; paired median {:.3f}, range {:.3f}..{:.3f} (7 alternating rounds; complete binding, not gameplay FPS)",type,vs_mask,ps_mask,times[0][3],times[1][3],times[0][3]/times[1][3],paired[3],paired[0],paired[6]);
        }
    }
    return true;
}

#include "binding_cache.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>

PPC_EXTERN_FUNC(__imp__sub_8266E0A0);
REXCVAR_DEFINE_BOOL(em2_native_binding_cache,true,"CPU","Simplify render binding-cache checks while preserving complete guest register results");
REXCVAR_DEFINE_BOOL(em2_benchmark_binding_cache,false,"Debug","Benchmark binding-cache checks in same-executable CPU diagnostics");
namespace {
// Diagnostic-only: the exact translated leaf under a single ISA change.
// Normal gameplay does not call it. CPUID gates every diagnostic invocation.
template<bool Combined> __attribute__((always_inline)) uint32_t ProbeLoad(uint32_t address,uint8_t* base) {
    uint32_t value;const auto* pointer=reinterpret_cast<const volatile uint32_t*>(PPC_RAW_ADDR(address));
    if constexpr(Combined)__asm__ volatile("movbel %1, %0" : "=r"(value) : "m"(*pointer));
    else __asm__ volatile("movl %1, %0\n\tbswapl %0" : "=r"(value) : "m"(*pointer));
    return value;
}
template<bool Combined> __attribute__((target("movbe"), noinline)) void MovbeBindingReference(PPCContext& __restrict ctx,uint8_t* base) {
	PPC_FUNC_PROLOGUE();
	// lbz r9,4(r4)
	ctx.r9.u64 = PPC_LOAD_U8(ctx.r4.u32 + 4);
	// cmplwi cr6,r9,0
	ctx.cr6.compare<uint32_t>(ctx.r9.u32, 0, ctx.xer);
	// bne cr6,0x8266e0b4
	if (!ctx.cr6.eq) goto loc_8266E0B4;
loc_8266E0AC:
	// li r3,0
	ctx.r3.s64 = 0;
	// blr 
	return;
loc_8266E0B4:
	// lwz r11,0(r4)
	ctx.r11.u64 = ProbeLoad<Combined>(ctx.r4.u32 + 0,base);
	// rlwinm r10,r11,1,0,30
	ctx.r10.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 1) & 0xFFFFFFFE;
	// add r10,r11,r10
	ctx.r10.u64 = ctx.r11.u64 + ctx.r10.u64;
	// rlwinm r10,r10,2,0,29
	ctx.r10.u64 = __builtin_rotateleft64(ctx.r10.u32 | (ctx.r10.u64 << 32), 2) & 0xFFFFFFFC;
	// add r10,r10,r3
	ctx.r10.u64 = ctx.r10.u64 + ctx.r3.u64;
	// lwz r8,8(r10)
	ctx.r8.u64 = ProbeLoad<Combined>(ctx.r10.u32 + 8,base);
	// cmpw cr6,r8,r11
	ctx.cr6.compare<int32_t>(ctx.r8.s32, ctx.r11.s32, ctx.xer);
	// bne cr6,0x8266e0f4
	if (!ctx.cr6.eq) goto loc_8266E0F4;
	// lbz r8,12(r10)
	ctx.r8.u64 = PPC_LOAD_U8(ctx.r10.u32 + 12);
	// cmplw cr6,r8,r9
	ctx.cr6.compare<uint32_t>(ctx.r8.u32, ctx.r9.u32, ctx.xer);
	// bne cr6,0x8266e0f4
	if (!ctx.cr6.eq) goto loc_8266E0F4;
	// lwz r10,16(r10)
	ctx.r10.u64 = ProbeLoad<Combined>(ctx.r10.u32 + 16,base);
	// lwz r9,8(r4)
	ctx.r9.u64 = ProbeLoad<Combined>(ctx.r4.u32 + 8,base);
	// cmplw cr6,r10,r9
	ctx.cr6.compare<uint32_t>(ctx.r10.u32, ctx.r9.u32, ctx.xer);
	// li r10,1
	ctx.r10.s64 = 1;
	// beq cr6,0x8266e0f8
	if (ctx.cr6.eq) goto loc_8266E0F8;
loc_8266E0F4:
	// li r10,0
	ctx.r10.s64 = 0;
loc_8266E0F8:
	// subfic r9,r11,31
	ctx.xer.ca = ctx.r11.u32 <= 31;
	ctx.r9.s64 = 31 - ctx.r11.s64;
	// lwz r8,236(r3)
	ctx.r8.u64 = ProbeLoad<Combined>(ctx.r3.u32 + 236,base);
	// li r7,1
	ctx.r7.s64 = 1;
	// clrlwi r11,r10,24
	ctx.r11.u64 = ctx.r10.u32 & 0xFF;
	// slw r6,r7,r9
	ctx.r6.u64 = ctx.r9.u8 & 0x20 ? 0 : (ctx.r7.u32 << (ctx.r9.u8 & 0x3F));
	// and r4,r6,r8
	ctx.r4.u64 = ctx.r6.u64 & ctx.r8.u64;
	// cmplwi cr6,r4,0
	ctx.cr6.compare<uint32_t>(ctx.r4.u32, 0, ctx.xer);
	// beq cr6,0x8266e124
	if (ctx.cr6.eq) goto loc_8266E124;
	// clrlwi r10,r11,24
	ctx.r10.u64 = ctx.r11.u32 & 0xFF;
	// cmplwi cr6,r10,0
	ctx.cr6.compare<uint32_t>(ctx.r10.u32, 0, ctx.xer);
	// beq cr6,0x8266e0ac
	if (ctx.cr6.eq) goto loc_8266E0AC;
loc_8266E124:
	// clrlwi r10,r5,24
	ctx.r10.u64 = ctx.r5.u32 & 0xFF;
	// cmplwi cr6,r10,0
	ctx.cr6.compare<uint32_t>(ctx.r10.u32, 0, ctx.xer);
	// bne cr6,0x8266e140
	if (!ctx.cr6.eq) goto loc_8266E140;
	// clrlwi r11,r11,24
	ctx.r11.u64 = ctx.r11.u32 & 0xFF;
	// li r3,0
	ctx.r3.s64 = 0;
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// bnelr cr6
	if (!ctx.cr6.eq) return;
loc_8266E140:
	// li r3,1
	ctx.r3.s64 = 1;
	// blr 
	return;
}
bool HasMovbe(){int cpu[4];__cpuid(cpu,1);return (cpu[2]&(1<<22))!=0;}
uint32_t RawWord(uint32_t address,uint8_t* base) {
    return *reinterpret_cast<volatile uint32_t*>(PPC_RAW_ADDR(address));
}
template<bool Views> __declspec(noinline) void MatchActiveBinding(PPCContext& __restrict ctx,uint8_t* base,uint32_t version,uint32_t request) {
    const uint32_t table=ctx.r3.u32;
    const auto raw_index=RawWord(request,base),index=std::byteswap(raw_index);
    const uint32_t entry=table+index*12+8;
    bool same=false;
    // Preserve native short-circuit loads, including their order and widths.
    // The intermediate compared words never survive in the guest context, so
    // equality can use guest byte order without swapping both value words.
    if constexpr(Views) {
        if(RawWord(entry,base)==raw_index) {
            const auto* pointer=PPC_RAW_ADDR(entry);
            // One resolved view is valid throughout the record unless it
            // crosses a 512 MiB boundary (including E0 mapping and 32-bit wrap).
            if(__builtin_expect((entry&0x1FFFFFFFu)<=0x1FFFFFF4u,1)) {
                if(*reinterpret_cast<const volatile uint8_t*>(pointer+4)==version) {
                    const auto cached=*reinterpret_cast<const volatile uint32_t*>(pointer+8);
                    const auto requested=RawWord(request+8,base);same=cached==requested;
                }
            }else if(PPC_LOAD_U8(entry+4)==version) {
                const auto cached=RawWord(entry+8,base);
                const auto requested=RawWord(request+8,base);same=cached==requested;
            }
        }
    }else if(RawWord(entry,base)==raw_index && PPC_LOAD_U8(entry+4)==version) {
        const auto cached=RawWord(entry+8,base);
        const auto requested=RawWord(request+8,base);same=cached==requested;
    }
    ctx.r9.s64=31-int64_t(index);
    ctx.xer.ca=index<=31;
    ctx.r8.u64=PPC_LOAD_U32(table+236);
    ctx.r7.u64=1;
    ctx.r11.u64=same;
    const unsigned shift=ctx.r9.u8;
    ctx.r6.u64=(shift&32)?0:(uint32_t(1)<<(shift&31));
    ctx.r4.u64=ctx.r6.u64&ctx.r8.u64;
    if(ctx.r4.u32 && !same) {
        ctx.r10.u64=0;ctx.cr6.compare<uint32_t>(0,0,ctx.xer);ctx.r3.u64=0;return;
    }
    ctx.r10.u64=ctx.r5.u32&255;
    if(ctx.r10.u32){ctx.cr6.compare<uint32_t>(ctx.r10.u32,0,ctx.xer);ctx.r3.u64=1;return;}
    ctx.cr6.compare<uint32_t>(unsigned(same),0,ctx.xer);
    ctx.r3.u64=!same;
}
template<bool Views> __attribute__((always_inline)) void BindingEntry(PPCContext& ctx,uint8_t* base) {
    if(!REXCVAR_GET(em2_native_binding_cache)){__imp__sub_8266E0A0(ctx,base);return;}
    const uint32_t request=ctx.r4.u32;const auto version=PPC_LOAD_U8(request+4);
    if(!version){ctx.r9.u64=0;ctx.cr6.compare<uint32_t>(0,0,ctx.xer);ctx.r3.u64=0;return;}
    MatchActiveBinding<Views>(ctx,base,version,request);
}
}
PPC_FUNC_IMPL(sub_8266E0A0) {BindingEntry<true>(ctx,base);}

bool em2::TestNativeBindingCache() {
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100010000ull,MEM_RESERVE,PAGE_NOACCESS));
    const auto saved=REXCVAR_GET(em2_native_binding_cache);const auto saved_csr=_mm_getcsr();
    struct Release {
        uint8_t* base;bool setting;unsigned csr;
        ~Release(){if(base)VirtualFree(base,0,MEM_RELEASE);REXCVAR_SET(em2_native_binding_cache,setting);_mm_setcsr(csr);}
    } release{base,saved,saved_csr};
    if(!base || !VirtualAlloc(base,0x100000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0xDFFF0000,0x40000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0xFFFF0000,0x20000,MEM_COMMIT,PAGE_READWRITE))return false;
    unsigned comparisons=0,previous_comparisons=0,movbe_comparisons=0;const bool movbe=HasMovbe();
    auto compare=[&](uint32_t table,uint32_t request,unsigned force,unsigned status) {
        PPCContext initial{};
        initial.r3.u64=0xFEDCBA9800000000ull|table;initial.r4.u64=0x8765432100000000ull|request;initial.r5.u64=0x13579BDF00000000ull|force;
        initial.r6.u64=0x1234567812345678ull;initial.r7.u64=0xDEADBEEF12345678ull;initial.r8.u64=0xF0E1D2C3B4A59687ull;
        initial.r9.u64=0x1122334455667788ull;initial.r10.u64=0xFFEEDDCCBBAA9988ull;initial.r11.u64=UINT64_MAX;
        initial.r14.u64=0x7654321012345678ull;initial.lr=0xFFFFFFFF8266E148ull;initial.ctr.u64=0x0123456789ABCDEFull;
        initial.xer.ca=status&1;initial.xer.so=(status>>1)&1;initial.xer.ov=(status>>2)&1;
        initial.cr6.compare<int32_t>(-1,0,initial.xer);
        _mm_setcsr(0x1F80|((status&3)<<13));
        auto reference=initial;__imp__sub_8266E0A0(reference,base);const auto csr=_mm_getcsr();
        for(bool enabled:{false,true}) {
            REXCVAR_SET(em2_native_binding_cache,enabled);auto actual=initial;_mm_setcsr(0x1F80|((status&3)<<13));
            sub_8266E0A0(actual,base);
            if(std::memcmp(&reference,&actual,sizeof(actual)) || csr!=_mm_getcsr()) {
                REXLOG_ERROR("Binding cache mismatch table {:08X}, request {:08X}, force {}, status {}, enabled {}",table,request,force,status,enabled);return false;
            }
            ++comparisons;
        }
        {auto actual=initial;_mm_setcsr(0x1F80|((status&3)<<13));BindingEntry<false>(actual,base);
            if(std::memcmp(&reference,&actual,sizeof(actual)) || csr!=_mm_getcsr()){REXLOG_ERROR("Prior binding-cache control mismatch");return false;}++previous_comparisons;}
        if(movbe)for(auto invoke:{&MovbeBindingReference<false>,&MovbeBindingReference<true>}){auto actual=initial;_mm_setcsr(0x1F80|((status&3)<<13));invoke(actual,base);
            if(std::memcmp(&reference,&actual,sizeof(actual)) || csr!=_mm_getcsr()){REXLOG_ERROR("Guest load instruction probe mismatch");return false;}++movbe_comparisons;}
        return true;
    };
    for(uint32_t table_base:{0x10000u,0xE0004000u,0xFFFF8000u,0xDFFFFFF0u,0xFFFFFFF0u})for(unsigned alignment:{0u,1u,3u})
    for(uint32_t index:{0u,1u,15u,31u,32u,33u,63u,64u,127u,255u,256u,0x40000000u,0x80000000u,0xFFFFFFFEu,0xFFFFFFFFu})
    for(unsigned mismatch=0;mismatch<4;++mismatch)for(unsigned version:{0u,1u,255u})for(uint32_t mask:{0u,0xA5C38100u,UINT32_MAX})
    for(unsigned force:{0u,1u,255u,256u})for(unsigned status:{0u,3u,5u,6u}) {
        const uint32_t table=table_base+alignment,request=0x4000+alignment,entry=table+index*12+8;
        PPC_STORE_U32(request,index);PPC_STORE_U8(request+4,uint8_t(version));PPC_STORE_U32(request+8,0x7F812345);
        PPC_STORE_U32(entry,index^(mismatch==1?1u:0u));PPC_STORE_U8(entry+4,uint8_t(version^(mismatch==2?1u:0u)));
        PPC_STORE_U32(entry+8,0x7F812345u^(mismatch==3?1u:0u));PPC_STORE_U32(table+236,mask);
        if(!compare(table,request,force,status))return false;
    }
    // First mismatch must not read later fields across an inaccessible page.
    for(unsigned guard=0;guard<3;++guard) {
        uint32_t table=0x20000,request=0x4000;
        if(guard==0){table=0x23FF4;PPC_STORE_U32(table+0xC008,0x1001);}
        if(guard==1){table=0x23FF3;PPC_STORE_U32(table+0xC008,0x1000);PPC_STORE_U8(table+0xC00C,2);}
        if(guard==2){request=0x2FFFC;PPC_STORE_U8(request+4,0);}
        else {PPC_STORE_U32(request,0x1000);PPC_STORE_U8(request+4,1);PPC_STORE_U32(table+236,0);}
        DWORD previous=0;const uint32_t page=guard==2?0x2F000:0x30000;
        if(!VirtualProtect(base+page,0x1000,PAGE_NOACCESS,&previous))return false;
        const bool ok=compare(table,request,0,3);
        DWORD ignored=0;VirtualProtect(base+page,0x1000,previous,&ignored);
        if(!ok)return false;
    }
    REXLOG_INFO("Binding cache passed {} full-context/MXCSR comparisons: flags, exact bits, match stages, masks, force flags, unusual/wrapped indices, unaligned/E0 mappings and guarded short-circuit loads",comparisons);
    REXLOG_INFO("Prior binding-cache control passed {} full-context/MXCSR comparisons",previous_comparisons);
    REXLOG_INFO("MOVBE translated-leaf differential checks: supported {}, {} context/MXCSR comparisons; diagnostic only",movbe,movbe_comparisons);
    struct BenchAffinity {DWORD_PTR previous=0;~BenchAffinity(){if(previous)SetThreadAffinityMask(GetCurrentThread(),previous);}} affinity;
    if(REXCVAR_GET(em2_benchmark_binding_cache)) {
        affinity.previous=SetThreadAffinityMask(GetCurrentThread(),DWORD_PTR(1)<<GetCurrentProcessorNumber());
        _mm_setcsr(0x1F80);
        constexpr uint32_t table=0x10000,request=0x4000;
        PPC_STORE_U32(request,3);PPC_STORE_U8(request+4,1);PPC_STORE_U32(request+8,0xDEADBEEF);
        PPC_STORE_U32(table+44,3);PPC_STORE_U8(table+48,1);PPC_STORE_U32(table+52,0xDEADBEEF);PPC_STORE_U32(table+236,UINT32_MAX);
        PPCFunc* volatile warm=__imp__sub_8266E0A0;PPCContext ctx{};
        const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
        do {for(unsigned i=0;i<10000;++i){ctx.r3.u64=table;ctx.r4.u64=request;ctx.r5.u64=i&1;warm(ctx,base);}}while(std::chrono::steady_clock::now()<end);
        REXLOG_INFO("Guest-load benchmark setup: affinity pinned {}, 250 ms original-leaf warmup; no gameplay",affinity.previous!=0);
    }
    if(REXCVAR_GET(em2_benchmark_binding_cache)) {
        _mm_setcsr(0x1F80);
        constexpr unsigned rounds=7,iterations=200000;using Clock=std::chrono::steady_clock;
        for(unsigned scenario=0;scenario<6;++scenario) {
            constexpr uint32_t table=0x10000,request=0x4000,index=3,entry=table+index*12+8;
            PPC_STORE_U32(request,index);PPC_STORE_U8(request+4,scenario==0?0:1);PPC_STORE_U32(request+8,0xDEADBEEF);
            PPC_STORE_U32(entry,index^(scenario==1?1:0));PPC_STORE_U8(entry+4,scenario==2?2:1);
            PPC_STORE_U32(entry+8,0xDEADBEEFu^(scenario==3?1u:0u));PPC_STORE_U32(table+236,scenario==5?UINT32_MAX:0);
            std::array<std::array<double,rounds>,2> times{};std::array<uint64_t,2> sums{};
            auto* volatile invoke=&sub_8266E0A0;
            for(unsigned round=0;round<rounds;++round)for(unsigned order=0;order<2;++order) {
                const auto mode=(round+order)&1;REXCVAR_SET(em2_native_binding_cache,mode!=0);PPCContext ctx{};
                const auto begin=Clock::now();
                for(unsigned i=0;i<iterations;++i){ctx.r3.u64=table;ctx.r4.u64=request;ctx.r5.u64=i&1;invoke(ctx,base);sums[mode]+=ctx.r3.u64;}
                times[mode][round]=std::chrono::duration<double,std::nano>(Clock::now()-begin).count()/iterations;
            }
            if(sums[0]!=sums[1])return false;
            for(auto& mode:times)std::sort(mode.begin(),mode.end());
            REXLOG_INFO("Binding cache benchmark scenario {}: original {:.2f}, native {:.2f} ns/call, {:.3f}x (seven alternating rounds; production entry; not gameplay FPS)",scenario,times[0][3],times[1][3],times[0][3]/times[1][3]);
        }
    }
    if(REXCVAR_GET(em2_benchmark_binding_cache)) {
        constexpr unsigned rounds=7,iterations=200000;using Clock=std::chrono::steady_clock;
        constexpr uint32_t table=0x10000,request=0x4000,index=3,entry=table+index*12+8;
        REXCVAR_SET(em2_native_binding_cache,true);
        for(unsigned scenario=0;scenario<6;++scenario) {
            PPC_STORE_U32(request,index);PPC_STORE_U8(request+4,scenario==0?0:1);PPC_STORE_U32(request+8,0xDEADBEEF);
            PPC_STORE_U32(entry,index^(scenario==1?1:0));PPC_STORE_U8(entry+4,scenario==2?2:1);
            PPC_STORE_U32(entry+8,0xDEADBEEFu^(scenario==3?1u:0u));PPC_STORE_U32(table+236,scenario==5?UINT32_MAX:0);
            std::array<std::array<double,rounds>,2> times{};std::array<uint64_t,2> sums{};
            for(unsigned round=0;round<rounds;++round)for(unsigned order=0;order<2;++order) {
                const auto mode=(round+order)&1;PPCFunc* volatile invoke=mode?sub_8266E0A0:BindingEntry<false>;PPCContext ctx{};
                const auto begin=Clock::now();
                for(unsigned i=0;i<iterations;++i){ctx.r3.u64=table;ctx.r4.u64=request;ctx.r5.u64=i&1;invoke(ctx,base);sums[mode]+=ctx.r3.u64;}
                times[mode][round]=std::chrono::duration<double,std::nano>(Clock::now()-begin).count()/iterations;
            }
            if(sums[0]!=sums[1])return false;
            for(auto& mode:times)std::sort(mode.begin(),mode.end());
            REXLOG_INFO("Binding cache view benchmark scenario {}: previous {:.2f}, view {:.2f} ns/call, {:.3f}x (seven alternating rounds; pinned/warmed; not gameplay FPS)",scenario,times[0][3],times[1][3],times[0][3]/times[1][3]);
        }
    }
    if(movbe && REXCVAR_GET(em2_benchmark_binding_cache)) {
        constexpr unsigned rounds=9,iterations=200000;using Clock=std::chrono::steady_clock;
        constexpr uint32_t table=0x10000,request=0x4000,index=3,entry=table+index*12+8;
        for(unsigned scenario=0;scenario<6;++scenario) {
            PPC_STORE_U32(request,index);PPC_STORE_U8(request+4,scenario==0?0:1);PPC_STORE_U32(request+8,0xDEADBEEF);
            PPC_STORE_U32(entry,index^(scenario==1?1:0));PPC_STORE_U8(entry+4,scenario==2?2:1);
            PPC_STORE_U32(entry+8,0xDEADBEEFu^(scenario==3?1u:0u));PPC_STORE_U32(table+236,scenario==5?UINT32_MAX:0);
            std::array<std::array<double,rounds>,3> times{};std::array<uint64_t,3> sums{};
            for(unsigned round=0;round<rounds;++round)for(unsigned order=0;order<3;++order) {
                const auto mode=(round+order)%3;PPCFunc* volatile invoke=mode==0?__imp__sub_8266E0A0:mode==1?MovbeBindingReference<false>:MovbeBindingReference<true>;PPCContext ctx{};
                const auto begin=Clock::now();
                for(unsigned i=0;i<iterations;++i){ctx.r3.u64=table;ctx.r4.u64=request;ctx.r5.u64=i&1;invoke(ctx,base);sums[mode]+=ctx.r3.u64;}
                times[mode][round]=std::chrono::duration<double,std::nano>(Clock::now()-begin).count()/iterations;
            }
            if(sums[0]!=sums[1] || sums[0]!=sums[2])return false;
            for(auto& mode:times)std::sort(mode.begin(),mode.end());
            REXLOG_INFO("Guest-load three-way benchmark scenario {}: original {:.2f}, explicit MOV+BSWAP {:.2f}, MOVBE {:.2f} ns/call; compiler/asm {:.3f}x, asm/MOVBE {:.3f}x (nine rotating rounds, same translated source; pinned/warmed; not gameplay FPS)",scenario,times[0][4],times[1][4],times[2][4],times[0][4]/times[1][4],times[1][4]/times[2][4]);
        }
    }
    return true;
}

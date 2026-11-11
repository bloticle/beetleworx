#include "shader_upload.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <windows.h>
#include <array>
#include <chrono>
#include <cstring>
#include <immintrin.h>
#include <vector>

PPC_EXTERN_FUNC(__imp__sub_826BF668);
PPC_EXTERN_FUNC(sub_826BF668);
PPC_EXTERN_FUNC(__imp__sub_826BF2D0);
PPC_EXTERN_FUNC(sub_826BF2D0);
PPC_EXTERN_FUNC(__imp__sub_826BF510);
PPC_EXTERN_FUNC(sub_826BF510);
REXCVAR_DEFINE_BOOL(em2_native_shader_upload,true,"CPU","Copy long native shader constant runs directly, preserving packet encoding and final VMX state");
REXCVAR_DEFINE_BOOL(em2_benchmark_shader_upload,false,"Debug","Benchmark shader constant upload in headless regressions");
namespace {
bool CopyUploadPrefix(uint32_t source,uint32_t destination,unsigned bytes,unsigned copied,uint8_t* base) {
    const auto source_end=uint64_t(source)+bytes,destination_end=uint64_t(destination)+bytes;
    if(source_end>0x100000000ull || destination_end>0x100000000ull ||
       PPC_PHYS_HOST_OFFSET(source)!=PPC_PHYS_HOST_OFFSET(uint32_t(source_end-1)) ||
       PPC_PHYS_HOST_OFFSET(destination)!=PPC_PHYS_HOST_OFFSET(uint32_t(destination_end-1)) ||
       (source<destination_end && destination<source_end))return false;
    // MMIO cannot be read/written as ordinary host RAM. Distinct physical
    // views can alias the same pages even when virtual ranges are disjoint.
    // A run crossing a physical view boundary may wrap to those pages too.
    if((uint64_t(source)<0x80000000ull && source_end>0x7F000000ull) ||
       (uint64_t(destination)<0x80000000ull && destination_end>0x7F000000ull) ||
       (source_end>0xA0000000ull && (source>>29)!=(uint32_t(source_end-1)>>29)) ||
       (destination_end>0xA0000000ull && (destination>>29)!=(uint32_t(destination_end-1)>>29)) ||
       (source>=0xA0000000u && destination>=0xA0000000u && (source>>29)!=(destination>>29)))return false;
    std::memcpy(PPC_RAW_ADDR(destination),PPC_RAW_ADDR(source),copied);return true;
}
}
void em2::SkipShaderUploadPrefix(PPCContext& ctx,uint8_t* base) {
    const auto count=ctx.r28.u32;
    if(count<8 || count>64)return;
    const auto source=uint32_t(ctx.r31.u32+4)&~15u,destination=uint32_t(ctx.r4.u32+4)&~15u;
    // lvx/stvx reverse byte order twice. Their net memory result is an exact
    // copy. Retain the last native iteration to publish v60..v63 and CR/XER.
    const auto prefix=count-1;const auto copied=prefix*64;
    if(!CopyUploadPrefix(source,destination,count*64,copied,base))return;
    ctx.r31.s64+=copied;ctx.r4.s64+=copied;ctx.r28.s64-=prefix;ctx.r30.u64<<=prefix;
}
void em2::SkipScalarShaderUploadPrefix(PPCContext& ctx,uint8_t* base) {
    const auto count=ctx.r11.u32;
    if(count<8 || count>64)return;
    const auto prefix=count-1,copied=prefix*4;
    if(!CopyUploadPrefix(uint32_t(ctx.r29.u32+4),uint32_t(ctx.r4.u32+4),count*4,copied,base))return;
    // Keep the last lwzu/stwu iteration for the native final word and CR/XER.
    ctx.r29.s64+=copied;ctx.r4.s64+=copied;ctx.r11.s64-=prefix;ctx.r31.u64<<=prefix;
}
void em2::SkipFetchShaderUploadPrefix(PPCContext& ctx,uint8_t* base) {
    const auto count=ctx.r26.u32;
    if(count<8 || count>64)return;
    const auto prefix=count-1,copied=prefix*24;
    if(!CopyUploadPrefix(uint32_t(ctx.r30.u32+4),uint32_t(ctx.r4.u32+4),count*24,copied,base))return;
    // ld/std byte swaps cancel. The last native iteration publishes the three
    // final words, CR/XER and dirty-mask state exactly as the original does.
    ctx.r30.s64+=copied;ctx.r4.s64+=copied;ctx.r26.s64-=prefix;ctx.r31.u64<<=prefix;
}
bool em2::TestNativeShaderUpload() {
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100010000ull,MEM_RESERVE,PAGE_NOACCESS));
    const bool enabled=REXCVAR_GET(em2_native_shader_upload);
    struct Release {uint8_t* base;bool enabled;unsigned csr=_mm_getcsr();~Release(){REXCVAR_SET(em2_native_shader_upload,enabled);_mm_setcsr(csr);if(base)VirtualFree(base,0,MEM_RELEASE);}} release{base,enabled};
    if(!base || !VirtualAlloc(base,0x100000,MEM_COMMIT,PAGE_READWRITE))return false;
    constexpr uint32_t device=0x1000;
    std::vector<uint8_t> before(0x100000),expected_memory(0x100000);
    uint32_t random=0xF668360;
    for(auto& byte:before){random=random*1664525u+1013904223u;byte=uint8_t(random>>24);}
    constexpr std::array<uint64_t,12> masks={1,0x8000000000000000ull,0x7F,0xFF,0xF,0xF000000000000000ull,
        0xFFFFFFFFFFFFFFFFull,0xAAAAAAAAAAAAAAAAull,0xF0F0F0F0F0F0F0F0ull,0xFFFF0000FFFF0000ull,
        0x00000000FFFFFFFFull,0x800000000000000Full};
    unsigned comparisons=0;
    for(bool scalar:{false,true})for(auto mask:masks)for(unsigned source_alignment:{0u,1u,8u,15u})for(unsigned destination_alignment:{0u,4u,8u,12u})
      for(int alias:{-128,-16,0,16,128,0x20000})for(unsigned index:{0u,512u}) {
        const uint32_t source=0x20000+source_alignment;
        const uint32_t destination=uint32_t(0x20000+alias)+destination_alignment;
        auto prepare=[&] {
            std::memcpy(base,before.data(),before.size());
            PPC_STORE_U32(device+48,destination);PPC_STORE_U32(device+52,destination+0x10000);
        };
        PPCContext initial{};initial.r1.u64=0xF0000;initial.r3.u64=device;initial.r4.u64=mask;
        initial.r5.u64=index;initial.r6.u64=source;initial.r12.u64=0xFEDCBA9876543210ull;
        initial.lr=0xFFFFFFFF829D8228ull;initial.ctr.u64=0x1122334455667788ull;initial.xer.so=bool(index);initial.fpscr.InitHost();
        const auto initial_csr=_mm_getcsr();prepare();auto expected=initial;
        (scalar?__imp__sub_826BF2D0:__imp__sub_826BF668)(expected,base);const auto expected_csr=_mm_getcsr();
        std::memcpy(expected_memory.data(),base,expected_memory.size());
        for(bool optimized:{false,true}) {
            prepare();auto actual=initial;REXCVAR_SET(em2_native_shader_upload,optimized);_mm_setcsr(initial_csr);
            (scalar?sub_826BF2D0:sub_826BF668)(actual,base);
            if(std::memcmp(&actual,&expected,sizeof(actual)) || std::memcmp(base,expected_memory.data(),expected_memory.size()) || _mm_getcsr()!=expected_csr) {
                REXLOG_ERROR("Shader upload mismatch: scalar {}, mask {:016X}, source alignment {}, destination alignment {}, alias {}, index {}, optimized {}",
                    scalar,mask,source_alignment,destination_alignment,alias,index,optimized);return false;
            }
            ++comparisons;
        }
    }
    // Real native calls across the SDK's physical mapping gap and a wrapping
    // source validate the guarded fallback, not just the helper's predicate.
    if(!VirtualAlloc(base+0xDFFF0000,0x30000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0xFFFF0000,0x11000,MEM_COMMIT,PAGE_READWRITE))return false;
    std::vector<uint8_t> high_before(0x41000),high_expected(0x41000);
    for(auto& byte:high_before){random=random*1664525u+1013904223u;byte=uint8_t(random>>24);}
    auto high_copy=[&](uint8_t* bytes,bool to_guest) {
        std::memcpy(to_guest?base+0xDFFF0000:bytes,to_guest?bytes:base+0xDFFF0000,0x30000);
        std::memcpy(to_guest?base+0xFFFF0000:bytes+0x30000,to_guest?bytes+0x30000:base+0xFFFF0000,0x11000);
    };
    for(bool scalar:{false,true})for(unsigned edge=0;edge<3;++edge) {
        const uint32_t source=edge==0?0x20000u:edge==1?(scalar?0xDFFFFF80u:0xDFFFF800u):(scalar?0xFFFFFF80u:0xFFFFF800u);
        const uint32_t destination=edge==0?0xDFFFF800u:0x40000u;
        auto prepare=[&] {
            std::memcpy(base,before.data(),before.size());high_copy(high_before.data(),true);
            PPC_STORE_U32(device+48,destination);PPC_STORE_U32(device+52,destination+0x10000);
        };
        PPCContext initial{};initial.r1.u64=0xF0000;initial.r3.u64=device;
        initial.r4.u64=0xFFFFFFFFFFFFFFFFull;initial.r6.u64=source;initial.fpscr.InitHost();
        const auto csr=_mm_getcsr();prepare();auto expected=initial;
        (scalar?__imp__sub_826BF2D0:__imp__sub_826BF668)(expected,base);
        const auto expected_csr=_mm_getcsr();std::memcpy(expected_memory.data(),base,expected_memory.size());high_copy(high_expected.data(),false);
        for(bool optimized:{false,true}) {
            prepare();auto actual=initial;REXCVAR_SET(em2_native_shader_upload,optimized);_mm_setcsr(csr);
            (scalar?sub_826BF2D0:sub_826BF668)(actual,base);
            if(std::memcmp(&actual,&expected,sizeof(actual)) || std::memcmp(base,expected_memory.data(),expected_memory.size()) ||
               std::memcmp(base+0xDFFF0000,high_expected.data(),0x30000) || std::memcmp(base+0xFFFF0000,high_expected.data()+0x30000,0x11000) ||
               _mm_getcsr()!=expected_csr) {
                REXLOG_ERROR("Shader upload boundary mismatch: scalar {}, edge {}, optimized {}",scalar,edge,optimized);return false;
            }
            ++comparisons;
        }
    }
    REXLOG_INFO("Native shader upload matched original packet bytes, full context and MXCSR in {} comparisons, including short/mixed/64-block runs, unaligned/overlapping buffers, physical-map gaps and wrapping sources",comparisons);
    if(REXCVAR_GET(em2_benchmark_shader_upload)) {
        for(bool scalar:{false,true})for(auto mask:{uint64_t(1),uint64_t(0xF),uint64_t(0xFF),uint64_t(0xFFFF),uint64_t(0xFFFFFFFFFFFFFFFFull),uint64_t(0xAAAAAAAAAAAAAAAAull)}) {
            std::array<double,2> elapsed{};std::array<uint64_t,2> sums{};
            for(unsigned trial=0;trial<6;++trial)for(unsigned phase=0;phase<2;++phase) {
                const unsigned mode=(trial+phase)%2;REXCVAR_SET(em2_native_shader_upload,mode!=0);
                const auto function=mode?(scalar?sub_826BF2D0:sub_826BF668):(scalar?__imp__sub_826BF2D0:__imp__sub_826BF668);
                PPCContext context{};context.r1.u64=0xF0000;
                const auto start=std::chrono::steady_clock::now();
                for(unsigned i=0;i<20000;++i) {
                    PPC_STORE_U32(device+48,0x40000);PPC_STORE_U32(device+52,0x50000);
                    context.r3.u64=device;context.r4.u64=mask;context.r5.u64=0;context.r6.u64=0x20000;
                    function(context,base);sums[mode]+=PPC_LOAD_U32(device+48);
                }
                elapsed[mode]+=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count();
            }
            if(sums[0]!=sums[1])return false;
            REXLOG_INFO("Shader upload benchmark scalar {}, mask {:016X}: original {:.1f}, native {:.1f} ns/call, {:.2f}x routine speedup; not game FPS",scalar,mask,elapsed[0]/120000,elapsed[1]/120000,elapsed[0]/elapsed[1]);
        }
    }
    return TestNativeFetchUpload();
}

bool em2::TestNativeFetchUpload() {
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100010000ull,MEM_RESERVE,PAGE_NOACCESS));
    const bool enabled=REXCVAR_GET(em2_native_shader_upload);
    struct Release {uint8_t* base;bool enabled;unsigned csr=_mm_getcsr();~Release(){REXCVAR_SET(em2_native_shader_upload,enabled);_mm_setcsr(csr);if(base)VirtualFree(base,0,MEM_RELEASE);}} release{base,enabled};
    if(!base || !VirtualAlloc(base,0x100000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0xDFFF0000,0x30000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0xFFFF0000,0x11000,MEM_COMMIT,PAGE_READWRITE))return false;
    constexpr size_t sizes[]={0x100000,0x30000,0x11000};
    uint8_t* regions[]={base,base+0xDFFF0000,base+0xFFFF0000};
    std::array<std::vector<uint8_t>,3> before,expected_memory;
    uint32_t random=0xBF510360;
    for(unsigned i=0;i<3;++i) {
        before[i].resize(sizes[i]);expected_memory[i].resize(sizes[i]);
        for(auto& byte:before[i]){random=random*1664525u+1013904223u;byte=uint8_t(random>>24);}
    }
    constexpr std::array<uint64_t,12> masks={1,0x8000000000000000ull,0x7F,0xFF,0xF,0xF000000000000000ull,
        0xFFFFFFFFFFFFFFFFull,0xAAAAAAAAAAAAAAAAull,0xF0F0F0F0F0F0F0F0ull,0xFFFF0000FFFF0000ull,
        0x00000000FFFFFFFFull,0x800000000000000Full};
    unsigned comparisons=0;
    auto compare=[&](uint32_t device,uint32_t destination,uint64_t mask) {
        auto prepare=[&] {
            for(unsigned i=0;i<3;++i)std::memcpy(regions[i],before[i].data(),sizes[i]);
            PPC_STORE_U32(device+48,destination);PPC_STORE_U32(device+52,destination+0x10000);
            PPC_STORE_U32(device+56,destination+0x10000);
        };
        PPCContext initial{};initial.r1.u64=0xF0000;initial.r3.u64=device;initial.r4.u64=mask;
        initial.r12.u64=0xFEDCBA9876543210ull;initial.lr=0xFFFFFFFF829D8228ull;initial.ctr.u64=0x1122334455667788ull;
        initial.xer.so=bool(mask&1);initial.fpscr.InitHost();
        const auto csr=_mm_getcsr();prepare();auto expected=initial;
        __imp__sub_826BF510(expected,base);const auto expected_csr=_mm_getcsr();
        for(unsigned i=0;i<3;++i)std::memcpy(expected_memory[i].data(),regions[i],sizes[i]);
        for(bool optimized:{false,true}) {
            prepare();auto actual=initial;REXCVAR_SET(em2_native_shader_upload,optimized);_mm_setcsr(csr);
            sub_826BF510(actual,base);
            bool match=!std::memcmp(&actual,&expected,sizeof(actual)) && _mm_getcsr()==expected_csr;
            for(unsigned i=0;i<3;++i)match&=!std::memcmp(regions[i],expected_memory[i].data(),sizes[i]);
            if(!match) {
                REXLOG_ERROR("Fetch upload mismatch: device {:08X}, destination {:08X}, mask {:016X}, optimized {}",device,destination,mask,optimized);return false;
            }
            ++comparisons;
        }
        return true;
    };
    for(auto mask:masks)for(unsigned device_alignment:{0u,1u,8u,15u})for(unsigned destination_alignment:{0u,4u,8u,12u})
      for(int alias:{-128,-24,0,24,128,0x20000}) {
        const uint32_t device=0x10000+device_alignment,source=device+1152;
        if(!compare(device,uint32_t(source+alias)+destination_alignment,mask))return false;
    }
    for(bool output:{false,true})for(uint32_t address:{0xDFFFFD00u,0xFFFFFD00u})
        if(!compare(output?0x10000u:address-1152,output?address:0x40000,0xFFFFFFFFFFFFFFFFull))return false;
    unsigned rejected=0;
    for(unsigned kind=0;kind<3;++kind)for(const auto pair:std::array<std::array<uint32_t,2>,9>{{
        {0xA0001000u,0xE0001000u},{0xBFFFFFF0u,0xA0001000u},{0xA0001000u,0xBFFFFFF0u},
        {0x9FFFFFF0u,0xE0001000u},{0xE0001000u,0x9FFFFFF0u},
        {0x7F000000u,0x40000u},{0x40000u,0x7F000000u},{0x7EFFFFF0u,0x40000u},{0x40000u,0x7EFFFFF0u}}}) {
        // These addresses are deliberately inaccessible in this sparse fixture:
        // rejection must leave every context byte intact without touching RAM.
        PPCContext context{};context.r4.u64=pair[1]-4;
        if(kind==0){context.r28.u64=64;context.r31.u64=pair[0]-4;context.r30.u64=~0ull;}
        if(kind==1){context.r11.u64=64;context.r29.u64=pair[0]-4;context.r31.u64=~0ull;}
        if(kind==2){context.r26.u64=64;context.r30.u64=pair[0]-4;context.r31.u64=~0ull;}
        const auto expected=context;
        (kind==0?SkipShaderUploadPrefix:kind==1?SkipScalarShaderUploadPrefix:SkipFetchShaderUploadPrefix)(context,base);
        if(std::memcmp(&expected,&context,sizeof(context))){REXLOG_ERROR("Shader upload alias/MMIO guard failed: kind {}",kind);return false;}
        ++rejected;
    }
    REXLOG_INFO("Shader upload guards rejected {} inaccessible MMIO/physical-view candidates without changing context",rejected);
    REXLOG_INFO("Native fetch upload matched original packets, full context, owned memory and MXCSR in {} comparisons, including short/mixed/64-record, alignment, overlap, physical-map and wrap cases",comparisons);
    if(REXCVAR_GET(em2_benchmark_shader_upload)) {
        constexpr uint32_t device=0x10000;
        for(auto mask:{uint64_t(1),uint64_t(0xF),uint64_t(0xFF),uint64_t(0xFFFF),uint64_t(0xFFFFFFFFFFFFFFFFull),uint64_t(0xAAAAAAAAAAAAAAAAull)}) {
            std::array<double,2> elapsed{};std::array<uint64_t,2> sums{};
            for(unsigned trial=0;trial<6;++trial)for(unsigned phase=0;phase<2;++phase) {
                const unsigned mode=(trial+phase)%2;REXCVAR_SET(em2_native_shader_upload,mode!=0);
                const auto function=mode?sub_826BF510:__imp__sub_826BF510;
                PPCContext context{};context.r1.u64=0xF0000;
                const auto start=std::chrono::steady_clock::now();
                for(unsigned i=0;i<20000;++i) {
                    PPC_STORE_U32(device+48,0x40000);PPC_STORE_U32(device+52,0x50000);PPC_STORE_U32(device+56,0x50000);
                    context.r3.u64=device;context.r4.u64=mask;function(context,base);sums[mode]+=PPC_LOAD_U32(device+48);
                }
                elapsed[mode]+=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count();
            }
            if(sums[0]!=sums[1])return false;
            REXLOG_INFO("Fetch upload benchmark mask {:016X}: original {:.1f}, native {:.1f} ns/call, {:.2f}x routine speedup; not game FPS",mask,elapsed[0]/120000,elapsed[1]/120000,elapsed[0]/elapsed[1]);
        }
    }
    return true;
}

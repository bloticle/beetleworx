#include "memory_copy.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <windows.h>
#include <array>
#include <chrono>
#include <cstring>
#include <vector>

PPC_EXTERN_FUNC(__imp__sub_82B6A550);
PPC_EXTERN_FUNC(sub_82B6A550);
REXCVAR_DEFINE_BOOL(em2_native_memory_copy,true,"CPU","Accelerate native copy-loop prefixes while retaining original register results and tails");
REXCVAR_DEFINE_BOOL(em2_benchmark_memory_copy,false,"Debug","Benchmark native copy acceleration in the same-executable regression mode");

void em2::SkipNativeCopyPrefix(PPCContext& ctx,uint8_t* base,unsigned step) {
    const auto iterations=ctx.ctr.u64;
    if(iterations<2 || iterations>0x100000000ull/step) return;
    const uint64_t size=(iterations-1)*step;
    if(size<24) return;
    // The 128-byte unrolled loop also starts at +8 after its predecrement.
    const unsigned first=step==128?8:step;
    const uint32_t source=ctx.r4.u32+first,destination=ctx.r3.u32+first;
    const auto source_end=uint64_t(source)+size,destination_end=uint64_t(destination)+size;
    const auto contiguous=[](uint32_t start,uint64_t end) {
        return end<=0x100000000ull &&
            PPC_PHYS_HOST_OFFSET(start)==PPC_PHYS_HOST_OFFSET(uint32_t(end-1)) &&
            !(uint64_t(start)<0x80000000ull && end>0x7F000000ull);
    };
    if(!contiguous(source,source_end) || !contiguous(destination,destination_end)) return;
    if(uint64_t(source)<destination_end && uint64_t(destination)<source_end) return;
    // Different physical heaps may alias the same physical pages. Keep the
    // native order there rather than assuming disjoint virtual addresses suffice.
    if(source>=0xA0000000u && destination>=0xA0000000u &&
       (source>>29)!=(destination>>29)) return;
    if((source_end>0xA0000000ull && (source>>29)!=(uint32_t(source_end-1)>>29)) ||
       (destination_end>0xA0000000ull && (destination>>29)!=(uint32_t(destination_end-1)>>29)))return;
    std::memcpy(PPC_RAW_ADDR(destination),PPC_RAW_ADDR(source),size_t(size));
    ctx.r3.u32+=uint32_t(size);
    ctx.r4.u32+=uint32_t(size);
    ctx.ctr.u64=1;
    // Execute the original final iteration: it produces the exact scratch
    // registers, condition fields, final pointer values and CTR seen by callers.
}

bool em2::TestNativeMemoryCopy() {
    constexpr size_t low_size=0x80000;
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100010000ull,MEM_RESERVE,PAGE_NOACCESS));
    const auto saved=REXCVAR_GET(em2_native_memory_copy);
    struct Release {
        uint8_t* base; bool setting;
        ~Release(){if(base)VirtualFree(base,0,MEM_RELEASE);REXCVAR_SET(em2_native_memory_copy,setting);}
    } release{base,saved};
    if(!base || !VirtualAlloc(base,low_size,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0xDFFF0000,0x30000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0xFFFF0000,0x20000,MEM_COMMIT,PAGE_READWRITE)) return false;
    struct Region {uint32_t address; size_t size;};
    const std::array<Region,3> regions={{{0,low_size},{0xDFFF0000u,0x30000},{0xFFFF0000u,0x20000}}};
    std::vector<uint8_t> initial(low_size+0x50000),expected(initial.size());
    uint32_t random=0xE21C360;
    for(auto& byte:initial) {random=random*1664525u+1013904223u;byte=uint8_t(random>>24);}
    const auto restore=[&] {
        size_t offset=0;
        for(const auto region:regions) {
            std::memcpy(base+region.address,initial.data()+offset,region.size);
            offset+=region.size;
        }
    };
    unsigned comparisons=0;
    const auto compare=[&](uint32_t destination,uint32_t source,uint32_t size) {
        PPCContext initial_context{};
        initial_context.r1.u64=0x8000;
        const uint64_t pointer_tag=(size&1)?0xFFFFFFFF00000000ull:0;
        initial_context.r3.u64=destination|pointer_tag;
        initial_context.r4.u64=source|pointer_tag;initial_context.r5.u64=size;
        initial_context.r6.u64=initial_context.r7.u64=initial_context.r8.u64=initial_context.r9.u64=
            initial_context.r10.u64=initial_context.r11.u64=initial_context.r12.u64=0xFEDCBA9876543210ull;
        initial_context.ctr.u64=0x123456789ABCDEF0ull;
        initial_context.xer.so=(source&4)!=0;
        auto expected_context=initial_context;
        restore();
        __imp__sub_82B6A550(expected_context,base);
        size_t offset=0;
        for(const auto region:regions) {
            std::memcpy(expected.data()+offset,base+region.address,region.size);
            offset+=region.size;
        }
        for(bool enabled:{false,true}) {
            restore();
            REXCVAR_SET(em2_native_memory_copy,enabled);
            auto actual=initial_context;
            sub_82B6A550(actual,base);
            bool memory_equal=true;
            offset=0;
            for(const auto region:regions) {
                memory_equal&=std::memcmp(expected.data()+offset,base+region.address,region.size)==0;
                offset+=region.size;
            }
            if(!memory_equal || std::memcmp(&expected_context,&actual,sizeof(actual))) {
                REXLOG_ERROR("Native memcpy mismatch: dst {:08X}, src {:08X}, bytes {}, enabled {}, memory {}",destination,source,size,enabled,memory_equal);
                return false;
            }
            ++comparisons;
        }
        return true;
    };
    for(uint32_t size:{0u,1u,3u,4u,7u,8u,9u,15u,16u,17u,31u,32u,63u,64u,65u,127u,128u,129u,255u,256u,257u,511u,512u,513u,1023u,4096u,65536u})
      for(unsigned destination_alignment=0;destination_alignment<16;++destination_alignment)
       for(unsigned source_alignment=0;source_alignment<8;++source_alignment)
        if(!compare(0x40000+destination_alignment,0x10000+source_alignment,size)) return false;
    for(uint32_t size:{32u,64u,129u,256u,4096u})
      for(int delta:{-128,-64,-16,-8,-1,0,1,8,16,64,128})
        if(!compare(uint32_t(0x10000+delta),0x10000,size)) return false;
    for(uint32_t address:{0xDFFFFF80u,0xE0000000u,0xE0000004u,0xFFFFFF80u})
      for(uint32_t size:{64u,128u,256u,512u}) {
        if(!compare(0x40000,address,size) || !compare(address,0x10000,size)) return false;
      }
    // These candidates must leave every register unchanged and avoid touching
    // inaccessible MMIO / differently aliased physical addresses altogether.
    for(const auto addresses:std::array<std::array<uint32_t,2>,9>{{
        {0xE0001000u,0xA0001000u},{0x40000u,0x7F000000u},
        {0xA0001000u,0xBFFFFFF0u},{0xBFFFFFF0u,0xA0001000u},
        {0xE0001000u,0x9FFFFFF0u},{0x9FFFFFF0u,0xE0001000u},
        {0x40000u,0xDFFFFFF0u},{0x40000u,0xFFFFFFF0u},{0x10008u,0x10000u}}}) {
        PPCContext ctx{};
        ctx.r3.u64=addresses[0]-8;ctx.r4.u64=addresses[1]-8;ctx.ctr.u64=4;
        const auto expected_context=ctx;
        SkipNativeCopyPrefix(ctx,base,8);
        if(std::memcmp(&ctx,&expected_context,sizeof(ctx))) return false;
    }
    if(REXCVAR_GET(em2_benchmark_memory_copy)) {
        for(uint32_t size:{0u,4u,8u,16u,32u,64u,128u,256u,4096u,65536u}) {
            std::array<double,2> elapsed{};
            std::array<uint64_t,2> sums{};
            for(unsigned trial=0;trial<6;++trial) for(unsigned phase=0;phase<2;++phase) {
                const auto mode=(phase+trial)%2;
                REXCVAR_SET(em2_native_memory_copy,mode!=0);
                PPCContext ctx{}; ctx.r1.u64=0x8000;
                const auto start=std::chrono::steady_clock::now();
                for(unsigned i=0;i<20000;++i) {
                    ctx.r3.u64=0x40000;ctx.r4.u64=0x10000;ctx.r5.u64=size;
                    sub_82B6A550(ctx,base);
                    sums[mode]+=ctx.r3.u64+PPC_LOAD_U8(0x40000+(size?i%size:0));
                }
                elapsed[mode]+=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count();
            }
            if(sums[0]!=sums[1]) return false;
            REXLOG_INFO("Native memcpy benchmark {} bytes: original {:.1f}, optimized {:.1f} ns/call, {:.2f}x routine speedup (not game FPS)",size,elapsed[0]/120000,elapsed[1]/120000,elapsed[0]/elapsed[1]);
        }
    }
    REXLOG_INFO("Native memcpy matched original memory and full guest context in {} comparisons",comparisons);
    return true;
}

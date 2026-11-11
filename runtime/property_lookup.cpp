#include "property_lookup.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <immintrin.h>
#include <windows.h>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>

PPC_EXTERN_FUNC(__imp__sub_825E1D10);
REXCVAR_DEFINE_BOOL(em2_native_property_lookup,true,"CPU","Vectorize native pointer-key property lookup, preserving first-match and guest register results");
REXCVAR_DEFINE_BOOL(em2_benchmark_property_lookup,false,"Debug","Benchmark property lookup in the existing headless regression mode");

namespace {
void Lookup(PPCContext& ctx,uint8_t* base) {
    const auto object=ctx.r3.u32;
    const uint32_t key=PPC_LOAD_U32(ctx.r4.u32);
    ctx.r6.u64=key;
    const uint32_t empty=PPC_LOAD_U32(0x8203D278);
    ctx.r11.u64=empty;
    ctx.cr6.compare<uint32_t>(0,0,ctx.xer);
    if(key==empty) {ctx.r3.u64=0; return;}
    const uint32_t count=PPC_LOAD_U16(object+24);
    ctx.r7.u64=count; ctx.r10.u64=0;
    if(!count) {ctx.r3.u64=0; return;}
    const uint32_t data=PPC_LOAD_U32(object+20);
    ctx.r8.u64=data;
    unsigned index=0;
    // Each eight-byte record is {value,key}. Compare only odd 32-bit lanes,
    // in guest byte order, and return the first match even for duplicate keys.
    // Fall back for guest wrap or the SDK's discontinuous E0 physical mapping.
    const auto end=uint64_t(data)+uint64_t(count)*8;
    if(end<=0x100000000ull &&
       PPC_PHYS_HOST_OFFSET(data)==PPC_PHYS_HOST_OFFSET(uint32_t(end-1))) {
        const auto needle=_mm256_set1_epi32(int(__builtin_bswap32(key)));
        for(;index+4<=count;index+=4) {
            const auto records=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(PPC_RAW_ADDR(data+index*8)));
            const auto equal=_mm256_cmpeq_epi32(records,needle);
            const unsigned mask=unsigned(_mm256_movemask_ps(_mm256_castsi256_ps(equal)))&0xAAu;
            if(mask) {index+=std::countr_zero(mask)/2; goto done;}
        }
    }
    for(;index<count && PPC_LOAD_U32(uint32_t(data+index*8+4))!=key;++index) {}
done:
    ctx.r10.u64=index;
    ctx.r11.u64=index*8;
    ctx.r9.u64=index<count?uint64_t(key):uint64_t(data)+uint64_t(index)*8;
    ctx.r3.u64=index<count?PPC_LOAD_U32(uint32_t(data+index*8)):0;
}
}

PPC_FUNC_IMPL(sub_825E1D10) {
    if(REXCVAR_GET(em2_native_property_lookup)) Lookup(ctx,base);
    else __imp__sub_825E1D10(ctx,base);
}

bool em2::TestNativePropertyLookup() {
    // The SDK's final physical page needs host padding beyond guest 4 GiB.
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100010000ull,MEM_RESERVE,PAGE_NOACCESS));
    const auto saved=REXCVAR_GET(em2_native_property_lookup);
    struct Release {
        uint8_t* base; bool setting;
        ~Release() {if(base) VirtualFree(base,0,MEM_RELEASE); REXCVAR_SET(em2_native_property_lookup,setting);}
    } release{base,saved};
    if(!base || !VirtualAlloc(base,0x100000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0x82030000,0x10000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0xDFFF0000,0x30000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0xFFFF0000,0x20000,MEM_COMMIT,PAGE_READWRITE)) return false;
    constexpr uint32_t object=0x1000,key_pointer=0x2000,empty=0xFEE1DEAD;
    PPC_STORE_U32(0x8203D278,empty);
    unsigned comparisons=0;
    const auto compare=[&](uint32_t key,unsigned count,unsigned alignment,bool duplicate) {
        PPC_STORE_U32(key_pointer,key);
        PPCContext initial{};
        initial.r3.u64=object; initial.r4.u64=key_pointer;
        initial.r6.u64=initial.r7.u64=initial.r8.u64=initial.r9.u64=initial.r10.u64=initial.r11.u64=0xFEDCBA9876543210ull;
        initial.r14.u64=0x123456789ABCDEF0ull; initial.r31.u64=0x9876543210ABCDEFull;
        initial.lr=0xFFFFFFFF829D8228ull; initial.ctr.u64=0x1122334455667788ull;
        initial.xer.so=duplicate;
        auto expected=initial;
        __imp__sub_825E1D10(expected,base);
        // Exercise both production entry paths, not just the private SIMD helper.
        for(bool enabled : {false,true}) {
            REXCVAR_SET(em2_native_property_lookup,enabled);
            auto actual=initial;
            sub_825E1D10(actual,base);
            if(std::memcmp(&expected,&actual,sizeof(actual))) {
                REXLOG_ERROR("Native property lookup register mismatch: count {}, alignment {}, duplicate {}, key {:08X}, enabled {}",count,alignment,duplicate,key,enabled);
                return false;
            }
            ++comparisons;
        }
        return true;
    };
    for(unsigned count : {0u,1u,2u,3u,4u,5u,7u,8u,9u,15u,16u,17u,31u,32u,33u,63u,64u,65u,127u,128u,255u,256u,257u,1023u,1024u,4095u,4096u,65535u})
        for(unsigned alignment=0;alignment<8;++alignment)
            for(bool duplicate : {false,true}) {
                const auto data=0x10000+alignment*4;
                PPC_STORE_U32(object+20,data); PPC_STORE_U16(object+24,uint16_t(count));
                for(unsigned i=0;i<count;++i) {
                    PPC_STORE_U32(data+i*8,i?0x76540000u+i:0);
                    PPC_STORE_U32(data+i*8+4,0x12340000u+i);
                }
                if(duplicate && count>1) PPC_STORE_U32(data+(count-1)*8+4,0x12340000u);
                for(uint32_t key : {empty,0u,0xABCDEF01u,0x12340000u,
                                   0x12340000u+count/2,0x12340000u+(count?count-1:0)}) {
                    if(!compare(key,count,alignment,duplicate)) return false;
                }
            }
    // A real wrapping table exercises the scalar fallback and the original
    // routine's 64-bit final r9 carry on a miss. Also test a valid zero key.
    for(uint32_t data : {0xFFFFFFF0u,0xDFFFFFF0u,0xE0000000u,0xE0000004u})
      for(unsigned count : {1u,2u,3u,4u,5u,8u}) {
        PPC_STORE_U32(object+20,data); PPC_STORE_U16(object+24,uint16_t(count));
        for(unsigned i=0;i<count;++i) {
            PPC_STORE_U32(uint32_t(data+i*8),i+1);
            PPC_STORE_U32(uint32_t(data+i*8+4),i);
        }
        for(uint32_t key : {0u,count/2,count-1,empty,0xABCDEF01u})
            if(!compare(key,count,0,false)) return false;
    }
    PPC_STORE_U32(0x8203D278,0);
    if(!compare(0,8,0,true)) return false;
    PPC_STORE_U32(0x8203D278,empty);
    if(REXCVAR_GET(em2_benchmark_property_lookup)) {
        for(unsigned count : {32u,64u,256u}) {
            constexpr uint32_t data=0x10000;
            PPC_STORE_U32(object+20,data); PPC_STORE_U16(object+24,uint16_t(count));
            for(unsigned i=0;i<count;++i) {PPC_STORE_U32(data+i*8,i+1); PPC_STORE_U32(data+i*8+4,0x12340000u+i);}
            std::array<double,2> totals{};
            std::array<uint64_t,2> sums{};
            for(unsigned trial=0;trial<6;++trial) for(unsigned phase=0;phase<2;++phase) {
                const auto mode=(phase+trial)%2;
                REXCVAR_SET(em2_native_property_lookup,mode!=0);
                PPCContext ctx{}; ctx.r4.u64=key_pointer;
                const auto start=std::chrono::steady_clock::now();
                for(unsigned iteration=0;iteration<20000;++iteration) {
                    const auto position=iteration%4;
                    PPC_STORE_U32(key_pointer,position==3?0xABCDEF01u:0x12340000u+(position==0?0:position==1?count/2:count-1));
                    ctx.r3.u64=object;
                    sub_825E1D10(ctx,base);
                    sums[mode]+=ctx.r3.u64;
                }
                totals[mode]+=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count();
            }
            if(sums[0]!=sums[1]) return false;
            REXLOG_INFO("Property lookup benchmark {} records, first/middle/last/miss: original {:.1f}, native {:.1f} ns/call, {:.2f}x routine speedup (not game FPS)",
                count,totals[0]/120000,totals[1]/120000,totals[0]/totals[1]);
        }
    }
    REXLOG_INFO("Native property lookup matched the original full guest context in {} comparisons",comparisons);
    return true;
}

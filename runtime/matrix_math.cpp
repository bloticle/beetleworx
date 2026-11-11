#include "matrix_math.h"
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <immintrin.h>
#include <vector>
#include <windows.h>

PPC_EXTERN_FUNC(__imp__sub_82B9D350);
PPC_EXTERN_FUNC(sub_82B9D350);
REXCVAR_DEFINE_BOOL(em2_native_matrix_stores,true,"CPU","Coalesce native matrix output stores while preserving arithmetic results and registers");
REXCVAR_DEFINE_BOOL(em2_benchmark_matrix_math,false,"Debug","Benchmark native matrix output stores in the existing headless regression mode");

bool em2::TestNativeMatrixMath() {
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100010000ull,MEM_RESERVE,PAGE_NOACCESS));
    struct Restore {
        uint8_t* base;bool enabled=REXCVAR_GET(em2_native_matrix_stores);unsigned csr=_mm_getcsr();
        ~Restore(){REXCVAR_SET(em2_native_matrix_stores,enabled);_mm_setcsr(csr);if(base)VirtualFree(base,0,MEM_RELEASE);}
    } restore{base};
    struct Region {uint32_t offset;size_t size;std::vector<uint8_t> before,expected;};
    std::array<Region,3> regions={Region{0,0x10000},Region{0xDFFF0000,0x30000},Region{0xFFFF0000,0x11000}};
    if(!base)return false;
    uint64_t random=0x82B9D350360;
    auto next=[&](){random^=random<<13;random^=random>>7;random^=random<<17;return random;};
    for(auto& region:regions) {
        if(!VirtualAlloc(base+region.offset,region.size,MEM_COMMIT,PAGE_READWRITE))return false;
        region.before.resize(region.size);region.expected.resize(region.size);
        for(auto& byte:region.before)byte=uint8_t(next());
    }
    unsigned comparisons=0;
    auto run=[&](uint32_t output,uint32_t a,uint32_t b,unsigned pattern,bool boundary) {
        PPCContext initial{};
        auto* gprs=reinterpret_cast<uint8_t*>(&initial.r0);
        for(unsigned i=0;i<32;++i){const auto value=next();std::memcpy(gprs+i*8,&value,8);}
        for(auto* bytes=reinterpret_cast<uint8_t*>(&initial.v0);bytes<reinterpret_cast<uint8_t*>(&initial.v127)+16;++bytes)*bytes=uint8_t(next());
        initial.r1.u64=0xF000;initial.r3.u64=output;initial.r4.u64=a;initial.r5.u64=b;
        if(pattern==1){initial.r3.u64|=next()&0xFFFFFFFF00000000ull;initial.r4.u64|=next()&0xFFFFFFFF00000000ull;initial.r5.u64|=next()&0xFFFFFFFF00000000ull;}
        initial.lr=next();initial.ctr.u64=next();initial.xer.so=pattern&1;initial.xer.ca=(pattern>>1)&1;
        initial.cr6.compare<uint32_t>(uint32_t(next()),uint32_t(next()),initial.xer);
        initial.fpscr.InitHost();if(pattern&1)initial.fpscr.enableFlushMode();
        const auto initial_csr=_mm_getcsr();
        const unsigned count=boundary?3:1;
        for(unsigned i=0;i<count;++i)std::memcpy(base+regions[i].offset,regions[i].before.data(),regions[i].size);
        auto expected=initial;__imp__sub_82B9D350(expected,base);const auto expected_csr=_mm_getcsr();
        for(unsigned i=0;i<count;++i)std::memcpy(regions[i].expected.data(),base+regions[i].offset,regions[i].size);
        for(bool enabled:{false,true}) {
            for(unsigned i=0;i<count;++i)std::memcpy(base+regions[i].offset,regions[i].before.data(),regions[i].size);
            auto actual=initial;REXCVAR_SET(em2_native_matrix_stores,enabled);_mm_setcsr(initial_csr);
            sub_82B9D350(actual,base);
            bool same=std::memcmp(&actual,&expected,sizeof(actual))==0 && _mm_getcsr()==expected_csr;
            for(unsigned i=0;i<count;++i)same&=std::memcmp(base+regions[i].offset,regions[i].expected.data(),regions[i].size)==0;
            if(!same){
                REXLOG_ERROR("Native matrix mismatch: output {:08X}, inputs {:08X}/{:08X}, pattern {}, enabled {}, MXCSR {:08X}/{:08X}",output,a,b,pattern,enabled,_mm_getcsr(),expected_csr);
                const auto* actual_bytes=reinterpret_cast<const uint8_t*>(&actual);const auto* expected_bytes=reinterpret_cast<const uint8_t*>(&expected);
                unsigned shown=0;for(size_t i=0;i<sizeof(actual) && shown<8;++i)if(actual_bytes[i]!=expected_bytes[i]){REXLOG_ERROR("Matrix context byte {}: {:02X}/{:02X}",i,actual_bytes[i],expected_bytes[i]);++shown;}
                for(unsigned v=0;v<128;++v) {
                    const auto* av=reinterpret_cast<const uint32_t*>(&actual.v0)+v*4;
                    const auto* ev=reinterpret_cast<const uint32_t*>(&expected.v0)+v*4;
                    if(std::memcmp(av,ev,16))REXLOG_ERROR("Matrix v{} actual {:08X} {:08X} {:08X} {:08X}, expected {:08X} {:08X} {:08X} {:08X}",v,av[0],av[1],av[2],av[3],ev[0],ev[1],ev[2],ev[3]);
                }
                for(unsigned r=0;r<count;++r)for(size_t i=0;i<regions[r].size;++i)if(base[regions[r].offset+i]!=regions[r].expected[i]){REXLOG_ERROR("Matrix memory {:08X}: {:02X}/{:02X}",uint32_t(regions[r].offset+i),base[regions[r].offset+i],regions[r].expected[i]);break;}
                return false;
            }
            ++comparisons;
        }
        return true;
    };
    // Normal finite values, arbitrary bits, then signed zero/denormals/NaNs.
    constexpr std::array<uint32_t,12> special={0,0x80000000,1,0x80000001,0x007FFFFF,0x00800000,
        0x3F800000,0xBF800000,0x7F800000,0xFF800000,0x7FC12345,0x7F812345};
    constexpr std::array<std::array<uint32_t,3>,6> layouts={
        std::array<uint32_t,3>{0x4000,0x2000,0x3000},{0x2000,0x2000,0x3000},
        {0x3000,0x2000,0x3000},{0x2010,0x2000,0x2020},
        {0xEFC0,0x2000,0x3000},{0x4000,0xEFD0,0x3000}};
    for(unsigned pattern=0;pattern<3;++pattern) {
        for(size_t i=0;i<regions[0].size;i+=4) {
            uint32_t word;
            if(pattern==0)word=__builtin_bswap32(std::bit_cast<uint32_t>(float(int(next()%33)-16)*0.125f));
            else if(pattern==1)word=uint32_t(next());
            else word=__builtin_bswap32(special[(i/4)%special.size()]);
            std::memcpy(regions[0].before.data()+i,&word,4);
        }
        for(auto layout:layouts)for(unsigned a=0;a<16;++a)for(unsigned b=0;b<16;++b)
            for(unsigned out=0;out<16;++out)
                if(!run(layout[0]+out,layout[1]+a,layout[2]+b,pattern,false))return false;
        // The following low addresses end exactly at the committed block's
        // guard page, including byte-unaligned destinations rounded by stvewx.
        for(uint32_t out:{0xFFC0u,0xFFC1u,0xFFC3u})
            if(!run(out,0x2001,0x300F,pattern,false))return false;
        if(!run(0x4000,0xFFC0,0xFFC0,pattern,false))return false;
        for(uint32_t out:{0xDFFFFFC0u,0xDFFFFFE0u,0xE0000040u,0xFFFFFFC0u,0xFFFFFFE0u})
            if(!run(out,0x2001,0x300F,pattern,true))return false;
    }
    for(uint32_t out:{0x7EFFFFE0u,0x7F000000u,0x7FFFFFF0u,0xDFFFFFE0u,0xFFFFFFE0u})
        if(NativeMatrixStoresSupported(out)){REXLOG_ERROR("Unsafe matrix output accepted: {:08X}",out);return false;}
    REXLOG_INFO("Native matrix stores matched original full context, fixture memory and MXCSR in {} comparisons: all input byte alignments, aligned/unaligned output, aliasing, stack overlap, raw FP bits, physical-gap/wrap and both modes",comparisons);
    if(REXCVAR_GET(em2_benchmark_matrix_math)) {
        using Clock=std::chrono::steady_clock;
        // Stable disjoint finite matrices; output is consumed by the external
        // routine and retained in fixture memory. No guest gameplay executes.
        constexpr unsigned rounds=7,iterations=200000;
        for(auto alignment:std::array<std::array<unsigned,3>,4>{std::array<unsigned,3>{0,0,0},{0,1,7},{0,15,15},{4,0,0}}) {
            for(uint32_t address:{0x2000u+alignment[1],0x3000u+alignment[2]})for(unsigned i=0;i<16;++i) {
                const auto word=__builtin_bswap32(std::bit_cast<uint32_t>(float(i+1)*0.125f));
                std::memcpy(PPC_RAW_ADDR(address+4*i),&word,4);
            }
            std::array<double,rounds> original{},optimized{};
            for(unsigned round=0;round<rounds;++round)for(unsigned order=0;order<2;++order) {
                const bool native=((round+order)&1)!=0;REXCVAR_SET(em2_native_matrix_stores,true);
                auto* function=native?sub_82B9D350:__imp__sub_82B9D350;
                PPCContext ctx{};ctx.r1.u64=0xF000;ctx.r3.u64=0x4000+alignment[0];ctx.r4.u64=0x2000+alignment[1];ctx.r5.u64=0x3000+alignment[2];ctx.fpscr.InitHost();
                const auto begin=Clock::now();
                // The native helper overwrites r4/r5, so each call restores
                // exactly its two input arguments (same work in both modes).
                for(unsigned i=0;i<iterations;++i){ctx.r4.u64=0x2000+alignment[1];ctx.r5.u64=0x3000+alignment[2];function(ctx,base);}
                (native?optimized:original)[round]=std::chrono::duration<double,std::nano>(Clock::now()-begin).count()/iterations;
            }
            std::sort(original.begin(),original.end());std::sort(optimized.begin(),optimized.end());
            REXLOG_INFO("Native matrix benchmark output/input byte offsets {}/{}/{}: original {:.2f}, optimized {:.2f} ns/call, {:.3f}x (7 alternating rounds; not gameplay FPS)",
                alignment[0],alignment[1],alignment[2],original[3],optimized[3],original[3]/optimized[3]);
        }
    }
    return true;
}

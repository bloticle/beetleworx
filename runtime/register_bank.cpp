#include "register_bank.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <immintrin.h>
#include <windows.h>
#include <array>
#include <chrono>
#include <cstring>
#include <vector>

REXCVAR_DEFINE_BOOL(em2_native_register_bank,true,"CPU","SIMD native integer/floating register save/restore with original boundary fallback");
REXCVAR_DEFINE_BOOL(em2_benchmark_register_bank,false,"Debug","Benchmark register banks in headless regression mode");
namespace {
static_assert(sizeof(PPCRegister)==8 && offsetof(PPCContext,r31)-offsetof(PPCContext,r14)==17*8);
static_assert(offsetof(PPCContext,f31)-offsetof(PPCContext,f14)==17*8);
template<unsigned First,bool Save,bool Floating=false> bool Transfer(PPCContext& ctx,uint8_t* base) {
    constexpr unsigned bytes=(32-First)*8;
    const auto address=uint32_t((Floating?ctx.r12.u32:ctx.r1.u32)-bytes-(Floating?0:8));
    const auto end=uint64_t(address)+bytes+(Floating?0:4);
    if(end>0x100000000ull || PPC_PHYS_HOST_OFFSET(address)!=PPC_PHYS_HOST_OFFSET(uint32_t(end-1)))return false;
    auto* guest=PPC_RAW_ADDR(address);
    auto* registers=reinterpret_cast<uint8_t*>(&ctx)+(Floating?offsetof(PPCContext,f14):offsetof(PPCContext,r14))+(First-14)*8;
    // lfd/stfd helpers also update the runtime's cached host flush mode.
    // Copy raw payload bits, including NaNs, without FP arithmetic.
    if constexpr(Floating)ctx.fpscr.disableFlushMode();
    const auto swap=_mm256_setr_epi8(7,6,5,4,3,2,1,0,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,15,14,13,12,11,10,9,8);
    unsigned offset=0;
    for(;offset+32<=bytes;offset+=32) {
        const auto value=_mm256_loadu_si256(reinterpret_cast<const __m256i*>((Save?registers:guest)+offset));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>((Save?guest:registers)+offset),_mm256_shuffle_epi8(value,swap));
    }
    if constexpr(bytes%32>=16) {
        const auto value=_mm_loadu_si128(reinterpret_cast<const __m128i*>((Save?registers:guest)+offset));
        _mm_storeu_si128(reinterpret_cast<__m128i*>((Save?guest:registers)+offset),_mm_shuffle_epi8(value,_mm256_castsi256_si128(swap)));
        offset+=16;
    }
    if constexpr(bytes%16) {
        uint64_t value;std::memcpy(&value,(Save?registers:guest)+offset,8);value=__builtin_bswap64(value);
        std::memcpy((Save?guest:registers)+offset,&value,8);
    }
    if constexpr(!Floating) {
        if constexpr(Save)PPC_STORE_U32(ctx.r1.u32-8,ctx.r12.u32);
        else {ctx.r12.u64=PPC_LOAD_U32(ctx.r1.u32-8);ctx.lr=ctx.r12.u64;}
    }
    return true;
}
}
#define EM2_BANKS(X) X(14) X(15) X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27)
#define EM2_BANK(N) \
 PPC_EXTERN_FUNC(__imp____savegprlr_##N); PPC_EXTERN_FUNC(__imp____restgprlr_##N); \
 PPC_FUNC_IMPL(__savegprlr_##N) {if(!REXCVAR_GET(em2_native_register_bank) || !Transfer<N,true>(ctx,base))__imp____savegprlr_##N(ctx,base);} \
 PPC_FUNC_IMPL(__restgprlr_##N) {if(!REXCVAR_GET(em2_native_register_bank) || !Transfer<N,false>(ctx,base))__imp____restgprlr_##N(ctx,base);}
EM2_BANKS(EM2_BANK)
#undef EM2_BANK
#define EM2_FLOAT_SAVE_BANKS(X) X(14) X(15) X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26)
#define EM2_FLOAT_DECLARE(N) PPC_EXTERN_FUNC(__imp____savefpr_##N); PPC_EXTERN_FUNC(__imp____restfpr_##N); PPC_EXTERN_FUNC(__savefpr_##N);
EM2_BANKS(EM2_FLOAT_DECLARE)
#define EM2_FLOAT_SAVE(N) PPC_FUNC_IMPL(__savefpr_##N) {if(!REXCVAR_GET(em2_native_register_bank) || !Transfer<N,true,true>(ctx,base))__imp____savefpr_##N(ctx,base);}
#define EM2_FLOAT_RESTORE(N) PPC_FUNC_IMPL(__restfpr_##N) {if(!REXCVAR_GET(em2_native_register_bank) || !Transfer<N,false,true>(ctx,base))__imp____restfpr_##N(ctx,base);}
// Five-register floating saves measured equal to the original. Leave that
// symbol entirely untouched rather than adding a wrapper to an unhelpful path.
EM2_FLOAT_SAVE_BANKS(EM2_FLOAT_SAVE)
EM2_BANKS(EM2_FLOAT_RESTORE)
#undef EM2_FLOAT_DECLARE
#undef EM2_FLOAT_SAVE
#undef EM2_FLOAT_RESTORE

bool em2::TestNativeRegisterBank() {
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100010000ull,MEM_RESERVE,PAGE_NOACCESS));
    const bool enabled=REXCVAR_GET(em2_native_register_bank);
    struct Release {uint8_t* base;bool enabled;unsigned csr=_mm_getcsr();~Release(){REXCVAR_SET(em2_native_register_bank,enabled);_mm_setcsr(csr);if(base)VirtualFree(base,0,MEM_RELEASE);}} release{base,enabled};
    constexpr std::array<std::array<uint64_t,2>,3> ranges={{{0,0x10000},{0xDFFF0000,0x30000},{0xFFFF0000,0x11000}}};
    if(!base)return false;
    for(auto range:ranges)if(!VirtualAlloc(base+range[0],range[1],MEM_COMMIT,PAGE_READWRITE))return false;
    constexpr size_t size=0x51000;
    std::vector<uint8_t> before(size),expected_memory(size);
    auto copy=[&](uint8_t* buffer,bool to_guest) {
        size_t offset=0;for(auto range:ranges){std::memcpy(to_guest?base+range[0]:buffer+offset,to_guest?buffer+offset:base+range[0],range[1]);offset+=range[1];}
    };
#define ORIGINAL_SAVE(N) __imp____savegprlr_##N,
#define ORIGINAL_RESTORE(N) __imp____restgprlr_##N,
#define NATIVE_SAVE(N) __savegprlr_##N,
#define NATIVE_RESTORE(N) __restgprlr_##N,
    const std::array<PPCFunc*,14> saves={EM2_BANKS(ORIGINAL_SAVE)},restores={EM2_BANKS(ORIGINAL_RESTORE)},
        native_saves={EM2_BANKS(NATIVE_SAVE)},native_restores={EM2_BANKS(NATIVE_RESTORE)};
#define ORIGINAL_FLOAT_SAVE(N) __imp____savefpr_##N,
#define ORIGINAL_FLOAT_RESTORE(N) __imp____restfpr_##N,
#define NATIVE_FLOAT_SAVE(N) __savefpr_##N,
#define NATIVE_FLOAT_RESTORE(N) __restfpr_##N,
    const std::array<PPCFunc*,14> float_saves={EM2_BANKS(ORIGINAL_FLOAT_SAVE)},float_restores={EM2_BANKS(ORIGINAL_FLOAT_RESTORE)},
        native_float_saves={EM2_BANKS(NATIVE_FLOAT_SAVE)},native_float_restores={EM2_BANKS(NATIVE_FLOAT_RESTORE)};
    uint64_t random=0x1234E21C360;
    auto next=[&](){random^=random<<13;random^=random>>7;random^=random<<17;return random;};
    for(auto& b:before)b=uint8_t(next());
    unsigned comparisons=0;
    for(bool floating:{false,true})for(unsigned bank=0;bank<14;++bank)for(bool save:{false,true})for(uint32_t stack:{0x1000u,0x1001u,0x80u,0xE0000040u})for(unsigned seed=0;seed<8;++seed) {
        PPCContext initial{};
        auto* registers=reinterpret_cast<uint8_t*>(&initial)+(floating?offsetof(PPCContext,f14):offsetof(PPCContext,r14));
        for(unsigned i=0;i<18;++i){auto value=next();std::memcpy(registers+i*8,&value,8);}
        initial.r1.u64=floating?0x12345678:stack;initial.r12.u64=floating?stack:next();initial.lr=next();initial.ctr.u64=next();initial.xer.so=seed%2;initial.fpscr.InitHost();
        if(seed%2)initial.fpscr.enableFlushMode();
        const auto initial_csr=_mm_getcsr();
        const auto original=(floating?(save?float_saves:float_restores):(save?saves:restores))[bank];
        const auto replacement=(floating?(save?native_float_saves:native_float_restores):(save?native_saves:native_restores))[bank];
        copy(before.data(),true);auto expected=initial;original(expected,base);const auto expected_csr=_mm_getcsr();copy(expected_memory.data(),false);
        for(bool optimized:{false,true}) {
            copy(before.data(),true);auto actual=initial;REXCVAR_SET(em2_native_register_bank,optimized);_mm_setcsr(initial_csr);
            replacement(actual,base);
            bool same=std::memcmp(&actual,&expected,sizeof(actual))==0 && _mm_getcsr()==expected_csr;
            size_t offset=0;for(auto range:ranges){same&=std::memcmp(base+range[0],expected_memory.data()+offset,range[1])==0;offset+=range[1];}
            if(!same){REXLOG_ERROR("Native register bank mismatch: floating {}, first {}, save {}, stack {:08X}, seed {}, optimized {}",floating,bank+14,save,stack,seed,optimized);return false;}
            ++comparisons;
        }
    }
    REXLOG_INFO("Native integer/floating register banks matched original context, guest memory and MXCSR in {} comparisons, including flush modes, wrap and physical-map boundaries",comparisons);
    if(REXCVAR_GET(em2_benchmark_register_bank)) {
        for(bool floating:{false,true})for(unsigned bank=0;bank<14;++bank)for(bool save:{false,true}) {
            std::array<double,2> totals{};
            PPCContext context{};context.r1.u64=context.r12.u64=0x1000;context.fpscr.InitHost();
            for(unsigned trial=0;trial<6;++trial)for(unsigned phase=0;phase<2;++phase) {
                const unsigned mode=(phase+trial)%2;REXCVAR_SET(em2_native_register_bank,mode!=0);
                const auto function=mode?(floating?(save?native_float_saves:native_float_restores):(save?native_saves:native_restores))[bank]:
                    (floating?(save?float_saves:float_restores):(save?saves:restores))[bank];
                const auto start=std::chrono::steady_clock::now();
                for(unsigned i=0;i<200000;++i)function(context,base);
                totals[mode]+=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count();
            }
            REXLOG_INFO("Register bank benchmark floating {}, first {}, save {}: original {:.2f}, SIMD {:.2f} ns/call, {:.2f}x routine speedup; not game FPS",floating,bank+14,save,totals[0]/1200000,totals[1]/1200000,totals[0]/totals[1]);
        }
    }
    return true;
}

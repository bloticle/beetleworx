#include "native_turn_rate_defaults.h"
#include "online_behavior_trace.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <windows.h>
#include <immintrin.h>
#include <array>
#include <cstring>

PPC_EXTERN_FUNC(__imp__sub_82759898);
PPC_EXTERN_FUNC(__imp__sub_82790848);
PPC_EXTERN_FUNC(__imp__sub_829FCBD8);
PPC_EXTERN_FUNC(__imp__sub_82AB7EA8);
PPC_EXTERN_FUNC(__imp__sub_83200830);
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(bool,em2_behavior_trace_turn_limits);
REXCVAR_DECLARE(bool,em2_behavior_trace_turn_lifetime);
namespace {
void ConstructMotion(PPCContext& ctx,uint8_t* base,PPCFunc* original,uint32_t constructor=0){
    const auto motion=ctx.r3.u32;
    original(ctx,base);
    struct ReturnEnvironment {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~ReturnEnvironment(){_mm_setcsr(csr);SetLastError(error);}} returned;
    // All four native Motion constructor copies leave this gap uninitialized.
    // Three inline copies bypass82790848 entirely. Navigation assembly can
    // read Moving Turn Rate before the later component initializer runs, and
    // caches that value for the first solver step. Use the native attribute
    // schema defaults at construction, before assembly can observe the object.
    const bool trace=constructor && REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_behavior_trace_turn_lifetime);
    const auto prior_stationary=trace?PPC_LOAD_U32(motion+856):0;
    const auto prior_moving=trace?PPC_LOAD_U32(motion+860):0;
    PPC_STORE_U32(motion+856,0);
    PPC_STORE_U32(motion+860,0);
    if(trace){
        REXLOG_INFO("Native Motion constructor schema defaults: constructor {:08X} motion {:08X} table {:08X} previous {:08X}/{:08X} defaults 00000000/00000000",constructor,motion,PPC_LOAD_U32(motion),prior_stationary,prior_moving);
    }
}
// Both native float schema factories (82790D10/82790D88) initialize these
// attributes to +0. Their four consumers ignore lookup failure and multiply
// the output by 2*pi, otherwise consuming stale/uninitialized Motion fields.
uint32_t TurnRateOffset(uint32_t caller){
    switch(caller){
    case 0x8278719C:case 0x8278C7CC:return 848; // Stationary Turn Rate
    case 0x827871D4:case 0x8278C804:return 852; // Moving Turn Rate
    default:return 0;
    }
}
void ReadTurnRate(PPCContext& ctx,uint8_t* base,PPCFunc* original){
    const auto caller=uint32_t(ctx.lr),face=ctx.r3.u32,output=ctx.r5.u32,index=ctx.r6.u32;
    original(ctx,base);
    if(index)return;
    const auto offset=TurnRateOffset(caller);
    if(!offset || face<8 || uint64_t(face)+offset>UINT32_MAX || output!=face+offset)return;
    // These exact native callers own this output. Preserve the native failure
    // result and complete returned context; only define its missing float.
    const auto previous=PPC_LOAD_U32(output);
    const bool defaulted=!ctx.r3.u32;
    if(defaulted)PPC_STORE_U32(output,0);
    if(REXCVAR_GET(em2_behavior_trace_turn_lifetime))em2::net::ObserveNativeTurnRateRead(ctx,base,caller,face,output,ctx.r3.u32,previous,defaulted);
    if(defaulted && REXCVAR_GET(em2_private_profile) && REXCVAR_GET(em2_behavior_trace_turn_limits)){
        const auto csr=_mm_getcsr();const auto error=GetLastError();
        REXLOG_INFO("Native turn rate schema default: caller {:08X} motion {:08X} destination {:08X} previous {:08X} default 00000000",caller,face-8,output,previous);
        _mm_setcsr(csr);SetLastError(error);
    }
}
}
PPC_FUNC_IMPL(sub_82759898){
    if(!TurnRateOffset(uint32_t(ctx.lr))){__imp__sub_82759898(ctx,base);return;}
    ReadTurnRate(ctx,base,__imp__sub_82759898);
}
PPC_FUNC_IMPL(sub_82790848){ConstructMotion(ctx,base,__imp__sub_82790848,0x82790848);}
PPC_FUNC_IMPL(sub_829FCBD8){ConstructMotion(ctx,base,__imp__sub_829FCBD8,0x829FCBD8);}
PPC_FUNC_IMPL(sub_82AB7EA8){ConstructMotion(ctx,base,__imp__sub_82AB7EA8,0x82AB7EA8);}
PPC_FUNC_IMPL(sub_83200830){ConstructMotion(ctx,base,__imp__sub_83200830,0x83200830);}

bool em2::TestNativeTurnRateDefaults(){
    struct CallerEnvironment {unsigned csr=_mm_getcsr();DWORD error=GetLastError();~CallerEnvironment(){_mm_setcsr(csr);SetLastError(error);}} caller;
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x10000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!base)return false;
    struct Release{uint8_t* p;~Release(){VirtualFree(p,0,MEM_RELEASE);}}release{base};
    static unsigned calls=0;
    const auto missing=+[](PPCContext& ctx,uint8_t*){++calls;ctx.r3.u64=0;ctx.r11.u64=0x1122334455667788ull;};
    const auto present=+[](PPCContext& ctx,uint8_t* base){++calls;PPC_STORE_U32(ctx.r5.u32,0x3FC00000);ctx.r3.u64=1;ctx.r11.u64=0x8877665544332211ull;};
    unsigned checked=0;
    const auto constructor=+[](PPCContext& ctx,uint8_t* base){
        ++calls;PPC_STORE_U32(ctx.r3.u32+844,0x11223344);PPC_STORE_U32(ctx.r3.u32+864,0x55667788);
        ctx.r11.u64=0x8877665544332211ull;
        _mm_setcsr(_mm_getcsr()|0x20);SetLastError(456);
    };
    for(auto stale:std::array<uint32_t,5>{0,0x3F7C50A5,0x3EE91AA5,0x7FC01234,0xDEADBEEF}){
        PPCContext initial{};initial.r3.u64=0x2000;initial.r14.u64=0xABCDEF0123456789ull;
        initial.f1.u64=0x400921FB54442D18ull;initial.lr=0x827F1A60;
        PPC_STORE_U32(0x2000+856,stale);PPC_STORE_U32(0x2000+860,stale);
        const auto incoming_csr=_mm_getcsr();const auto incoming_error=GetLastError();
        auto expected=initial;constructor(expected,base);const auto expected_csr=_mm_getcsr();const auto expected_error=GetLastError();
        _mm_setcsr(incoming_csr);SetLastError(incoming_error);
        auto actual=initial;const auto before=calls;
        ConstructMotion(actual,base,constructor,0x83200830);
        if(calls!=before+1 || std::memcmp(&expected,&actual,sizeof(actual)) || _mm_getcsr()!=expected_csr || GetLastError()!=expected_error ||
           PPC_LOAD_U32(0x2000+856) || PPC_LOAD_U32(0x2000+860) ||
           PPC_LOAD_U32(0x2000+844)!=0x11223344 || PPC_LOAD_U32(0x2000+864)!=0x55667788){
            REXLOG_ERROR("Native Motion turn-rate construction test failed: stale {:08X}",stale);return false;
        }
    }
    for(auto caller:std::array<uint32_t,5>{0x8278719C,0x827871D4,0x8278C7CC,0x8278C804,0x82888B38})
    for(auto stale:std::array<uint32_t,4>{0,0x3EE91AA5,0x7FC01234,0xDEADBEEF})
    for(bool success:{false,true})for(bool indexed:{false,true})for(bool displaced:{false,true}){
        PPCContext initial{};initial.lr=caller;initial.r3.u64=0x1008;initial.r6.u64=indexed;
        const auto offset=TurnRateOffset(caller);initial.r5.u64=0x1008+(offset?offset:848)+(displaced?4:0);
        initial.r14.u64=0xABCDEF0123456789ull;initial.f1.u64=0x400921FB54442D18ull;
        const auto output=initial.r5.u32;PPC_STORE_U32(output,stale);
        auto expected=initial;auto* lookup=success?present:missing;
        lookup(expected,base);const auto original_value=PPC_LOAD_U32(output);
        PPC_STORE_U32(output,stale);auto actual=initial;const auto before=calls;
        ReadTurnRate(actual,base,lookup);
        const bool fallback=!success && !indexed && !displaced && offset;
        if(calls!=before+1 || std::memcmp(&expected,&actual,sizeof(actual)) || PPC_LOAD_U32(output)!=(fallback?0:original_value)){
            REXLOG_ERROR("Native turn-rate default test failed: caller {:08X}, success {}, indexed {}, displaced {}",caller,success,indexed,displaced);return false;
        }
        ++checked;
    }
    REXLOG_INFO("Native turn-rate defaults passed {} checked lookup cases: four native callers, stale/NaN outputs, retained configured values, unrelated/indexed/displaced rejection, exactly one lookup and complete returned PPC context (injected lookup; native gameplay separate)",checked);
    REXLOG_INFO("Native Motion construction passed five stale-heap cases: schema defaults before navigation assembly, adjacent native fields and complete constructor return preserved (injected constructor; native gameplay separate)");
    return true;
}

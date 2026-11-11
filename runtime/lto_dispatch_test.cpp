#include "lto_dispatch_test.h"
#include "ppc_context.h"
#include <rex/logging.h>
#include <array>
#include <immintrin.h>

PPC_EXTERN_FUNC(EM2LtoOriginal);
PPC_EXTERN_FUNC(EM2LtoDecoratedOriginal);
PPC_EXTERN_FUNC(EM2LtoSameUnitOverride);
PPC_EXTERN_FUNC(EM2LtoSameUnitFallback);
PPC_EXTERN_FUNC(EM2LtoSameUnitDecorated);
PPC_EXTERN_FUNC(EM2LtoFallback);
extern PPCFunc* EM2LtoDispatchTable[];

PPC_FUNC_IMPL(EM2LtoOverridden) {ctx.r3.u64=0xFFEEDDCCBBAA0099ull;}
PPC_FUNC_IMPL(EM2LtoDecorated) {EM2LtoDecoratedOriginal(ctx,base);ctx.r3.u64^=0x0101010101010101ull;}

bool em2::TestLinkTimeDispatch() {
    struct Case {PPCFunc* call;uint64_t expected;};
    const std::array cases={
        Case{EM2LtoSameUnitOverride,0xFFEEDDCCBBAA0099ull},
        Case{EM2LtoSameUnitFallback,0x8877665544332211ull},
        Case{EM2LtoSameUnitDecorated,0x133557799BBDDFF1ull},
        Case{EM2LtoOriginal,0x1020304050607080ull},
        Case{EM2LtoDecoratedOriginal,0x123456789ABCDEF0ull},
        Case{EM2LtoOverridden,0xFFEEDDCCBBAA0099ull},
        Case{EM2LtoFallback,0x8877665544332211ull},
        Case{EM2LtoDecorated,0x133557799BBDDFF1ull},
        Case{EM2LtoDispatchTable[0],0xFFEEDDCCBBAA0099ull},
        Case{EM2LtoDispatchTable[1],0x8877665544332211ull},
        Case{EM2LtoDispatchTable[2],0x133557799BBDDFF1ull}};
    const auto csr=_mm_getcsr();
    for(unsigned i=0;i<cases.size();++i) {
        PPCContext ctx{};ctx.r3.u64=0xDEADBEEFull;cases[i].call(ctx,nullptr);
        if(ctx.r3.u64!=cases[i].expected || _mm_getcsr()!=csr) {
            REXLOG_ERROR("Link-time dispatch fixture {} failed: got {:016X}, expected {:016X}",i,ctx.r3.u64,cases[i].expected);return false;
        }
    }
    REXLOG_INFO("Link-time dispatch passed {} same-TU, cross-TU, original, decorated and mapping-table cases",cases.size());
    return true;
}

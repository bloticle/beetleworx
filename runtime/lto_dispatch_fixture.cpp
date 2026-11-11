#include "ppc_context.h"

PPC_EXTERN_FUNC(EM2LtoOverridden);
PPC_EXTERN_FUNC(EM2LtoFallback);
PPC_EXTERN_FUNC(EM2LtoDecorated);

PPC_FUNC_IMPL(EM2LtoOriginal) {ctx.r3.u64=0x1020304050607080ull;}
PPC_WEAK_FUNC(EM2LtoOverridden) {EM2LtoOriginal(ctx,base);}
PPC_WEAK_FUNC(EM2LtoFallback) {ctx.r3.u64=0x8877665544332211ull;}
PPC_FUNC_IMPL(EM2LtoDecoratedOriginal) {ctx.r3.u64=0x123456789ABCDEF0ull;}
PPC_WEAK_FUNC(EM2LtoDecorated) {EM2LtoDecoratedOriginal(ctx,base);}

// Calls beside the weak definitions are the important interposition case:
// early inlining must never bake these fallback bodies into their callers.
PPC_FUNC_IMPL(EM2LtoSameUnitOverride) {EM2LtoOverridden(ctx,base);}
PPC_FUNC_IMPL(EM2LtoSameUnitFallback) {EM2LtoFallback(ctx,base);}
PPC_FUNC_IMPL(EM2LtoSameUnitDecorated) {EM2LtoDecorated(ctx,base);}
PPCFunc* EM2LtoDispatchTable[]={EM2LtoOverridden,EM2LtoFallback,EM2LtoDecorated};

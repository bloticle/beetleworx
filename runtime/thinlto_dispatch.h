#pragma once
#include <rex/runtime/guest/context.h>

#if defined(EM2_THIN_LTO_DISPATCH)
// Keep weak interposition during compilation. ThinLTO resolves native strong
// overrides before optimizing; only then may it inline the selected wrapper.
// The ordinary build retains the SDK's noinline dispatch policy.
#undef PPC_WEAK_FUNC
#define PPC_WEAK_FUNC(x) __attribute__((weak)) PPC_FUNC(x)
#endif

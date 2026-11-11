#pragma once
// Use the runtime's context layout for every translated function and host export.
// XenonRecomp copies this header into generated/ppc/ppc_context.h.
#include <rex/runtime/guest/context.h>
#include <simde/simde-f16.h>
using namespace rex::runtime::guest;

// XenonUtils already names Xbox imports __imp__*. ReXGlue exports those with
// C linkage; use the same linkage for generated declarations and definitions.
// A translated call can enter a throwing C++ host callback. Explicitly retain
// that contract: /EHsc otherwise treats implicit C-linkage calls as nonthrowing
// and can remove catches/RAII cleanup in their callers. Link names are unchanged.
#undef PPC_EXTERN_FUNC
#define PPC_EXTERN_FUNC(x) extern "C" PPC_FUNC(x) noexcept(false)
#undef PPC_FUNC_IMPL
#define PPC_FUNC_IMPL(x) extern "C" PPC_FUNC(x) noexcept(false)

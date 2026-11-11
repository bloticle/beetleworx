#include "ppc_recomp_shared.h"
#include <rex/kernel/kernel_state.h>
#include <rex/kernel/xfile.h>
using rex::X_STATUS;

PPC_FUNC_IMPL(__imp__NtCancelIoFile) {
    // ReXGlue 0.1's Read/Write paths complete synchronously even for handles
    // opened with asynchronous flags. There is no pending host request to cancel.
    auto file = ctx.kernel_state->object_table()->LookupObject<rex::kernel::XFile>(ctx.r3.u32);
    const uint32_t status = file ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE;
    if (ctx.r4.u32) {
        PPC_STORE_U32(ctx.r4.u32, status);
        PPC_STORE_U32(ctx.r4.u32 + 4, 0u);
    }
    ctx.r3.u64 = status;
}

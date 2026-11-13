#pragma once
#include "ppc_context.h"
#include <cstdint>
namespace em2::net {
void BeginNativeBehaviorTraceTick(uint64_t tick) noexcept;
void ResetNativeBehaviorTrace() noexcept;
void DumpNativeBehaviorTrace() noexcept;
// Called AFTER the unchanged native rand wrapper. Observes only the current
// original chooser frame and its two audited return addresses.
void ObserveNativeBehaviorRandom(uint32_t caller,PPCContext&,uint8_t* base) noexcept;
// Existing strong Motion owner calls this AFTER its existing preparation.
// original must be nonnull. Executes it exactly once; never changes admission.
// Private TLS-ring observations include explicitly labelled between-tick calls.
void RunNativeMotionTrace(PPCContext&,uint8_t* base,PPCFunc* original);
}

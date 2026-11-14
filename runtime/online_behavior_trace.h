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
// The existing motion/basis dispatch remains authoritative. Calls next once,
// then privately observes the incoming command and its resulting direction.
void RunNativeMotionInputTrace(PPCContext&,uint8_t* base,PPCFunc* next);
// Optional lifetime observation of the four native turn-rate init lookups.
void ObserveNativeTurnRateRead(PPCContext&,uint8_t* base,uint32_t caller,uint32_t face,uint32_t output,uint32_t success,uint32_t value,bool defaulted) noexcept;
}

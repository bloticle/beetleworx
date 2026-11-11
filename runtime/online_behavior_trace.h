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
}

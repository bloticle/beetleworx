#pragma once
#include <cstdint>
#include <string_view>
struct PPCContext;
namespace em2::em1 {
// Observational only; caller invokes before the existing raw-resource hook.
void ObserveSingleGsaRawLookup(PPCContext& ctx,uint8_t* base,uint32_t manager,uint32_t path);
void ObserveArchitectureCameraSnapshot(PPCContext& ctx,uint8_t* base,uint32_t controller,std::string_view phase);
}

#pragma once
#include "em1_rvz.h"
#include <span>
struct PPCContext;
namespace em2 { struct NativeActor; }

namespace em2::em1 {
struct OriginalInkPak {
  std::vector<uint8_t> bytes;
  uint32_t children = 0;
  uint32_t ribbon_models = 0;
};

// The caller supplies the original mounted EM2 _Dynamic.pak and the registered
// replacement technique for the authored Wii ribbon material.
bool BuildOriginalMickeyInkPak(std::shared_ptr<RvzSource> source,
    std::span<const uint8_t> retail, std::string_view ribbon_technique,
    OriginalInkPak& output, std::string& error);
void SetOriginalInkPakMounted(bool);
bool OriginalInkPakMounted();
void UpdateOriginalInkDrips(PPCContext&,uint8_t*,const NativeActor& mickey);
}

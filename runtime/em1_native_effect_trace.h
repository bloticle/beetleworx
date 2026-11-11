#pragma once
#include <cstdint>
namespace em2::em1 {
bool NativeEm1BlendEffectsLoaded();
bool NativeEm1DualUvEffectLoaded();
bool NativeEm1CutoutEffectLoaded();
void ObserveEm1FinalMaterialFlags(uint8_t* base);
}

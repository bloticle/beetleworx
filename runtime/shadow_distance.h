#pragma once
#include <array>
#include <cstdint>

namespace em2 {
// Menu choices: fixed, quality, balanced, performance. Application is deferred
// naturally to native light preparation; no renderer restart is required.
void ConfigureNativeShadowDistance(unsigned choice);
unsigned NativeShadowDistanceSetting();
// Runs after native light selection. Uses the actual camera inputs of retail
// 827A58A8, not texture slot numbers as a substitute for camera cascades.
void UpdateNativeShadowDistance(uint32_t caller,uint32_t owner,uint32_t selected,
                                uint32_t camera_count,uint32_t cameras,uint8_t* base);
std::array<uint32_t,3> ShadowDistanceTargetSizes(uint32_t owner,std::array<uint32_t,3> maximum);
bool TestShadowDistancePolicy();
}

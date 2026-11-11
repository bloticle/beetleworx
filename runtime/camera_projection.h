#pragma once
#include <cstdint>
namespace em2 {
bool IsNormalGameplayCamera(uint32_t controller,uint8_t* base);
bool IsGameplayViewCameraNode(uint32_t node,uint8_t* base) noexcept;
void ResetGameplayViewCameras() noexcept;
bool TestNativeCameraProjection();
}

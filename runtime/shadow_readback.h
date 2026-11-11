#pragma once
#include <cstdint>
namespace em2 {
// Private, one-shot numeric GPU readback. No image or gameplay input.
void ProbeNativeShadowPixels(uint8_t* base,uint32_t click) noexcept;
// Last validated one-shot probe address, for correlating shader fetch metadata.
// Not ownership or a guarantee that the texture remains current after resizing.
uint32_t ShadowPixelProbeAddress() noexcept;
}

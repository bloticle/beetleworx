#pragma once
#include <array>
#include <cstdint>

namespace em2 {
// Private rendering diagnostic only. Temporarily supplies an existing authored
// light to the original shadow preparation function, then restores its input.
// Creates no entities and never changes the original source asset or camera.
class NativeShadowCastProbe {
public:
    NativeShadowCastProbe(uint32_t caller,uint32_t owner,uint32_t selected,uint8_t* base);
    ~NativeShadowCastProbe();
    NativeShadowCastProbe(const NativeShadowCastProbe&)=delete;
    NativeShadowCastProbe& operator=(const NativeShadowCastProbe&)=delete;
private:
    friend bool TestNativeShadowCastProbe();
    uint8_t* base_=nullptr;
    uint32_t selected_=0;
    std::array<uint32_t,3> original_{};
};
bool TestNativeShadowCastProbe();
}

#pragma once
#include <cstdint>
#include <span>
namespace em2 {
// 0 disables Gaussian output,1 retains authored fetch-state sampling,2 linear.
void ConfigureBloomSampling(unsigned mode);
unsigned NativeBloomSampling();
std::span<const uint32_t> DisabledBloomShader();
// Input/output are GPU big-endian words. Empty means preserve input verbatim.
std::span<const uint32_t> SelectNativeBloomShader(const uint32_t*,size_t,bool pixel);
bool TestBloomKernelSelection();
}

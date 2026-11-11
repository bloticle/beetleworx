#pragma once
namespace em2 {
void ConfigureShadowKernel(unsigned samples);
unsigned NativeShadowSamples();
const char* NativeShadowCacheKey();
bool TestShadowKernelSelection();
}

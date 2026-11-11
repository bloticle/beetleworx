#pragma once
#include <filesystem>
namespace em2 {
// Optional, intrusive wall-time instruction sampling, not a benchmark.
class NativeRenderSampleScope {
public:
    NativeRenderSampleScope();
    ~NativeRenderSampleScope();
    NativeRenderSampleScope(const NativeRenderSampleScope&)=delete;
    NativeRenderSampleScope& operator=(const NativeRenderSampleScope&)=delete;
private:
    bool active_=false;
};
// Separate opt-in scope for startup/video work outside the render callback.
class NativeMainSampleScope {
public:
    NativeMainSampleScope();
    ~NativeMainSampleScope();
    NativeMainSampleScope(const NativeMainSampleScope&)=delete;
    NativeMainSampleScope& operator=(const NativeMainSampleScope&)=delete;
private:
    bool active_=false;
};
bool TestNativeRenderSampler(const std::filesystem::path& output);
}

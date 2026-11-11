#pragma once
#include <cstdint>
namespace em2 {
// NativeLinear bypasses native output gamma; guest-authored nonlinear math is
// retained. Compatibility preserves the original post-shader colour mirror.
enum class HdrSceneEncoding:uint32_t{kCompatibility,kNativeLinear};
// Encoding expected by an unsigned legacy guest texture fetch. Canonical HDR
// colour stays before native output gamma; this selects its separate read view.
enum class HdrGuestTransfer:uint32_t{kRaw,kXenosPwl,kSrgb};
}

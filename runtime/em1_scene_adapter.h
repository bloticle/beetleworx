#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace em2::em1 {
// Original EM1 bytes stay owned by the source mount. This result is transient
// loader input (or a versioned cache entry), never a replacement source asset.
struct SceneAdapterResult {
  std::vector<uint8_t> bytes;
  std::array<uint8_t, 16> source_uuid{};
  uint32_t entity_count = 0;
  uint32_t component_count = 0;
};
inline constexpr uint32_t kSceneAdapterVersion = 2;
// Includes original local IO subscription -> native Instigator `_self` defaults
// where the original owning layer has rows and no explicit Instigator field.
// Other component behavior and platform dependencies
// must be resolved by the origin-aware asset loader before activating a scene.
bool AdaptScene(std::span<const uint8_t> source, SceneAdapterResult& result,
                std::string& error);
bool TestSceneAdapterIOCompatibility();
}  // namespace em2::em1

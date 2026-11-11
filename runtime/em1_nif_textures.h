#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace em2::em1 {
struct NifTextureResult {
  std::vector<uint8_t> bytes;
  uint32_t textures = 0;
  uint32_t remaining_wii_display_lists = 0;
};
// Original EM1-origin NIF only: preserve opaque blocks and decode Wii textures
// into portable RGBA NiPixelData in memory. This does not convert mesh streams,
// skinning, animation, materials, physics, or prove EM2 renderer compatibility.
bool AdaptNifTextures(std::span<const uint8_t> original,
                       NifTextureResult& result, std::string& error);
bool TestNifContainerCompatibility(std::string& error);
}

#pragma once
#include <cstdint>
#include <span>
#include <vector>
namespace em2 {
// Internal archive bytes only: never extracts paths or changes original files.
std::vector<uint8_t> ApplyRegionalResourcePatch(std::span<const uint8_t> original,std::span<const uint8_t> patch);
bool TestRegionalResourcePatches();
}

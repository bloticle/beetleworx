#pragma once
#include <cstdint>
#include <span>
#include <vector>
namespace em2::em1 {
std::vector<uint8_t> DeflateNativePakResource(std::span<const uint8_t> bytes);
}

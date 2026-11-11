#pragma once
#include <atomic>
#include <cstdint>

// Counts calls to the game's D3D presentation routine, not host UI redraws
// or completed GPU frames. Keep submission timing separate from UI timing.
namespace em2 {
extern std::atomic<uint64_t> present_count;
extern std::atomic<uint64_t> maximum_present_gap_us;
extern std::atomic<uint64_t> maximum_native_present_us;
}

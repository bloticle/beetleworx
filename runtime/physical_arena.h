#pragma once
#include <cstdint>
#include <mutex>
namespace em2 {
std::recursive_mutex& NativePhysicalArenaMutex();
struct PhysicalArenaStats {
    uint32_t total=0,used=0,free=0,largest_aligned=0,blocks=0;
};
// Diagnostic snapshot, not a reservation. Callers must synchronize native
// allocation before using this for admission; another thread may allocate.
// This does not reserve metadata nodes or validate other graphics pools.
bool ReadPhysicalArenaStats(uint8_t* base,uint32_t descriptor,uint32_t alignment,PhysicalArenaStats& result);
bool TestPhysicalArenaStats();
}

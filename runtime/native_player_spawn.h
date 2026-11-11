#pragma once
#include <array>
#include <cstdint>
#include <string>
struct PPCContext;
namespace em2 {
// Initialized game thread only. Borrows an existing character's native hull;
// performs native shape sweeps without moving it or admitting another actor.
bool FindNativePlayerSpawn(PPCContext&,uint8_t*,uint32_t prototype_entity,
    const std::array<float,3>& preferred,std::array<float,3>& grounded,std::string& error);
bool TestNativePlayerSpawn();
}

#pragma once
#include "online.h"
#include "ppc_context.h"
namespace em2 {
bool ReadNativeWorldIdentity(uint8_t* base,net::GameplayObservation::World&);
// Tick-owner read. Invalid/missing world memory never publishes partial output.
// Memory validation does not pin objects against concurrent engine destruction.
bool ReadNativeGameplayObservation(PPCContext&,uint8_t* base,net::GameplayObservation&);
bool ReadNativeResources(PPCContext&,uint8_t* base,const std::array<uint32_t,2>& entities,
                         std::array<uint32_t,4>& life,std::array<float,6>& ammo);
bool TestNativeResources();
}

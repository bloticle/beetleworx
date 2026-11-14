#pragma once
#include "online.h"
namespace em2 {
// Completed-tick owner only. No native callbacks, retained pointers or writes.
// Covers BasicMotion character proxies, not rigid props, Lua or all world state.
bool ReadNativeCharacterPhysics(uint8_t* base,std::vector<net::GameplayObservation::CharacterPhysics>& out);
// Same completed-tick contract, limited to one currently owned entity.
bool ReadNativeActorPhysics(uint8_t* base,uint32_t entity,net::GameplayObservation::CharacterPhysics& out);
bool TestNativeCharacterPhysics();
}

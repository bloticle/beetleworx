#pragma once
#include "online.h"
struct PPCContext;
namespace em2 {
// Completed-tick owner only. No native callbacks, retained pointers or writes.
// Covers BasicMotion character proxies, not rigid props, Lua or all world state.
bool ReadNativeCharacterPhysics(uint8_t* base,std::vector<net::GameplayObservation::CharacterPhysics>& out);
// Same completed-tick contract, limited to one currently owned entity.
bool ReadNativeActorPhysics(uint8_t* base,uint32_t entity,net::GameplayObservation::CharacterPhysics& out);
// Game-thread completed boundary only. Uses the original rigid-body position,
// velocity and entity transform setters; never copies a Havok object or sends
// native addresses over the wire. Caller resolves the current owned entity.
bool CorrectNativeActorPhysics(PPCContext&,uint8_t* base,uint32_t entity,
    const std::array<float,3>& position,const std::array<float,3>& velocity);
bool TestNativeCharacterPhysics();
}

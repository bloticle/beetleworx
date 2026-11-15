#pragma once
#include <cstdint>
#include <optional>
struct PPCContext;
namespace em2 {
// Completed game-thread boundary. Original Health-property dispatch; a living
// actor corrected to zero uses the retail Damage-export sequence so native
// death listeners run immediately. No raw component writes. General damage
// stimulus, instigator and combat-effect replication remains separate work.
std::optional<int32_t> ReadNativeActorHealth(PPCContext&,uint8_t* base,uint32_t entity);
bool CorrectNativeActorHealth(PPCContext&,uint8_t* base,uint32_t entity,int32_t health);
}

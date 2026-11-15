#pragma once
#include <cstdint>
#include <optional>
struct PPCContext;
namespace em2 {
// Owner-thread native damage invoked while applying a host correction.
bool NativeDamageReplayActive()noexcept;
// Dispatch a fully constructed native event under the scoped host authority
// lease. Caller must validate its local owner, manager and event storage.
void DispatchNativeAuthorityDamage(PPCContext&,uint8_t*);
// Completed game-thread boundary. Original Health-property dispatch; a living
// actor corrected to zero uses the retail Damage-export sequence so native
// death listeners run immediately. No raw component writes. General damage
// stimulus, instigator and combat-effect replication remains separate work.
std::optional<int32_t> ReadNativeActorHealth(PPCContext&,uint8_t* base,uint32_t entity);
bool CorrectNativeActorHealth(PPCContext&,uint8_t* base,uint32_t entity,int32_t health);
}

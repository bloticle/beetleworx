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
// Original Health-property dispatch, with no raw component writes. BeforeUpdate
// stages zero before native processing. CompletedPropertyOnly reconciles an
// end-of-update property after that update, without inventing a lethal Damage
// event; original health/death processing then runs on the next native tick.
// Legacy CompletedUpdate retains immediate Damage-export notification.
enum class NativeHealthCorrectionPhase {CompletedUpdate,BeforeUpdate,CompletedPropertyOnly};
std::optional<int32_t> ReadNativeActorHealth(PPCContext&,uint8_t* base,uint32_t entity);
bool CorrectNativeActorHealth(PPCContext&,uint8_t* base,uint32_t entity,int32_t health,
    NativeHealthCorrectionPhase phase=NativeHealthCorrectionPhase::CompletedUpdate);
}

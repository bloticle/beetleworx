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
// stages authoritative zero for the upcoming original health/death processing;
// it must not be used after the native update has already completed. Completed
// correction retains the immediate retail Damage-export notification path.
enum class NativeHealthCorrectionPhase {CompletedUpdate,BeforeUpdate};
std::optional<int32_t> ReadNativeActorHealth(PPCContext&,uint8_t* base,uint32_t entity);
bool CorrectNativeActorHealth(PPCContext&,uint8_t* base,uint32_t entity,int32_t health,
    NativeHealthCorrectionPhase phase=NativeHealthCorrectionPhase::CompletedUpdate);
}

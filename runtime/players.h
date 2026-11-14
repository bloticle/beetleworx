#pragma once
#include "ppc_context.h"
#include "online.h"
namespace em2 {
bool TestNativePlayerActions();
bool TestCanonicalNativeJoin();
void ResetNativeSynchronizedJoinTiming();
struct NativeJoinTick {
    // Exact current published-controls generation, not a session pointer.
    uint64_t owner_generation=0;
    net::Frame raw_retail{};
    bool joining=false;
};
enum class NativeJoinResult {Inactive,OutsideAgreedTick,Unpublished,NotReady,Nested,Duplicate,Dispatched};
NativeJoinResult RunNativeSynchronizedJoin(PPCContext&,uint8_t*,const NativeJoinTick&);
// Legacy Session/InputGate adapter; new owners use the explicit context above.
void RunNativeSynchronizedJoin(PPCContext&,uint8_t*,uint16_t buttons);
void RequestCharacterSwap();
void UpdateCharacterSwap(PPCContext& ctx,uint8_t* base);
void UpdateNativeJoinedPlayerControls(PPCContext& ctx,uint8_t* base);
void MaybeQueueNativeStartLevel();
// Bounded read-only owner-thread diagnostics after an original physics step.
void TracePlayerSpawnPhysicsStep(uint8_t* base);
bool NativeCoopReady(uint8_t* base);
// Existing sessions may follow an original mode owned by the same camera.
bool NativeCoopRuntimeReady(uint8_t* base);
// Read-only diagnostic; nullptr means the original co-op readiness checks pass.
const char* NativeCoopReadinessFailure(uint8_t* base);
bool OswaldIsSelected();
bool NativeOnlineJoinAllowed(uint8_t* base);
const char* CharacterSwapNotice();
inline constexpr uint32_t kSelectedCharacterGetter=uint32_t(PPC_CODE_BASE+PPC_CODE_SIZE);
void ReadSelectedCharacterProperty(PPCContext& ctx,uint8_t* base);
}

#pragma once
#include "online_actor_input.h"
#include <cstdint>
#include <span>
namespace em2::net {
enum class OwnedJumpEdge : uint8_t { Pressed, Released };
struct NativeActorJump {
    uint64_t generation=0,tick=0,sequence=0;
    OwnedJumpEdge edge=OwnedJumpEdge::Pressed;
    NativeActorAction action{};
};
// Explicit NATIVE action edges, never a held raw Pad button. Sequence starts at
// 1 per native actor generation, increments exactly, and ticks never reverse.
// Deliver after this tick's Movement callback and before controller update.
// Applied means native delivery, not that the native ability accepted a jump.
ActorInputResult ValidateOwnedActorJump(uint64_t peer,const NativeActorJump&,PPCContext&,uint8_t* base);
// Preflight up to two same-tick edges against a private copy of edge history.
// No native callback, stage claim, or history write occurs (including failure).
ActorInputResult ValidateOwnedActorJumpBatch(uint64_t peer,std::span<const NativeActorJump>,PPCContext&,uint8_t* base);
ActorInputResult ReplayOwnedActorJump(uint64_t peer,const NativeActorJump&,PPCContext&,uint8_t* base);
// Only after native listener removal or world teardown; live ownership rejects.
// Idempotent when this generation has no retained history.
bool ForgetOwnedActorJump(uint64_t peer,uint64_t generation,PPCContext&,uint8_t* base);
// Read-only task activation snapshot for a native human entity, including retail.
bool ReadNativeJumpTaskFlags(uint32_t entity,uint8_t* base,uint32_t& flags);
// Private probe diagnostic; observes the authored task map without enabling tasks.
void LogNativeActorTaskInventory(uint32_t entity,uint8_t* base);
bool TestOwnedActorJumps(PPCContext&,uint8_t* base,uint64_t first_peer,uint64_t second_peer,void(*native_update)(PPCContext&,uint8_t*));
namespace detail {
// Input-module backend retains its private ReplayScope and current staged tick.
// A private permit exists only during ReplayOwnedActorJump; direct use rejects.
ActorInputResult DispatchOwnedActorJump(uint64_t peer,const NativeActorJump&,PPCContext&,uint8_t* base);
bool OwnedJumpDispatchAuthorized(uint64_t peer,const NativeActorJump&) noexcept;
ActorInputResult ValidateOwnedJumpTargets(uint64_t peer,const NativeActorJump&,PPCContext&,uint8_t* base);
}
}

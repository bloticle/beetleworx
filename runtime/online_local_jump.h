#pragma once
#include "online_local_movement.h"
#include "online_movement_frame.h"
#include <array>
#include <cstdint>

namespace em2::net {
struct NativeLocalJumpAction {
    uint32_t event=0,action=0,index=0,uid=0,kind=0,key=0;
    std::array<uint32_t,10> shape{};
    bool operator==(const NativeLocalJumpAction&)const=default;
};
struct NativeLocalJumpBinding {
    NativeLocalMovementBinding movement{};
    std::array<NativeLocalJumpAction,2> actions{}; // Jump, ReleaseJump.
    uint32_t button_count=0,buttons=0;
    uint16_t button_mask=0;
    bool operator==(const NativeLocalJumpBinding&)const=default;
};
struct NativeLocalJumpEvents {
    std::array<RoomJumpEvent,kMaxRoomJumpEvents> events{};
    uint16_t button_mask=0;
    uint8_t count=0;
    bool operator==(const NativeLocalJumpEvents&)const=default;
};
// Read-only selected retail context discovery; does not dispatch Jump or imply
// that native abilities accepted it. Requires a currently retained Movement lease.
bool DiscoverNativeLocalJumpBinding(const NativeLocalMovementBinding&,PPCContext&,uint8_t*,NativeLocalJumpBinding&);
// Explicit game-thread stream ownership. actor_epoch is authoritative, first_tick
// is the producer/source tick (before transport delay). Default is unarmed.
bool ArmNativeLocalJump(const NativeLocalJumpBinding&,uint64_t actor_epoch,uint64_t first_tick,PPCContext&,uint8_t*);
void DisarmNativeLocalJump();
// ORIGINAL native device/button and digital-action math, not a Pad bit-edge
// comparison. Identical repeated ticks return identical cached events; conflicting
// repeats, gaps, stale bindings and mismatched physical devices reject.
bool ProduceNativeLocalJump(const NativeLocalJumpBinding&,const Pad&,uint64_t actor_epoch,uint64_t source_tick,
    PPCContext&,uint8_t*,NativeLocalJumpEvents&,unsigned sampled_native_user=0);
bool ProduceNativeLocalJump(const Room&,const NativeLocalJumpBinding&,const NativeRoutedPadSample&,uint64_t actor_epoch,
    PPCContext&,uint8_t*,NativeLocalJumpEvents&);
bool TestNativeLocalJump();
}

#pragma once
#include "online.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <bit>

namespace em2::net {
inline constexpr std::size_t kOnlineRoomCapacity=16;

// Raw Pad is retained for native action production. Its axes are NOT valid
// movement values until the original native action interpreter has run.
// NativeActionV1 identifies post-interpreter x/y, including explicit neutral;
// neither transport nor the actor coordinator may infer it from Pad values.
enum class RoomMovementSource : uint8_t { UnconvertedPad=0, NativeActionV1=1, NativeActionV2=2 };
enum class RoomJumpEdge : uint8_t { None=0, Pressed=1, Released=2 };
inline constexpr std::size_t kMaxRoomJumpEvents=2;
struct RoomJumpEvent {
    uint64_t sequence=0;
    RoomJumpEdge edge=RoomJumpEdge::None;
    bool operator==(const RoomJumpEvent&)const=default;
};

struct RoomMovement {
    float x=0,y=0;
    // Ground-projected normalized native view forward; native motion derives
    // its own lateral axis. No guest pointer or shared retail camera slot.
    float forward_x=0,forward_z=1;
    Pad pad{};
    // Stable logical spawn incarnation assigned by the authoritative world.
    // Zero means unassigned. This is never a native entity handle/address.
    uint64_t actor_epoch=0;
    RoomMovementSource source=RoomMovementSource::UnconvertedPad;
    // V2 certifies native digital Jump evaluation, including held ticks with
    // no event. This mask comes from the selected native bindings, not an
    // assumed A-button mapping. Other raw intent remains unsupported by extras.
    uint16_t jump_button_mask=0;
    uint8_t jump_count=0;
    std::array<RoomJumpEvent,kMaxRoomJumpEvents> jump_events{};
    bool operator==(const RoomMovement& other)const{
        // Native floating-point input must echo bit-for-bit, including signed
        // zero; atan2 and later native arithmetic can distinguish its sign.
        return std::bit_cast<uint32_t>(x)==std::bit_cast<uint32_t>(other.x) &&
            std::bit_cast<uint32_t>(y)==std::bit_cast<uint32_t>(other.y) &&
            std::bit_cast<uint32_t>(forward_x)==std::bit_cast<uint32_t>(other.forward_x) &&
            std::bit_cast<uint32_t>(forward_z)==std::bit_cast<uint32_t>(other.forward_z) &&
            pad==other.pad && actor_epoch==other.actor_epoch && source==other.source &&
            jump_button_mask==other.jump_button_mask && jump_count==other.jump_count && jump_events==other.jump_events;
    }
};

// One admission rule shared by the network decoder and native coordinator.
inline bool ValidRoomMovement(const RoomMovement& value){
    if(value.source!=RoomMovementSource::NativeActionV1 && value.source!=RoomMovementSource::NativeActionV2)return false;
    if(value.jump_count>kMaxRoomJumpEvents)return false;
    if(value.source==RoomMovementSource::NativeActionV1 && (value.jump_count || value.jump_button_mask))return false;
    if(value.source==RoomMovementSource::NativeActionV2 && !std::has_single_bit(value.jump_button_mask))return false;
    for(unsigned i=0;i<kMaxRoomJumpEvents;++i){const auto& event=value.jump_events[i];
        if(i>=value.jump_count){if(event!=RoomJumpEvent{})return false;continue;}
        if(!event.sequence || (event.edge!=RoomJumpEdge::Pressed && event.edge!=RoomJumpEdge::Released))return false;
        if(i && (value.jump_events[i-1].sequence==UINT64_MAX || event.sequence!=value.jump_events[i-1].sequence+1 ||
            event.edge==value.jump_events[i-1].edge))return false;
    }
    return value.actor_epoch!=0 &&
        std::isfinite(value.x) && std::isfinite(value.y) && std::abs(value.x)<=1 && std::abs(value.y)<=1 &&
        std::isfinite(value.forward_x) && std::isfinite(value.forward_z) &&
        std::abs(value.forward_x)<=1 && std::abs(value.forward_z)<=1 &&
        std::abs(double(value.forward_x)*value.forward_x+double(value.forward_z)*value.forward_z-1)<1e-4;
}

// Per-incarnation semantic stream. A failed batch leaves history untouched.
// Actor epoch changes require a negotiated lifecycle reset outside this helper.
struct RoomJumpStream {
    uint64_t sequence=0;
    uint16_t button_mask=0;
    bool held=false,native=false;
    bool operator==(const RoomJumpStream&)const=default;
};
inline bool AdvanceRoomJumpStream(RoomJumpStream& state,const RoomMovement& value){
    if(!ValidRoomMovement(value))return false;
    if(value.source==RoomMovementSource::NativeActionV1)return !state.native;
    auto next=state;
    if(next.native && next.button_mask!=value.jump_button_mask)return false;
    next.native=true;next.button_mask=value.jump_button_mask;
    for(unsigned i=0;i<value.jump_count;++i){const auto& event=value.jump_events[i];
        if(next.sequence==UINT64_MAX || event.sequence!=next.sequence+1 ||
           (event.edge==RoomJumpEdge::Pressed)==next.held)return false;
        next.sequence=event.sequence;next.held=event.edge==RoomJumpEdge::Pressed;
    }
    state=next;return true;
}

struct RoomFrame {
    uint64_t tick=0;
    // Bit i owns players[i] under the agreed room roster. A cleared bit is
    // absent input, not an implicit neutral action or permission to advance.
    uint16_t players_mask=0;
    std::array<RoomMovement,kOnlineRoomCapacity> players{};
    bool operator==(const RoomFrame&) const = default;
};
}

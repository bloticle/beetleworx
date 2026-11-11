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
enum class RoomMovementSource : uint8_t { UnconvertedPad=0, NativeActionV1=1, NativeActionV2=2, NativeActionV3=3 };
// Digital interpreter outputs are booleans, ordered default context then active
// context and original ordinal. No guest address or guessed semantic ID travels.
struct RoomButtonActions {
    std::array<uint8_t,32> schema{};
    uint64_t source_tick=0;
    std::array<uint8_t,64> fired{}; // Two contexts, 256 original ordinals each.
    bool operator==(const RoomButtonActions&)const=default;
};
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
    RoomButtonActions button_actions;
    bool operator==(const RoomMovement& other)const{
        // Native floating-point input must echo bit-for-bit, including signed
        // zero; atan2 and later native arithmetic can distinguish its sign.
        return std::bit_cast<uint32_t>(x)==std::bit_cast<uint32_t>(other.x) &&
            std::bit_cast<uint32_t>(y)==std::bit_cast<uint32_t>(other.y) &&
            std::bit_cast<uint32_t>(forward_x)==std::bit_cast<uint32_t>(other.forward_x) &&
            std::bit_cast<uint32_t>(forward_z)==std::bit_cast<uint32_t>(other.forward_z) &&
            pad==other.pad && actor_epoch==other.actor_epoch && source==other.source &&
            jump_button_mask==other.jump_button_mask && jump_count==other.jump_count && jump_events==other.jump_events && button_actions==other.button_actions;
    }
};

// One admission rule shared by the network decoder and native coordinator.
inline bool ValidRoomMovement(const RoomMovement& value){
    if(value.source!=RoomMovementSource::NativeActionV1 && value.source!=RoomMovementSource::NativeActionV2 && value.source!=RoomMovementSource::NativeActionV3)return false;
    if(value.source==RoomMovementSource::NativeActionV3){
        if(value.button_actions.schema==std::array<uint8_t,32>{})return false;
        unsigned count=0;for(auto byte:value.button_actions.fired)count+=std::popcount(byte);
        if(count>256)return false;
    }else if(value.button_actions!=RoomButtonActions{})return false;
    if(value.jump_count>kMaxRoomJumpEvents)return false;
    if(value.source==RoomMovementSource::NativeActionV1 && (value.jump_count || value.jump_button_mask))return false;
    if(value.source!=RoomMovementSource::NativeActionV1 && !std::has_single_bit(value.jump_button_mask))return false;
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
    bool buttons=false,buttons_exhausted=false;
    uint64_t button_actor_epoch=0,next_button_tick=0;
    std::array<uint8_t,32> button_schema{};
    bool operator==(const RoomJumpStream&)const=default;
};
inline bool AdvanceRoomJumpStream(RoomJumpStream& state,const RoomMovement& value){
    if(!ValidRoomMovement(value))return false;
    if(value.source==RoomMovementSource::NativeActionV1)return !state.native;
    auto next=state;
    if(next.buttons && value.source!=RoomMovementSource::NativeActionV3)return false;
    if(value.source==RoomMovementSource::NativeActionV3){
        const auto& actions=value.button_actions;
        if(next.buttons && (next.buttons_exhausted || next.button_actor_epoch!=value.actor_epoch ||
            next.button_schema!=actions.schema || next.next_button_tick!=actions.source_tick))return false;
        next.buttons=true;next.button_actor_epoch=value.actor_epoch;next.button_schema=actions.schema;
        next.buttons_exhausted=actions.source_tick==UINT64_MAX;
        if(!next.buttons_exhausted)next.next_button_tick=actions.source_tick+1;
    }
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
enum class RetailButtonPhase { Invalid, StartupNeutral, NativeButtons };
// The two-retail-player V3 cutover keeps the negotiated delay seeds intact.
// A late legacy command, early button command or wrong source clock is never
// an implicit neutral frame. Receiver schemas/history are checked separately.
inline RetailButtonPhase ClassifyRetailButtonFrame(const RoomFrame& frame,uint64_t first,uint64_t delay){
    if(frame.players_mask!=3 || frame.tick<first || first>UINT64_MAX-delay)return RetailButtonPhase::Invalid;
    if(frame.tick<first+delay){
        RoomMovement neutral;neutral.actor_epoch=1;neutral.source=RoomMovementSource::NativeActionV1;
        return frame.players[0]==neutral && frame.players[1]==neutral?RetailButtonPhase::StartupNeutral:RetailButtonPhase::Invalid;
    }
    for(unsigned i=0;i<2;++i){const auto& command=frame.players[i];
        if(command.source!=RoomMovementSource::NativeActionV3 || !ValidRoomMovement(command) ||
           command.button_actions.source_tick!=frame.tick-delay)return RetailButtonPhase::Invalid;}
    return RetailButtonPhase::NativeButtons;
}
}

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
enum class RoomMovementSource : uint8_t { UnconvertedPad=0, NativeActionV1=1 };

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
    bool operator==(const RoomMovement& other)const{
        // Native floating-point input must echo bit-for-bit, including signed
        // zero; atan2 and later native arithmetic can distinguish its sign.
        return std::bit_cast<uint32_t>(x)==std::bit_cast<uint32_t>(other.x) &&
            std::bit_cast<uint32_t>(y)==std::bit_cast<uint32_t>(other.y) &&
            std::bit_cast<uint32_t>(forward_x)==std::bit_cast<uint32_t>(other.forward_x) &&
            std::bit_cast<uint32_t>(forward_z)==std::bit_cast<uint32_t>(other.forward_z) &&
            pad==other.pad && actor_epoch==other.actor_epoch && source==other.source;
    }
};

// One admission rule shared by the network decoder and native coordinator.
inline bool ValidRoomMovement(const RoomMovement& value){
    return value.actor_epoch!=0 && value.source==RoomMovementSource::NativeActionV1 &&
        std::isfinite(value.x) && std::isfinite(value.y) && std::abs(value.x)<=1 && std::abs(value.y)<=1 &&
        std::isfinite(value.forward_x) && std::isfinite(value.forward_z) &&
        std::abs(value.forward_x)<=1 && std::abs(value.forward_z)<=1 &&
        std::abs(double(value.forward_x)*value.forward_x+double(value.forward_z)*value.forward_z-1)<1e-4;
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

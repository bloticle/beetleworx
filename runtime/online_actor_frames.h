#pragma once
#include "online_actor_input.h"
#include "online_movement_frame.h"
#include <optional>
namespace em2::net {
enum class OwnedFrameResult {Ready,NotConfigured,WrongThread,InvalidRoster,InvalidMovement,
    UnsupportedIntent,EpochMismatch,UnboundActor,StaleActor,NativeNotReady,UnexpectedTick,
    IncompleteTick,NativeFailure,Faulted};
// Simulation-thread owner for extra actors. The caller separately publishes
// retail players 1/2 through the canonical controller-input gate.
// Preflight before committing a transport frame;
// Deliver after native join/admission and before native controller updates.
// Finish only after the native world tick completes. Any partial native failure
// faults the coordinator: do not advance/retry the world with partial delivery.
class OwnedMovementFrames {
public:
    OwnedMovementFrames()=default;
    OwnedMovementFrames(const OwnedMovementFrames&)=delete;
    OwnedMovementFrames& operator=(const OwnedMovementFrames&)=delete;
    bool Configure(uint16_t roster_mask,uint16_t retail_mask,
        const std::array<uint64_t,kOnlineRoomCapacity>& actor_epochs,
        uint64_t first_tick,uint32_t registry);
    bool Bind(uint8_t room_peer,uint64_t native_peer,PPCContext&,uint8_t* base);
    OwnedFrameResult Preflight(const RoomFrame&,PPCContext&,uint8_t* base)const;
    OwnedFrameResult Deliver(const RoomFrame&,PPCContext&,uint8_t* base);
    OwnedFrameResult Finish(uint64_t tick,uint8_t* base);
    bool faulted()const{return faulted_;}
    uint64_t next_tick()const{return next_tick_;}
private:
    struct Binding {uint64_t native_peer=0,generation=0;uint32_t entity=0,controller=0;};
    std::array<Binding,kOnlineRoomCapacity> bindings_{};
    std::array<RoomJumpStream,kOnlineRoomCapacity> jumps_{};
    std::array<uint64_t,kOnlineRoomCapacity> epochs_{};
    std::optional<RoomFrame> delivered_;
    uint32_t registry_=0,owner_thread_=0;
    uint16_t roster_=0,retail_=0;
    uint64_t next_tick_=0;
    bool configured_=false,faulted_=false,exhausted_=false;
};
// Invoked only while the native Jump-task/physics-stimulus fixture owns its
// private image. Exercises actual TCP authoritative frames before controllers.
bool TestOwnedJumpMovementFrames(PPCContext&,uint8_t* base,uint64_t first_native_peer,uint64_t second_native_peer);
using OwnedFrameUpdate=void(*)(PPCContext&,uint8_t*);
bool TestOwnedMovementFrames(PPCContext&,uint8_t* base,uint64_t first_native_peer,
    uint64_t second_native_peer,OwnedFrameUpdate bounded_native_update);
}

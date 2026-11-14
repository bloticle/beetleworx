#pragma once
#include "online_actor_input.h"
#include "online_movement_frame.h"
#include "online_native_triggers.h"
#include <optional>
namespace em2::net {
enum class OwnedFrameResult {Ready,NotConfigured,WrongThread,InvalidRoster,InvalidMovement,
    UnsupportedIntent,EpochMismatch,UnboundActor,StaleActor,NativeNotReady,UnexpectedTick,
    IncompleteTick,NativeFailure,Faulted};
// Simulation-thread owner for extra actors. The caller separately publishes
// retail players 1/2 through the canonical controller-input gate and, for
// interpreted commands, the armed native retail receivers.
// Preflight before committing a transport frame;
// Deliver after native join/admission and before native controller updates.
// Finish only after retail receivers and the native world tick complete. The
// caller verifies retail completion; this coordinator verifies owned actors.
// Any partial native failure
// faults the coordinator: do not advance/retry the world with partial delivery.
class OwnedMovementFrames {
public:
    // Read-only admission supplied by the actual armed retail receiver. A raw
    // pad publisher alone cannot opt into interpreted Jump/button commands.
    // Receives only the retail projection, retaining every command byte.
    using RetailAdmission=ActorInputResult(*)(const RoomFrame&,PPCContext&,uint8_t*);
    OwnedMovementFrames()=default;
    OwnedMovementFrames(const OwnedMovementFrames&)=delete;
    OwnedMovementFrames& operator=(const OwnedMovementFrames&)=delete;
    bool Configure(uint16_t roster_mask,uint16_t retail_mask,
        const std::array<uint64_t,kOnlineRoomCapacity>& actor_epochs,
        uint64_t first_tick,uint32_t registry,RetailAdmission=nullptr);
    bool Bind(uint8_t room_peer,uint64_t native_peer,PPCContext&,uint8_t* base);
    bool BindButtons(uint8_t room_peer,const NativeTriggerBinding&,PPCContext&,uint8_t*);
    OwnedFrameResult Preflight(const RoomFrame&,PPCContext&,uint8_t* base)const;
    OwnedFrameResult Deliver(const RoomFrame&,PPCContext&,uint8_t* base);
    OwnedFrameResult Finish(uint64_t tick,uint8_t* base);
    bool faulted()const{return faulted_;}
    uint64_t next_tick()const{return next_tick_;}
private:
    struct Binding {uint64_t native_peer=0,generation=0;uint32_t entity=0,controller=0;unsigned action_user=0;std::optional<NativeTriggerBinding> buttons;};
    std::array<Binding,kOnlineRoomCapacity> bindings_{};
    std::array<RoomJumpStream,kOnlineRoomCapacity> jumps_{};
    std::array<uint64_t,kOnlineRoomCapacity> epochs_{};
    std::optional<RoomFrame> delivered_;
    uint32_t registry_=0,owner_thread_=0;
    uint16_t roster_=0,retail_=0;
    uint64_t next_tick_=0;
    bool configured_=false,faulted_=false,exhausted_=false;
    RetailAdmission retail_admission_=nullptr;
};
namespace detail {
bool OwnedButtonDispatchAuthorized(uint64_t peer,uint64_t generation,uint64_t tick,const NativeActorAction&)noexcept;
ActorInputResult DispatchOwnedActorButton(uint64_t peer,uint64_t generation,uint64_t tick,const NativeActorAction&,PPCContext&,uint8_t*,uint32_t& native_result);
}
// Invoked only while the native Jump-task/physics-stimulus fixture owns its
// private image. Exercises actual TCP authoritative frames before controllers.
using OwnedFrameUpdate=void(*)(PPCContext&,uint8_t*);
bool TestOwnedJumpMovementFrames(PPCContext&,uint8_t* base,uint64_t first_native_peer,uint64_t second_native_peer,OwnedFrameUpdate);
bool TestOwnedMovementFrames(PPCContext&,uint8_t* base,uint64_t first_native_peer,
    uint64_t second_native_peer,OwnedFrameUpdate bounded_native_update);
}

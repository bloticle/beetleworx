#pragma once
#include "online_actor_input.h"
#include "online_actor_reference.h"
#include <array>
#include <span>
#include <string>
namespace em2::net {
class OwnedMovementFrames;
struct OwnedRosterActor {
    uint8_t room_peer=0;
    OwnedActorInput actor;
    ActorMovementReference reference;
};
// Simulation-thread binding lifetime for real extra humans. The native world,
// not this object, owns their entities. Add BEFORE native admission. Activate
// only AFTER admission. Keep registrations quarantined until native removal.
// Explicit retirement/world abandonment is required before destruction; the
// destructor can quarantine inputs but cannot issue native lifecycle calls.
class OwnedActorRoster {
public:
    OwnedActorRoster()=default;
    OwnedActorRoster(const OwnedActorRoster&)=delete;
    OwnedActorRoster& operator=(const OwnedActorRoster&)=delete;
    ~OwnedActorRoster();
    bool Add(uint8_t room_peer,uint64_t native_peer,uint32_t handle,unsigned action_user,PPCContext&,uint8_t*);
    bool Activate(uint16_t expected_mask,uint32_t movement_camera_handle,PPCContext&,uint8_t*);
    // One native camera handle per registered actor, in registration order.
    bool Activate(uint16_t expected_mask,std::span<const uint32_t> camera_handles,PPCContext&,uint8_t*);
    // Failure after a partial Bind requires discarding that frame coordinator.
    bool BindFrames(OwnedMovementFrames&,PPCContext&,uint8_t*);
    bool BeginRetirement(PPCContext&,uint8_t*);
    bool CompleteRetirement(PPCContext&,uint8_t*);
    // Only after the global registry changed; no old-world native calls.
    bool AbandonDestroyedWorld(PPCContext&,uint8_t*);
    // Immediately before the original engine destroys this complete world.
    // Release borrowed movement references while their entities still live.
    bool ReleaseForWorldUnload(PPCContext&,uint8_t*);
    bool active()const{return active_;}
    uint16_t mask()const{return mask_;}
    std::span<const OwnedRosterActor> actors()const{return {actors_.data(),count_};}
    const std::string& error()const{return error_;}
private:
    bool Owner()const;
    void Disable();
    bool Reject(const char*);
    std::array<OwnedRosterActor,16> actors_{};
    unsigned count_=0;
    uint32_t registry_=0,thread_=0;
    uint16_t mask_=0;
    bool active_=false,retiring_=false,retired_=false;
    std::string error_;
};
}

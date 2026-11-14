#pragma once
#include "online_actor_roster.h"
#include "online_lobby.h"
#include <memory>
namespace em2::net {
enum class NativeRosterStep {Waiting,Ready,Failed};
struct RoomFrame;
// Native world owns the entities. This adapter owns their quarantined input
// leases. Prepare/Retire run only on the simulation thread at agreed completed
// updates, never from a UI poll or the read-only gameplay-ready probe.
class NativeRoomActors {
public:
    NativeRoomActors();
    ~NativeRoomActors();
    NativeRoomActors(const NativeRoomActors&)=delete;
    NativeRoomActors& operator=(const NativeRoomActors&)=delete;
    bool Configure(const LobbySnapshot&);
    NativeRosterStep Prepare(PPCContext&,uint8_t*);
    bool Retire(PPCContext&,uint8_t*);
    bool PrepareWorldUnload(PPCContext&,uint8_t*);
    bool FinishWorldUnload(PPCContext&,uint8_t*);
    bool BindFrames(OwnedMovementFrames&,PPCContext&,uint8_t*);
    uint16_t mask()const;
    bool ready()const;
    std::span<const OwnedRosterActor> actors()const;
    uint32_t CameraController(uint64_t room_peer)const;
    bool UpdateCameras(const RoomFrame&,PPCContext&,uint8_t*);
    bool OwnsCameraMode(uint32_t,uint8_t*)const;
    bool ViewBasis(uint64_t room_peer,PPCContext&,uint8_t*,float& x,float& z)const;
    const std::string& error()const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

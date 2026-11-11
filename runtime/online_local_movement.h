#pragma once
#include "online_actor_input.h"
#include <array>
#include <cstdint>
struct PPCContext;
namespace em2::net {
struct Pad;
struct RoomMovement;
class Room;
class NativeRoutedPadSample;
// A local producer lease, not a network actor incarnation or simulation tick.
struct NativeLocalMovementBinding {
    uint64_t world_epoch=0,producer_generation=0;
    uint32_t retail_slot=0; // Canonical native record 0/1, never hardware/peer ID.
    uint32_t registry=0,player_manager=0,handle=0,entity=0,controller=0,listener=0;
    uint32_t manager=0,event=0,action=0,movement_uid=0,user=0,logical_user=0,input=0,device=0;
    uint32_t active_event=0,active_event_uid=0,profile=0,user_object=0;
    std::array<uint32_t,24> action_shape{};
    std::array<uint32_t,14> device_shape{};
    std::array<uint32_t,4> event_shape{};
    uint32_t reference=0;
    // Native entity GUID and its exact camera component identities. Never wire data.
    std::array<uint32_t,7> reference_shape{};
    bool operator==(const NativeLocalMovementBinding&)const=default;
};
// Explicit game-thread opt-in after real world admission. Default is unarmed.
// Caller must disarm before world teardown and use a new epoch after reload.
// selected_native_user is a real native device index, never a room peer ID.
// selected_retail_slot explicitly selects the canonical human record 0/1.
bool ArmNativeLocalMovement(uint64_t world_epoch,uint32_t registry,uint8_t* base,unsigned selected_native_user=0,unsigned selected_retail_slot=0);
void DisarmNativeLocalMovement();
// Observe at the original Mickey listener ENTRY, before its gates, only when
// LR=827C439C. Captures the real selected event/action; sends no input.
bool CaptureNativeLocalMovementBinding(PPCContext&,uint8_t* base,NativeLocalMovementBinding&);
void ObserveNativeLocalMovementBinding(PPCContext&,uint8_t* base) noexcept;
bool NativeLocalMovementBindingFor(PPCContext&,uint8_t* base,NativeLocalMovementBinding&);
// Pre-tick read-only discovery. Rejects ambiguity across default/current events;
// does not claim that a native dispatch was observed.
bool DiscoverNativeLocalMovementBinding(PPCContext&,uint8_t* base,NativeLocalMovementBinding&);
struct NativeLocalMovementDiagnostics {
    uint64_t observations=0,captured=0;
    uint32_t lr=0,listener=0,event=0,action=0,stage=0,best_stage=0;
    uint32_t math_return=0,axis_queries=0;
    float math_x=0,math_y=0;
    uint64_t discoveries=0;
    uint32_t discovery_stage=0,discovery_event=0,discovery_count=0,discovery_index=0;
    uint32_t discovery_uid=0,discovery_candidates=0,discovery_inspect_stage=0;
};
// Value snapshot only. Stage100 means binding accepted; stages below100 locate
// owner/world10, retail20, type30, action40, active event50, device60,
// axis records70, response80, device history90. Stage1 is dispatch admission.
// Discovery stages201..225 distinguish its pre-Inspect path; its candidate
// rejection retains discovery_inspect_stage separately. 225 means success.
NativeLocalMovementDiagnostics LocalMovementDiagnostics() noexcept;
// Original native action math only: no event dispatch, actor writes, network
// submission, camera basis, tick claim or fabricated wire actor_epoch.
// sampled_native_user must identify the same device used to obtain this Pad.
bool ProduceNativeLocalMovement(const NativeLocalMovementBinding&,const Pad&,
    PPCContext&,uint8_t* base,NativeActorAction&,unsigned sampled_native_user=0);
// Ordinary native camera-reference path only. actor_epoch is supplied by the
// authoritative room spawn lifecycle, never inferred from this local lease.
bool ProduceNativeLocalRoomMovement(const NativeLocalMovementBinding&,const Pad&,uint64_t actor_epoch,
    PPCContext&,uint8_t* base,RoomMovement&,unsigned sampled_native_user=0);
// Cross-device routing requires a sealed sample minted by the Room-owned
// physical sampler. The strict raw-Pad/user overload above is unchanged.
bool ProduceNativeLocalRoomMovement(const Room&,const NativeLocalMovementBinding&,const NativeRoutedPadSample&,uint64_t actor_epoch,
    PPCContext&,uint8_t* base,RoomMovement&);
bool TestNativeLocalMovement();
}

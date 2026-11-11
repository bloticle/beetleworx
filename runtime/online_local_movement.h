#pragma once
#include "online_actor_input.h"
#include <array>
#include <cstdint>
struct PPCContext;
namespace em2::net {
struct Pad;
// A local producer lease, not a network actor incarnation or simulation tick.
struct NativeLocalMovementBinding {
    uint64_t world_epoch=0,producer_generation=0;
    uint32_t registry=0,player_manager=0,handle=0,entity=0,controller=0,listener=0;
    uint32_t manager=0,event=0,action=0,movement_uid=0,user=0,logical_user=0,input=0,device=0;
    uint32_t active_event=0,active_event_uid=0,profile=0,user_object=0;
    std::array<uint32_t,24> action_shape{};
    std::array<uint32_t,14> device_shape{};
    std::array<uint32_t,4> event_shape{};
    bool operator==(const NativeLocalMovementBinding&)const=default;
};
// Explicit game-thread opt-in after real world admission. Default is unarmed.
// Caller must disarm before world teardown and use a new epoch after reload.
// selected_native_user is a real native device index, never a room peer ID.
bool ArmNativeLocalMovement(uint64_t world_epoch,uint32_t registry,uint8_t* base,unsigned selected_native_user=0);
void DisarmNativeLocalMovement();
// Observe at the original Mickey listener ENTRY, before its gates, only when
// LR=827C439C. Captures the real selected event/action; sends no input.
bool CaptureNativeLocalMovementBinding(PPCContext&,uint8_t* base,NativeLocalMovementBinding&);
void ObserveNativeLocalMovementBinding(PPCContext&,uint8_t* base) noexcept;
bool NativeLocalMovementBindingFor(PPCContext&,uint8_t* base,NativeLocalMovementBinding&);
struct NativeLocalMovementDiagnostics {
    uint64_t observations=0,captured=0;
    uint32_t lr=0,listener=0,event=0,action=0,stage=0,best_stage=0;
    uint32_t math_return=0,axis_queries=0;
    float math_x=0,math_y=0;
};
// Value snapshot only. Stage100 means binding accepted; stages below100 locate
// owner/world10, retail20, type30, action40, active event50, device60,
// axis records70, response80, device history90. Stage1 is dispatch admission.
NativeLocalMovementDiagnostics LocalMovementDiagnostics() noexcept;
// Original native action math only: no event dispatch, actor writes, network
// submission, camera basis, tick claim or fabricated wire actor_epoch.
// sampled_native_user must identify the same device used to obtain this Pad.
bool ProduceNativeLocalMovement(const NativeLocalMovementBinding&,const Pad&,
    PPCContext&,uint8_t* base,NativeActorAction&,unsigned sampled_native_user=0);
bool TestNativeLocalMovement();
}

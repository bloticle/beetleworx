#pragma once
#include "online_actor_input.h"
#include "online_movement_frame.h"
#include "ppc_context.h"
namespace em2::net {
// Retail PlayerManager ownership, deliberately disjoint from OwnedActorInput.
struct RetailActorInput {
    uint64_t world_epoch=0,actor_epoch=0,generation=0,load_generation=0;
    unsigned slot=0;
    uint32_t registry=0,manager=0,handle=0,entity=0,controller=0,listener=0,motion=0,reference=0;
    std::array<uint32_t,4> entity_guid{},reference_guid{};
    bool operator==(const RetailActorInput&)const=default;
};
bool ArmRetailInput(uint64_t world_epoch,uint32_t registry,uint64_t controls_generation,
    uint16_t retail_mask,const std::array<uint64_t,2>& actor_epochs,uint64_t first_tick,PPCContext&,uint8_t*);
// Only before the first gameplay Begin; requires the owning bootstrap's actual
// just-completed published raw frame. Does not reset input, RNG or actor state.
bool RebaseRetailInputFirstTick(uint64_t next_tick,PPCContext&,uint8_t*);
void DisarmRetailInput();
bool RetailActorInputFor(unsigned,PPCContext&,uint8_t*,RetailActorInput&);
// Retained gameplay ownership across completed/waiting/faulted tick gaps.
// Never armed-only bootstrap ownership and never a replacement controls/load.
bool RetailInputQuarantineFor(unsigned,PPCContext&,uint8_t*,RetailActorInput&);
using RetailJumpAdmission=bool(*)(unsigned,const RoomMovement&,PPCContext&,uint8_t*);
// Installed by an armed native Jump adapter; nullptr restores Movement-only.
bool SetRetailJumpAdmission(RetailJumpAdmission);
using RetailButtonAdmission=bool(*)(unsigned,const RoomMovement&,PPCContext&,uint8_t*);
// Explicit complete-button receiver capability; immutable after first Begin.
bool SetRetailButtonAdmission(RetailButtonAdmission);
// Only an owner publishing full canonical two-lane Pads may opt into native
// right-stick camera actions. Default rejects these axes; immutable once begun.
bool SetRetailCameraInputAllowed(bool);
ActorInputResult ValidateRetailMovement(unsigned,const RoomMovement&,PPCContext&,uint8_t*);
ActorInputResult ValidateRetailMovementFrame(const RoomFrame&,PPCContext&,uint8_t*);
// Receiver-local native descriptors, never wire pointers. Preflight both slots
// before consuming a frame and again before delivering either native callback.
ActorInputResult ValidateRetailMovementAction(unsigned,const NativeActorAction&,PPCContext&,uint8_t*);
bool BeginRetailBasisTick(uint32_t registry,uint64_t tick,PPCContext&,uint8_t*);
ActorInputResult PrepareRetailMovement(unsigned,const RoomMovement&,uint64_t tick,PPCContext&,uint8_t*);
ActorInputResult ReplayRetailMovement(unsigned,const NativeActorAction&,uint64_t tick,PPCContext&,uint8_t*);
bool RetailInputTickFor(unsigned,uint64_t,PPCContext&,uint8_t*,RetailActorInput&,bool& movement_delivered,RoomMovement* prepared_command=nullptr);
bool FinishRetailInputTick(uint64_t,PPCContext&,uint8_t*);
bool SuppressRetailMovement(PPCContext&,uint8_t*);
void UpdateRetailBasisController(PPCContext&,uint8_t*,PPCFunc* continuation);
bool ApplyRetailBasisMotion(PPCContext&,uint8_t*,PPCFunc* native);
bool TestNativeRetailInput();
}

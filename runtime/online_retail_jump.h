#pragma once
#include "online_actor_input.h"
#include "online_movement_frame.h"
#include <functional>
namespace em2::net {
// Both human retail Movement leases must already be armed. This only accepts
// the exact entity-owned original Jump task and its typed native components.
bool ArmRetailJump(PPCContext&,uint8_t* base);
ActorInputResult ValidateRetailJumpFrame(const RoomFrame&,PPCContext&,uint8_t* base);
ActorInputResult ReplayRetailJumpBatch(unsigned slot,const RoomMovement&,uint64_t tick,PPCContext&,uint8_t* base);
ActorInputResult FinishRetailJumpTick(uint64_t tick,PPCContext&,uint8_t* base,bool movie_entry=false);
// Lifecycle only: release after native world/listeners stopped. Never clear a
// live held task manually or resume a partially delivered frame after failure.
void DisarmRetailJump();
// Explicit capability for Movement's strict raw-intent policy. This is false
// until this receiver owns and revalidates that exact retail lease.
bool RetailJumpAllows(unsigned slot,const RoomMovement&,PPCContext&,uint8_t* base);
// Strong derived/base wrapper chain. base_handler=false for82A3C930 and true
// for82783CE8: the private permit authorizes one ordered derived→base path.
bool SuppressRetailJumpInput(PPCContext&,uint8_t* base,bool base_handler);
// Shared existing physics wrapper calls this only to service a private native
// task fixture; false in normal play. Does not replace production physics.
bool RetailJumpFixtureStimulus(PPCContext&,uint8_t* base);
bool TestRetailJumpReceiver(PPCContext&,uint8_t* base,const std::function<bool(const RoomFrame&)>& drive);
}

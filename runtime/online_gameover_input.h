#pragma once
#include "online_movement_frame.h"
struct PPCContext;
namespace em2::net {
struct NativeGameOverState {uint32_t registry=0,dead=0;bool operator==(const NativeGameOverState&)const=default;};
// Read the original native control-state interface and death counter. This
// alone does not authorize a Room frame: its owner also pins world and roster.
bool ReadNativeGameOverState(PPCContext&,uint8_t*,NativeGameOverState&);
bool ReadNativeControlState(PPCContext&,uint8_t*,NativeGameOverState&,uint32_t& mode);
RoomMovement NativeGameOverCommand(uint64_t input_epoch=1);
// Metadata validation after the gameplay owner has certified native game-over.
// Previously produced delayed commands may drain only inside the old window.
bool ValidNativeGameOverFrame(const RoomFrame&,uint16_t,uint64_t entry_tick,uint8_t delay,
    const std::array<std::array<uint8_t,32>,kOnlineRoomCapacity>& schemas,bool native_buttons,uint64_t input_epoch=1);
// Transport can retain terminal no-action commands without pretending to
// advance native gameplay edge history. This phase cannot resume in-place.
struct RoomTransportInputStream {RoomJumpStream gameplay;uint64_t input_epoch=1;bool terminal=false,suspended=false;bool operator==(const RoomTransportInputStream&)const=default;};
bool AdvanceRoomTransportInput(RoomTransportInputStream&,const RoomMovement&);
bool TestNativeGameOverInput();
}

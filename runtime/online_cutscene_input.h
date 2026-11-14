#pragma once
#include "online_gameover_input.h"
#include <string>
namespace em2::net {
bool ReadNativeInputContextName(uint8_t*,std::string&);
bool ReadNativeCutsceneContext(PPCContext&,uint8_t*,uint32_t registry);
RoomMovement NativeCutsceneCommand(uint64_t input_epoch);
bool ValidNativeCutsceneFrame(const RoomFrame&,uint16_t,uint64_t entry_tick,uint8_t delay,uint64_t input_epoch,
    const std::array<std::array<uint8_t,32>,kOnlineRoomCapacity>& schemas,bool native_buttons);
bool NativeCutsceneReturnPrefix(const RoomFrame&,uint16_t,uint64_t input_epoch);
bool TestNativeCutsceneInput();
}

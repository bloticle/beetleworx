#pragma once
#include "online_gameover_input.h"
#include <string>
#include <vector>
#include <array>
namespace em2::net {
bool ReadNativeInputContextName(uint8_t*,std::string&);
bool ReadNativeCutsceneContext(PPCContext&,uint8_t*,uint32_t registry);
RoomMovement NativeCutsceneCommand(uint64_t input_epoch);
bool ValidNativeCutsceneFrame(const RoomFrame&,uint16_t,uint64_t entry_tick,uint8_t delay,uint64_t input_epoch,
    const std::array<std::array<uint8_t,32>,kOnlineRoomCapacity>& schemas,bool native_buttons);
bool NativeCutsceneReturnPrefix(const RoomFrame&,uint16_t,uint64_t input_epoch);
// Read-only native context inventory. Addresses describe this process only;
// they never travel on the wire and do not authorize UI callbacks.
struct NativeCutsceneUiAction {uint32_t event=0,ordinal=0,action=0,kind=0,condition=0,curve=0;std::string name;std::vector<std::array<uint32_t,8>> bindings;};
struct NativeCutsceneUiEvent {uint32_t event=0;std::string name;std::vector<std::array<uint32_t,3>> listeners;std::vector<NativeCutsceneUiAction> actions;};
struct NativeCutsceneUiSnapshot {uint32_t manager=0;std::vector<NativeCutsceneUiEvent> contexts;};
bool InspectNativeCutsceneUi(PPCContext&,uint8_t*,uint32_t registry,NativeCutsceneUiSnapshot&);
void TracePrivateNativeCutsceneUi(uint64_t input_epoch,PPCContext&,uint8_t*,uint32_t registry);
bool TestNativeCutsceneInput();
}

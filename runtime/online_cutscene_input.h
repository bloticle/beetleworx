#pragma once
#include "online_gameover_input.h"
#include <string>
#include <vector>
#include <array>
namespace em2::net {
bool ReadNativeInputContextName(uint8_t*,std::string&);
// State 1 owns a direct movie; state 2 may also queue menu/credits entries.
// Both require their original target and movie content to validate.
constexpr bool NativeMovieControlMode(uint32_t mode){return mode==1 || mode==2;}
constexpr uint32_t NativeMovieInputHandler(uint32_t mode){return mode==1?0x82B1EDF8u:mode==2?0x82B1DF70u:0;}
constexpr bool NativeCutsceneControlMode(uint32_t mode){return NativeMovieControlMode(mode) || mode==3 || mode==6;}
bool ReadNativeCutsceneContext(PPCContext&,uint8_t*,uint32_t registry,uint32_t owned_movie_sequence=0);
RoomMovement NativeCutsceneCommand(uint64_t input_epoch);
RoomMovement NativeCutsceneUiCommand(uint64_t input_epoch,bool down);
// Entry fence: a carried gameplay press must release before UI intent begins.
struct NativeCutsceneUiGate {
    bool released=false;
    bool Sample(bool down){if(!down)released=true;return released && down;}
};
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
bool TestNativeMovieClock();
}

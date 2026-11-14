#pragma once
#include <cstdint>
struct PPCContext;
namespace em2::net {
struct Pad;
bool TestNativeOnlineViewport();
// Custom HUD property: 0 keeps offline behavior, 2/3 are online Mickey/Oswald.
unsigned NativeOnlineHudMode();
// Completed native camera boundary only. Rebinds the cached primary view after
// extra actors/cameras are admitted later than ordinary view construction.
bool BindNativeRoomLocalView(PPCContext&,uint8_t*);
// Current retained primary NiCamera, render owner only; no native calls.
uint32_t NativeRoomViewCamera(uint8_t*);
// Private loopback-only functional input test. Pads still cross the real
// transport and native controller adapter; this does not test physical devices.
void ApplyPrivateOnlineInputSequence(uint64_t source_tick,unsigned slot,bool joining,Pad&);
void ObservePrivateOnlineView(PPCContext&,uint8_t*,uint64_t completed_tick);
}

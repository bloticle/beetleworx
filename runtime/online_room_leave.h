#pragma once
#include <string_view>
struct PPCContext;
namespace em2::net {
// Transition runs only on a checked native owner boundary. UI requests never
// unload a world directly; an unsafe failed update still requires a restart.
void TickNativeRoomTitleLeave(PPCContext&,unsigned char*,bool completed_outer_boundary);
void NotifyNativeRoomTitleLeaveLoaded(std::string_view);
bool NativeRoomTitleLeaveHalted()noexcept;
bool NativeRoomTitleLeaveRenderRetired()noexcept;
// 2790 queries recovery availability (choice -1), or requests return (1).
// 2791 reads the bounded explanation as UTF-8 bytes.
bool NativeRoomRecoveryOption(int id,int choice,int& result);
bool NativeRoomRecoveryKey(unsigned key,bool down);
bool NativeRoomRecoveryPad(bool connected,unsigned short buttons);
// Render-thread-only: invoke the dedicated HUD method without advancing movies.
void PumpNativeRoomRecoveryHud(PPCContext&,unsigned char*,unsigned container);
bool TestNativeRoomRecoveryRequests();
}

#pragma once
#include <string_view>
struct PPCContext;
namespace em2::net {
// Private native QA prototype. No production option enables it.
void TickNativeRoomTitleLeave(PPCContext&,unsigned char*,bool completed_outer_boundary);
void NotifyNativeRoomTitleLeaveLoaded(std::string_view);
bool NativeRoomTitleLeaveHalted()noexcept;
bool NativeRoomTitleLeaveRenderRetired()noexcept;
}

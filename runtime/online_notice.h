#pragma once
#include "online_runtime.h"
#include <chrono>
#include <string>
#include <string_view>
namespace em2::net {
// Actionable connection/recovery guidance shared by setup and in-game notices.
std::string OnlineFailureAdvice(std::string_view reason,bool during_play);
struct OnlineNotice {std::string title,detail;bool error=false;explicit operator bool()const{return !title.empty();}};
class OnlineNoticeModel {
public:
    using Clock=std::chrono::steady_clock;using Time=Clock::time_point;
    OnlineNotice Update(const NativeSessionStatus&,bool auto_continue,Time now);
private:
    NativeSessionState state_=NativeSessionState::None;InputWait wait_=InputWait::None;
    bool initialized_=false,stalled_=false;
    Time last_{},state_since_{},wait_since_{},recovered_until_{};
};
// Noninteractive foreground drawing, independent of the performance HUD.
void DrawOnlineNotice(const OnlineNotice&,float width,float height);
bool TestOnlineNotices();
}

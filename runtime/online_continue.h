#pragma once
namespace em2 {
bool ClaimOfflineStartupSaveNotice();
bool OfflineStartupContinueAccepted();
bool ClaimNativeLobbyQaFreshSaveNotice();
bool NativeLobbyQaFreshSaveNoticeAccepted();
bool TestOfflineStartup();
}
namespace em2::net {
// Exercises offline/failed rejection or an adopted ready session with real callbacks.
bool TestNativeOnlineContinue(bool manual=false);
bool TestNativeOnlineProfile();
}

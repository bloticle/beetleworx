#pragma once
#include "online_boot.h"
namespace em2::net {
// Native GFx numeric command/status bridge. Only main-menu callers may start
// preparation; network/save work stays asynchronous and private.
bool NativeOnlineMenuOption(int id,int choice,int& result);
// Main application lifecycle owner consumes only after native Start is pressed.
// It must safely route and reload the isolated save before native Continue.
std::optional<BootResult> TakeNativeOnlineMenuReady();
// The original save reload and Continue have completed; the live session now
// owns the connection. Discard only the consumed menu preparation coordinator.
void CompleteNativeOnlineMenuHandoff();
void SetNativeOnlineMenuHandoffAvailable(bool available);
void FailNativeOnlineMenuHandoff(std::string reason);
void CancelNativeOnlineMenu();
// Explicit, validated, private-root/loopback-only diagnostic. Never enabled by
// production menu choices or by ordinary automatic Continue.
bool NativeOnlineMenuQaActive();
bool NativeOnlineNameKey(unsigned key,bool down,bool shift=false,bool control=false);
bool NativeOnlineNameCharacter(uint32_t utf16);
void CancelNativeOnlineNameInput();
void RestoreNativeOnlineNameFocus();
bool TestNativeOnlineMenu();
}

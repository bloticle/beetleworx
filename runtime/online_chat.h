#pragma once
#include "ppc_context.h"
namespace em2::net {
class Room;
void PumpOnlineChat(Room*); // Room owner thread; never called by the UI thread.
bool OnlineChatKey(unsigned key,bool down,bool shift=false,bool control=false);
bool OnlineChatCharacter(uint32_t utf16);
bool OnlineChatPad(uint16_t buttons); // true while chat owns this physical packet
void CloseOnlineChat();
void EnsureNativeChatBridge(PPCContext&,uint8_t*);
bool NativeOnlineChatOption(int id,int choice,int& result);
bool TestOnlineChatControls();
}

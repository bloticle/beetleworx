#pragma once
#include "ppc_context.h"
namespace em2 {
bool TestNativePlayerActions();
bool TestCanonicalNativeJoin();
void ResetNativeSynchronizedJoinTiming();
void RunNativeSynchronizedJoin(PPCContext&,uint8_t*,uint16_t buttons);
void RequestCharacterSwap();
void UpdateCharacterSwap(PPCContext& ctx,uint8_t* base);
void UpdateNativeJoinedPlayerControls(PPCContext& ctx,uint8_t* base);
void MaybeQueueNativeStartLevel();
bool NativeCoopReady(uint8_t* base);
bool OswaldIsSelected();
bool NativeOnlineJoinAllowed(uint8_t* base);
const char* CharacterSwapNotice();
inline constexpr uint32_t kSelectedCharacterGetter=uint32_t(PPC_CODE_BASE+PPC_CODE_SIZE);
void ReadSelectedCharacterProperty(PPCContext& ctx,uint8_t* base);
}

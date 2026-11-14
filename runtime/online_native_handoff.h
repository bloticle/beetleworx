#pragma once
#include <cstdint>
struct PPCContext;
namespace em2::net {
// Register before guest launch; disable before title shutdown. The consumer is
// experimental until the real authored reload/Resume path passes paired tests.
void RegisterNativeOnlineHandoff(bool available);
// Main guest thread, on the live authored base-menu update. Never retains the
// menu pointer after Resume, and never sends hardware input.
void UpdateNativeOnlineHandoff(PPCContext&,uint8_t*);
void TickNativeOnlineTitleMenuHandoff(PPCContext&,uint8_t*,bool completed_outer_boundary);
enum class NativeTitleRestoreState {Idle,Pending,Ready,Failed};
// Native owner only, after the online world and retained owner are released.
bool BeginNativeOnlineTitleRestore();
NativeTitleRestoreState NativeOnlineTitleRestoreStatus();
bool TestNativeOnlineHandoff();
}

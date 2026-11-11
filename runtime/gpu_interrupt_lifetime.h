#pragma once
#include <cstdint>
struct PPCContext;
namespace em2 {
void OpenGuestGPUInterrupts();
// Drain callbacks already executing before freeing the title process.
void CloseGuestGPUInterrupts();
void DispatchGuestGPUInterrupt(PPCContext&,uint8_t*,void(*callback)(PPCContext&,uint8_t*));
bool TestGPUInterruptLifetime();
}

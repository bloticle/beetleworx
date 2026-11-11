#pragma once
#include <cstdint>
struct PPCContext;
namespace em2 {
bool TestNativeCameraTiming();
void ObserveNativeCameraUpdate(const PPCContext&,uint8_t* base) noexcept;
void ObserveNativeCameraEntity(const PPCContext&,uint8_t* base) noexcept;
void ObserveNativeCameraInputDevice(uint32_t device,const PPCContext&,uint8_t* base) noexcept;
double NativePitchMotionThreshold(double authored,bool velocity=false) noexcept;
}

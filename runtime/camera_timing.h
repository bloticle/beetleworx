#pragma once
#include <cstdint>
struct PPCContext;
namespace em2 {
bool TestNativeCameraTiming();
void ObserveNativeCameraUpdate(const PPCContext&,uint8_t* base) noexcept;
double NativePitchMotionThreshold(double authored,bool velocity=false) noexcept;
}

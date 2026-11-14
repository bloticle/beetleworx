#pragma once
#include <array>
#include <cstdint>
struct PPCContext;
namespace em2 {
struct LiveLookInput;
struct NativeCameraLookBinding {
    uint32_t mode=0,controller=0,registry=0,event=0,action=0,manager=0,input_system=0,device=0,user=0;
    std::array<uint32_t,37> fingerprint{};
    bool operator==(const NativeCameraLookBinding&)const=default;
};
struct NativeCameraLookResult {float x=0,y=0;uint32_t event=0,action=0;};
// Called at the actual native normal-mode OnLook dispatch before its original.
void ObserveNativeCameraLookBinding(uint32_t mode,uint32_t event,uint32_t action,uint8_t* base) noexcept;
bool NativeCameraLookBindingFor(uint32_t mode,uint8_t* base,NativeCameraLookBinding&,unsigned user=0);
// Runs only original 827C5990 action math. The caller owns full normal-mode
// OnLook dispatch/gates; this does not write camera caches or dispatch events.
bool ReplayNativeCameraLookAction(const NativeCameraLookBinding&,const LiveLookInput&,PPCContext&,uint8_t* base,NativeCameraLookResult&);
// Explicit native Room extra-camera capability. Keeps the ordinary primary
// replay contract unchanged; validates live owned mode and native profile0/1.
bool NativeOwnedCameraLookBindingFor(uint32_t mode,PPCContext&,uint8_t*,NativeCameraLookBinding&,unsigned user);
bool ReplayNativeOwnedCameraLookAction(const NativeCameraLookBinding&,const LiveLookInput&,PPCContext&,uint8_t*,NativeCameraLookResult&);
bool DispatchNativeOwnedCameraLook(const NativeCameraLookBinding&,const NativeCameraLookResult&,PPCContext&,uint8_t*);
void TraceNativeOwnedLookDefinitions(uint32_t mode,PPCContext&,uint8_t*,unsigned user);
void ResetNativeCameraLookBindings();
bool TestNativeCameraLookAction();
}

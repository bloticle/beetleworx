#pragma once
#include <cstdint>
#include <memory>
#include "ppc_context.h"
namespace rex::ui {class Window;}
namespace em2 {
void NotifyMouseKeyboardInput();
void NotifyControllerInput(unsigned user,uint16_t buttons,uint8_t left,uint8_t right,int16_t lx,int16_t ly,int16_t rx,int16_t ry);
void NotifyMappedAimInput(unsigned user,bool aiming);
void TickMouseCursor(PPCContext&,uint8_t*);
struct MouseLookAngles {float yaw=0,pitch=0;};
// Simulation-thread native camera admission. No input/pose extrapolation.
MouseLookAngles TakeMouseLook(uint32_t mode,bool allowed);
bool MouseLookHasMotion(uint32_t mode);
void ResetMouseLook();
class MouseLookHost {
public:
    explicit MouseLookHost(rex::ui::Window*);
    ~MouseLookHost();
    MouseLookHost(const MouseLookHost&)=delete;
    MouseLookHost& operator=(const MouseLookHost&)=delete;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
bool TestMouseLook();
}

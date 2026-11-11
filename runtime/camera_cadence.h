#pragma once
#include <cstdint>
#include "ppc_context.h"

namespace em2 {
void BeginCameraPresentationFrame(bool fixed,double elapsed_seconds);
void ResetCameraPresentation();
bool HandleCameraPresentationUpdate(PPCContext&,uint8_t*,PPCFunc* native_update);
void CaptureCameraPresentationTarget(PPCContext&,uint8_t*,uint64_t tick);
void RenderPresentationCamera(PPCContext&,uint8_t*,double alpha);
bool PresentationOwnsCameraNode(uint32_t object);
bool CameraPresentationInputAllowed();
bool TestCameraPresentation();
}

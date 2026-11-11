#pragma once
#include <cstdint>
struct PPCContext;
namespace em2::em1 {
// Default-off, private NiStream fixture. Invoke after successful native scene
// begin on its loading thread, once engine streaming factories are initialized.
// Particle construction instead runs from an idle gameplay tick (world_tick).
// Does not attach objects to a scene, submit a draw, or alter the live parser.
void MaybeValidateNativeModel(PPCContext& ctx,uint8_t* base,bool world_tick=false);
}

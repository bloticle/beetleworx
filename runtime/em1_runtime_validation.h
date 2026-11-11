#pragma once
#include <cstdint>
struct PPCContext;
namespace em2::em1 {
// Opt-in startup diagnostic, called AFTER an existing native scene begin.
// Reads the configured mounted RVZ and validates real entry-scene headers in
// private parser objects. It never pumps/activates those scenes or changes the
// live world. Root owns adding the caller to the existing scene boundary.
void MaybeValidateNativeLevel(PPCContext& ctx, uint8_t* base,bool world_tick=false);
// Developer-only private-source clip sampling; never attaches an actor/world.
void MaybeValidateNativeAnimations(PPCContext& ctx, uint8_t* base);
}

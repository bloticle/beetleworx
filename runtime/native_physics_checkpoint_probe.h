#pragma once
#include <cstdint>
struct PPCContext;
namespace em2 {
// Explicit private original-VM boundary only. Constructs a separate original
// Havok world, never inserts test bodies into the actual level's physics world.
// Also inventories/captures/releases the actual level world when present. A
// separate private opt-in immediately restores that physics component at the
// same boundary; it never rewinds a complete simulation frame.
bool TestNativePhysicsCheckpoint(PPCContext&,uint8_t*,uint32_t,uint32_t,unsigned);
}

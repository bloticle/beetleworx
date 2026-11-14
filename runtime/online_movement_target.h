#pragma once
#include <cstdint>
struct PPCContext;
namespace em2::net {
// Original HumanController +24/+284 is a registry handle for an authored
// movement frame, not an input lock. No guest pointer is supplied over the wire.
bool ValidateNativeMovementTarget(uint32_t registry,uint32_t handle,PPCContext&,uint8_t* base);
}

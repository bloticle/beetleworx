#pragma once
#include <array>
#include <cstdint>
#include <span>
struct PPCContext;
namespace em2 {
struct NativeVisualRetirement {uint32_t entity=0,handle=0;};
// Exact seven-component particle-only native shape, already in the original
// retirement queue. Reads native readiness; never advances time or forces it.
bool ReadNativeParticleRetirement(uint8_t*,uint32_t registry,uint32_t entity,std::span<const uint32_t> humans,bool& ready);
bool TestNativeParticleRetirement();
// Completed native update, original world owner only. Uses original teardown
// and queue flush; no pointer or callback address is received over the network.
// Success with pending.entity != 0 explicitly retains queued visual cleanup;
// it is NOT registry absence. The caller must observe its bound identity until
// native removal, with a bounded lifetime and fresh-world reset.
bool ReplayNativeEntityRetirement(PPCContext&,uint8_t*,uint32_t registry,
    const std::array<uint32_t,4>& id,std::span<const uint32_t> humans,bool& requested,NativeVisualRetirement& pending,const char*& error);
}

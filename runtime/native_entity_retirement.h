#pragma once
#include <array>
#include <cstdint>
#include <span>
struct PPCContext;
namespace em2 {
// Completed native update, original world owner only. Uses original teardown
// and queue flush; no pointer or callback address is received over the network.
bool ReplayNativeEntityRetirement(PPCContext&,uint8_t*,uint32_t registry,
    const std::array<uint32_t,4>& id,std::span<const uint32_t> humans,bool& requested,const char*& error);
}

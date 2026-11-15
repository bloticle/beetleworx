#pragma once
#include <cstdint>
#include <optional>
struct PPCContext;
namespace em2 {
// Completed game-thread boundary. Original Health-property dispatch, no raw
// member writes or fabricated Damage event. Does not replicate death effects.
std::optional<int32_t> ReadNativeActorHealth(PPCContext&,uint8_t* base,uint32_t entity);
bool CorrectNativeActorHealth(PPCContext&,uint8_t* base,uint32_t entity,int32_t health);
}

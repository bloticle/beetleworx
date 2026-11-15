#pragma once
#include "online_projectile_impact.h"
namespace em2::net {
// Original DeathRewardComponent event 16. Local event pointers, entity handles
// and component addresses are reconstructed by the native adapter, never sent.
struct DeathReward {
    SpawnId owner{};
    uint32_t reason=0;
    std::array<uint32_t,4> before{},after{}; // Component words +76..+88.
    uint32_t random_before=0,random_after=0;
    std::vector<ImpactClone> clones;
    bool operator==(const DeathReward&)const=default;
};
inline constexpr size_t kMaxDeathRewardBytes=1024,kAuthorityDeathRewardBatch=32;
bool ValidDeathReward(const DeathReward&);
std::vector<uint8_t> EncodeDeathReward(const DeathReward&);
bool DecodeDeathReward(std::span<const uint8_t>,DeathReward&);
bool TestDeathRewardCodec();
}

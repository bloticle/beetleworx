#pragma once
#include "online_spawn_batch.h"
namespace em2::net {
struct ImpactClone {
    SpawnId prototype{},entity{};
    std::array<uint32_t,2> flags{};
    uint32_t random_before=0,random_after=0;
    bool operator==(const ImpactClone&)const=default;
};
struct ProjectileImpact {
    SpawnId projectile{};
    std::array<uint32_t,3> position{};
    uint32_t random_before=0,random_after=0;
    bool callback=false,queued=false;
    std::vector<ImpactClone> clones;
    bool operator==(const ProjectileImpact&)const=default;
};
inline constexpr size_t kMaxImpactClones=16,kMaxImpactBytes=1024,kAuthorityImpactBatch=64;
bool ValidProjectileImpact(const ProjectileImpact&);
std::vector<uint8_t> EncodeProjectileImpact(const ProjectileImpact&);
bool DecodeProjectileImpact(std::span<const uint8_t>,ProjectileImpact&);
bool TestProjectileImpactCodec();
}

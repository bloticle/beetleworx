#pragma once
#include "online_projectile_impact.h"
namespace em2::net {
struct ShooterLaunch {
    SpawnReference owner;
    std::vector<SpawnId> projectiles; // Native pending-list order, never handles.
    uint32_t random_before=0,random_after=0;
    bool success=false;
    std::vector<ImpactClone> clones;
    bool operator==(const ShooterLaunch&)const=default;
};
inline constexpr size_t kMaxLaunchBytes=2048,kAuthorityLaunchBatch=32;
bool ValidShooterLaunch(const ShooterLaunch&);
std::vector<uint8_t> EncodeShooterLaunch(const ShooterLaunch&);
bool DecodeShooterLaunch(std::span<const uint8_t>,ShooterLaunch&);
bool TestShooterLaunchCodec();
}

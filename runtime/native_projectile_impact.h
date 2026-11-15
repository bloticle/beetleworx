#pragma once
#include "online_projectile_impact.h"
#include "online_shooter_launch.h"
#include <optional>
struct PPCContext;
namespace em2 {
struct NativeImpactCloneTicket {void* scope=nullptr;size_t index=0;};
std::optional<NativeImpactCloneTicket> BeginNativeImpactClone(PPCContext&,uint8_t*);
void EndNativeImpactClone(const NativeImpactCloneTicket&,PPCContext&,uint8_t*);
bool ReplayNativeProjectileImpact(PPCContext&,uint8_t*,uint32_t entity,const net::ProjectileImpact&,const char*& error);
bool ReadNativeShooterPending(uint8_t*,uint32_t shooter,net::ShooterLaunch&,uint32_t& owner);
bool ReplayNativeShooterLaunch(PPCContext&,uint8_t*,uint32_t shooter,const net::ShooterLaunch&,const char*& error);
}

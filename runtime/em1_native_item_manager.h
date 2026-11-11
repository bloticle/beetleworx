#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
struct PPCContext;
namespace em2::em1 {
enum class NativeAmmoKind : uint32_t { Paint=0, Thinner=1, Electric=2 };
struct NativeItemAmmo {
  float amount=0,effective_max=0,configured_max=0,segment_amount=0;
  int32_t max_segments=0;
  float penalty_amount=0,penalty_wait=0,recharge_max=0,recharge_rate=0,recharge_wait=0;
};
struct NativeItemManagerSnapshot {
  uint32_t address=0;
  std::array<NativeItemAmmo,3> ammo;
  int32_t etickets=0,eticket_limit=0;
  bool amount_writes_blocked=false;
};
// Actual EM2 singleton/records used by its registered ItemManager Lua methods.
// Engine-thread only; no parallel EM1 inventory or recharge simulation.
bool ReadNativeItemManager(uint8_t*,NativeItemManagerSnapshot&,std::string&);
// Use retail setters, including their existing profile/event notifications.
bool SetNativeItemAmmo(PPCContext&,uint8_t*,NativeAmmoKind,float,std::string&);
bool SetNativeItemAmmoMax(PPCContext&,uint8_t*,NativeAmmoKind,float,std::string&);
// Legacy component rule names delegate to the existing native rule cache used
// by ItemManager's update. Cost/jump-cost properties are not manager rules and
// must continue through the component's authored/native instance properties.
bool ReadLegacyAmmoRule(uint8_t*,std::string_view property,float&,std::string&);
bool WriteLegacyAmmoRule(uint8_t*,std::string_view property,float,std::string&);
// The legacy player facade may forward these six names only. NPC/component
// local fields and unsupported legacy items must stay in native instance
// property storage; this does not bind an actor to the global player manager.
bool IsNativeInventoryProperty(std::string_view);
bool ReadNativeInventoryProperty(uint8_t*,std::string_view,float&,std::string&);
bool WriteNativeInventoryProperty(PPCContext&,uint8_t*,std::string_view,float,std::string&);
// Calls real channel lookup and same-value retail setters. No settings/profile
// mutation intended; verifies the entire sampled singleton prefix is unchanged.
bool ProbeNativeItemManager(PPCContext&,uint8_t*,uint32_t private_scratch,
                            NativeItemManagerSnapshot&,std::string&);
// Explicit private-save diagnostic: changes currency and its cap, exercises
// native clamping/notification paths, then restores the original values.
bool ProbeNativeInventoryWrites(PPCContext&,uint8_t*,std::string&);
}

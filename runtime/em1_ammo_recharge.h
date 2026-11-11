#pragma once
#include <array>
#include <optional>

namespace em2::em1 {
// GetAmmo's actual cheat-mode return value (REL rodata+108160), not FLT_MAX.
inline constexpr float kLegacyInfiniteAmmoAmount = 100.0f;
// Adapter contract for the original InventoryComponent, named component
// properties and engine-clock stopwatches. Implementations own those objects;
// this interface neither manufactures inventory nor substitutes a host clock.
class LegacyAmmoRechargeTimers {
 public:
  virtual ~LegacyAmmoRechargeTimers() = default;
  virtual bool TimerActive(unsigned channel) = 0;
  virtual float Elapsed(unsigned channel) = 0;
  virtual void RestartTimer(unsigned channel) = 0;
  virtual void ClearTimer(unsigned channel) = 0;
};
class LegacyAmmoRechargeAccess : public LegacyAmmoRechargeTimers {
 public:
  virtual ~LegacyAmmoRechargeAccess() = default;
  virtual float RechargeRate(unsigned channel) = 0;
  virtual float RechargeMax(unsigned channel) = 0;
  virtual std::optional<float> InventoryLimit(unsigned channel) = 0;
  // Includes the original infinite/shared-ammo handling, not a cached amount.
  virtual float Amount(unsigned channel) = 0;
  virtual void Add(unsigned channel, float delta) = 0;
};

// Complete retail recharge Update control flow (REL file2E42B4..2E4508).
// The caller supplies the actual engine delta and persists the two cooldowns.
// Class registration, owner binding and timers remain the caller's responsibility.
void TickLegacyAmmoRecharge(LegacyAmmoRechargeAccess&, float engine_delta,
                           std::array<float, 2>& cooldowns);
}  // namespace em2::em1

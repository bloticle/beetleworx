#include "em1_ammo_recharge.h"

namespace em2::em1 {
void TickLegacyAmmoRecharge(LegacyAmmoRechargeAccess& access, float engine_delta,
                           std::array<float, 2>& cooldowns) {
  // Retail reuses f29 for elapsed time. Preserve that cross-channel behavior:
  // after sampling the paint timer, thinner's cooldown also consumes this value.
  // Deliberately do not replace it with a freshly sampled delta each iteration.
  float elapsed = engine_delta;
  for (unsigned channel = 0; channel < 2; ++channel) {
    if (cooldowns[channel] > 0.0f) {
      cooldowns[channel] -= elapsed;
      continue;
    }
    const float rate = access.RechargeRate(channel);
    float maximum = access.RechargeMax(channel);
    const auto inventory_limit = access.InventoryLimit(channel);
    if (inventory_limit && maximum > *inventory_limit) maximum = *inventory_limit;
    if (access.Amount(channel) < maximum) {
      if (access.TimerActive(channel)) elapsed = access.Elapsed(channel);
      // This is a strict >0.5 second test in the retail code. Recharge is
      // accumulated in batches, not unconditionally rate*delta every frame.
      if (!access.TimerActive(channel) || elapsed > 0.5f) {
        access.RestartTimer(channel);
        access.Add(channel, rate * elapsed);
      }
    }
    // Add may clamp, remap a shared pool, or fail. Read the real amount again.
    if (access.Amount(channel) >= maximum) access.ClearTimer(channel);
  }
}
}  // namespace em2::em1

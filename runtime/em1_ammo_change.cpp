#include "em1_ammo_change.h"
#include <cmath>

namespace em2::em1 {
std::span<const AmmoPropertyDeclaration> LegacyAmmoProperties(){
  static constexpr AmmoPropertyDeclaration properties[]={
    {"PaintCost",0},{"ThinnerCost",0},{"PaintJumpCost",0},{"ThinnerJumpCost",0},
    {"PaintRechargeRate",0.5f},{"ThinnerRechargeRate",0.5f},
    {"PaintRechargeMax",10},{"ThinnerRechargeMax",10},
    {"PaintRechargeWait",1},{"ThinnerRechargeWait",1},
    {"PaintPenaltyAmount",1},{"ThinnerPenaltyAmount",1},
    {"PaintPenaltyWait",0.5f},{"ThinnerPenaltyWait",0.5f}};
  return properties;
}
bool ChangeLegacyAmmo(float current, float requested_delta,
    std::optional<float> owner_limit, const AmmoSpendDelays& delays,
    AmmoChange& output) {
  if (!std::isfinite(current) || !std::isfinite(requested_delta) ||
      (owner_limit && !std::isfinite(*owner_limit)) ||
      !std::isfinite(delays.normal) || !std::isfinite(delays.penalty) ||
      !std::isfinite(delays.penalty_threshold)) return false;
  AmmoChange result;
  result.applied_delta = requested_delta;
  if (requested_delta > 0.0f) {
    // RechargeMax is checked by Update. AddAmmo uses the owner's optional cap.
    const float remaining = owner_limit ? *owner_limit - current : 0.0f;
    if (owner_limit && requested_delta >= remaining) {
      result.amount = *owner_limit;
      result.applied_delta = remaining;
    } else {
      result.amount = current + requested_delta;
    }
  } else {
    if (-requested_delta >= current) {
      result.amount = 0.0f;
      result.applied_delta = -0.0f;
    } else {
      result.amount = current + requested_delta;
    }
    result.recharge_delay = result.amount < delays.penalty_threshold
        ? delays.penalty : delays.normal;
  }
  output = result;
  return true;
}
}  // namespace em2::em1

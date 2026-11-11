#pragma once
#include <optional>
#include <span>
#include <string_view>

namespace em2::em1 {
struct AmmoPropertyDeclaration {std::string_view name;float default_value;};
// Retail compiled class defaults. Authored GSA values override these defaults.
std::span<const AmmoPropertyDeclaration> LegacyAmmoProperties();
// AddAmmo arithmetic only; owner inventory access and lifecycle are separate.
struct AmmoSpendDelays {
  float normal = 0;
  float penalty = 0;
  float penalty_threshold = 0;
};
struct AmmoChange {
  float amount = 0;
  float applied_delta = 0;
  std::optional<float> recharge_delay;
};
// Original finite-input rules, including depletion returning -0 and the
// zero-delta spending path. Invalid input preserves output.
bool ChangeLegacyAmmo(float current, float requested_delta,
    std::optional<float> owner_limit, const AmmoSpendDelays& delays,
    AmmoChange& output);
}  // namespace em2::em1

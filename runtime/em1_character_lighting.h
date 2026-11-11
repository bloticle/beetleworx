#pragma once
#include "../private/em1_character_lighting_data.h"
#include <algorithm>
#include <optional>
namespace em2::em1 {
// Match the entire native pixel program, including its register operands.
// Other actors must be sampled at their own positions before adding bindings.
inline std::optional<unsigned> CharacterLightingRegister(std::span<const uint32_t> code) {
  for(const auto& binding:character_lighting_data::bindings)
    if(code.size()==binding.code.size() && std::equal(code.begin(),code.end(),binding.code.begin()))return binding.first;
  return std::nullopt;
}
}

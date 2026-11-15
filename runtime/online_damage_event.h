#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace em2::net {
using DamageEntityId=std::array<uint32_t,4>;
struct DamageStimulus {
    uint32_t type=0,raw_bits=0,effective_bits=0;
    bool operator==(const DamageStimulus&)const=default;
};
// Portable native event data. No local handles, pointers, allocation capacities
// or registry revisions are transmitted. Float words preserve signed zero and
// the exact adjusted amounts; replay must not recalculate host damage locally.
struct DamageEvent {
    DamageEntityId target{};
    std::optional<DamageEntityId> instigator;
    // Human actors use the scoped lobby peer instead of a respawn-generated
    // native GUID. Exactly one target identity representation is permitted.
    uint8_t target_peer=0,instigator_peer=0;
    std::array<uint8_t,2> flags{};
    std::array<uint32_t,8> vectors{};
    std::vector<DamageStimulus> stimuli;
    // Child shape keys within the target's collision asset, not body pointers.
    std::vector<uint32_t> shape_keys;
    bool tag_present=true;
    std::string tag;
    bool operator==(const DamageEvent&)const=default;
};
inline constexpr size_t kMaxDamageStimuli=16,kMaxDamageTag=255,kMaxDamageShapeKeys=512,kMaxDamageEventBytes=4096;
bool ValidDamageEvent(const DamageEvent&);
std::vector<uint8_t> EncodeDamageEvent(const DamageEvent&);
// Transactional: malformed/truncated/extra bytes never change the output.
bool DecodeDamageEvent(std::span<const uint8_t>,DamageEvent&);
bool TestDamageEventCodec();
}

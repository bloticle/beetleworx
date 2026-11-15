#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>
namespace em2::net {
using SpawnId=std::array<uint32_t,4>;
struct SpawnReference {
    SpawnId id{};
    uint8_t peer=0; // Stable human identity across native respawn.
    bool operator==(const SpawnReference&)const=default;
};
struct SpawnChild {
    SpawnId prototype{},entity{};
    std::array<uint32_t,3> arguments{}; // Native SpawnOne r6/r7/r8; no pointers.
    bool operator==(const SpawnChild&)const=default;
};
// The audited 827B3C60 operation, not a generic arbitrary engine RPC. Native
// component/anchor/template pointers and the output collection never travel.
// Outer authority tick/world/roster scope owns ordering. Nested calls retain
// their depth; a receiver must not flatten nested side effects into extra calls.
struct SpawnBatch {
    SpawnReference owner,anchor;
    int32_t selection=-1;
    bool flag=false,collection=false;
    uint8_t depth=0;
    std::array<uint32_t,3> before{},after{}; // Component +104/+140/+144.
    bool random_present=false;
    uint32_t random_before=0,random_after=0;
    SpawnId result{}; // Null is a valid native zero-birth result.
    std::vector<SpawnChild> children;
    bool operator==(const SpawnBatch&)const=default;
};
inline constexpr size_t kMaxSpawnChildren=64,kMaxSpawnBatchBytes=4096;
bool ValidSpawnBatch(const SpawnBatch&);
std::vector<uint8_t> EncodeSpawnBatch(const SpawnBatch&);
// Transactional, bounded, big-endian data only; never allocates native objects.
bool DecodeSpawnBatch(std::span<const uint8_t>,SpawnBatch&);
enum class SpawnEligibility {Original,Suppress,InvalidPrefix};
// Preserve host zero-birth calls and operations for an owner still present in
// the unconsumed sequence. Never hide a prior capture mismatch by cancelling a
// later call. Native scope/caller ABI checks belong to the execution adapter.
SpawnEligibility EvaluateSpawnEligibility(std::span<const SpawnBatch> host,std::span<const SpawnBatch> completed,const SpawnReference& owner);
bool TestSpawnBatchCodec();
}

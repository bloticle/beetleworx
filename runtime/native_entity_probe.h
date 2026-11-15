#pragma once
#include <array>
#include <cstdint>
#include <vector>
namespace em2 {
struct NativeEntityRecord {
    std::array<uint32_t,4> id{};
    uint32_t handle=0,vtable=0;
    uint32_t debug_address=0; // Diagnostic only; excluded from canonical bytes.
    std::array<uint8_t,32> component_mask{};
    bool has_position=false;
    std::array<uint32_t,3> position_bits{};
    bool operator==(const NativeEntityRecord&)const=default;
};
struct NativeEntitySnapshot {
    uint32_t registry=0,header_word_12=0,bucket_count=0,component_count=0;
    std::vector<NativeEntityRecord> records;
};
// Local restore preflight only. Addresses never belong in a network snapshot.
// This observes ownership without retaining objects or undoing their teardown.
struct NativeEntityOwnershipRecord {
    std::array<uint32_t,4> id{};
    uint32_t handle=0,entity=0,vtable=0;
    std::vector<uint8_t> type_slots;
    // Native component interface address and vtable, in native slot order.
    std::vector<std::array<uint32_t,2>> components;
    bool operator==(const NativeEntityOwnershipRecord&)const=default;
};
struct NativeEntityOwnership {
    uint64_t lifetime_revision=0;
    uint32_t registry=0,owner=0,world=0;
    std::vector<NativeEntityOwnershipRecord> entities;
    bool operator==(const NativeEntityOwnership&)const=default;
};
// Completed simulation-owner boundary only. Rejects pending native destruction
// and malformed/replaced component inventories before publishing any result.
// Includes a process-local fence for the audited registry lifecycle hooks.
// Equality does not prove unchanged component/property lifetimes or full state.
bool ReadNativeEntityOwnership(uint8_t*,NativeEntityOwnership&,const char*& error,uint32_t& address);
// Bounded tick-owner inspection; no native callbacks or retained guest pointers.
// Does not pin guest objects against concurrent native destruction.
bool ReadNativeEntitySnapshot(uint8_t*,NativeEntitySnapshot&,const char*& error,uint32_t& address,bool include_positions=true);
void TraceNativeEntitySnapshot(uint8_t*,uint64_t tick);
bool TestNativeEntityProbe();
}

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
// Bounded tick-owner inspection; no native callbacks or retained guest pointers.
// Does not pin guest objects against concurrent native destruction.
bool ReadNativeEntitySnapshot(uint8_t*,NativeEntitySnapshot&,const char*& error,uint32_t& address);
void TraceNativeEntitySnapshot(uint8_t*,uint64_t tick);
bool TestNativeEntityProbe();
}

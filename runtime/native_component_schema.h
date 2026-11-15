#pragma once
#include "native_entity_probe.h"
#include <optional>
#include <span>
#include <string>
#include <vector>
struct PPCContext;
namespace em2 {
struct NativePropertySchema {
    uint32_t index=0,descriptor=0,table=0,setter=0,getter=0;
    std::string name,type;
    std::optional<bool> array,direct;
};
struct NativeComponentSchema {
    uint32_t primary_table=0,interface_table=0,example=0,instances=0;
    uint32_t type_getter=0,count_getter=0,descriptor_getter=0;
    uint32_t type=0;
    std::string name;
    std::optional<uint32_t> properties;
    std::vector<NativePropertySchema> declarations;
};
struct NativeCachedPropertyBox {
    uint32_t address=0,table=0;
    NativePropertySchema property;
};
struct NativeComponentPropertyCache {
    uint32_t object=0,data=0,count=0,capacity=0;
    std::vector<NativeCachedPropertyBox> boxes;
};
// Reads existing cache allocations only; never invokes lazy property creation.
bool ReadNativeComponentPropertyCaches(uint8_t*,const NativeEntityOwnership&,std::vector<NativeComponentPropertyCache>&,std::string&);
// Read-only schema discovery from a completed, exclusively owned native entity
// inventory. Only audited constant/global-load metadata leaves are decoded;
// unknown methods are reported, never called with a guessed ABI.
// Declared properties are not all mutable component state or a checkpoint.
bool ReadNativeComponentSchemas(uint8_t*,const NativeEntityOwnership&,std::vector<NativeComponentSchema>&,std::string&);
bool ProbeNativeComponentSchemas(PPCContext&,uint8_t*,const NativeEntityOwnership&);
}

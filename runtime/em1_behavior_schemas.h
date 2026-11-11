#pragma once
#include "em1_behavior_upgrade.h"
#include <span>
struct PPCContext;
namespace em2::em1 {
// Call on the initialized native title thread, with its real PPC stack. Reads
// checked stock reflection and invokes only native hkClass::getSignature(0).
// Copies all schema/default/enum ownership; never retains guest pointers.
bool ReadNativeBehaviorSchemas(PPCContext&,uint8_t* base,
    std::span<const std::string> requested,NativeBehaviorSchemas&,std::string& error);
bool TestNativeBehaviorSchemaRejection(std::string& error);
}

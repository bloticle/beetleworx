#pragma once
#include "em1_behavior_upgrade.h"
#include "em1_native_resources.h"
struct PPCContext;
namespace em2::em1 {
struct BehaviorProbeInput { std::string origin;std::vector<uint8_t> original; };
struct BehaviorProbeResourceResult {
 std::string origin,error;
 uint32_t objects=0,semantic_migrations=0,dependencies=0,verified_fields=0;
 bool cpu_checked=false,converted=false,loaded=false,fields_match=false,released=false;
};
struct BehaviorProbeResult {
 std::vector<BehaviorProbeResourceResult> resources;
 // A failed native release remains owned here; caller must retain this result
 // and retry cleanup before native pack-manager shutdown.
 std::vector<NativeHavokResource> retained;
 bool records_restored=false;
};
// Private diagnostic only: stock load/finish/serialized-field/readback/release;
// no graph activation, clip substitution, entity attachment or world admission.
bool ProbeNativeBehaviorResources(PPCContext&,uint8_t* base,uint32_t manager,
 std::shared_ptr<RvzSource>,std::span<const BehaviorProbeInput>,
 BehaviorProbeResult&,std::string& error);
}

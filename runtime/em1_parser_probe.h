#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <map>
#include <vector>
#include <span>
#include <array>
#include <atomic>
#include <string_view>
struct PPCContext;
namespace em2::em1 {
struct SceneAdapterResult;
struct NativeBlueprintReport {uint32_t entities=0,components=0,properties=0,authored_ammo_probes=0,authored_inventory_probes=0;};
// Optional retained definitions transfer ownership to the caller. Keep all input
// scratch allocations alive until ReleaseNativeBlueprints returns.
bool ProbeNativeSceneBlueprints(PPCContext&,uint8_t*,uint32_t scratch,size_t scratch_size,const SceneAdapterResult&,NativeBlueprintReport&,std::string&,std::vector<uint32_t>* retained=nullptr);
// Production reader: retains definitions and their original parameter data;
// performs no component factory probes or actor/world activation. Caller owns
// the guest input allocation through ReleaseNativeBlueprints.
bool ReadNativeSceneBlueprintDefinitions(PPCContext&,uint8_t*,uint32_t,size_t,
    const SceneAdapterResult&,NativeBlueprintReport&,std::string&,std::vector<uint32_t>&);
using NativeBlueprintIndex=std::map<std::array<uint32_t,4>,uint32_t>;
class NativeBlueprintParentScope {
 public:
  explicit NativeBlueprintParentScope(const NativeBlueprintIndex*,std::atomic<uint64_t>* resolutions=nullptr);
  ~NativeBlueprintParentScope();
  NativeBlueprintParentScope(const NativeBlueprintParentScope&)=delete;
  NativeBlueprintParentScope& operator=(const NativeBlueprintParentScope&)=delete;
 private:const NativeBlueprintIndex* previous_;std::atomic<uint64_t>* previous_counter_;
};
void ReleaseNativeBlueprints(PPCContext&,uint8_t*,std::vector<uint32_t>&);
struct NativeInheritanceReport {uint32_t unique=0,duplicates=0,parents=0,components=0,inherited_components=0,component_links=0;};
// One explicitly selected level's definitions only. No global blueprint lookup,
// publication, component factory calls or actor/world activation.
bool LinkNativeBlueprintInheritance(PPCContext&,uint8_t*,std::span<const uint32_t>,NativeInheritanceReport&,std::string&);
bool NativeSceneConstructionActive();
// Supply the exact retained original master to the native linker while an
// explicitly owned source-definition scope is active.
void ResolveNativeBlueprintParent(PPCContext&,uint8_t*);
// Read the actual creation-callback map used by retail 82510678. Component
// type IDs alone do not establish whether an object can be constructed.
bool ReadNativeComponentFactories(uint8_t* base,std::map<std::string,uint32_t>& output,std::string& error);
inline constexpr size_t kParserProbeScratchSize = 1024;
// Developer-only, deliberately not installed in a runtime hook. Call from an
// initialized engine loading/game thread with its valid PPC context. scratch
// must be a PRIVATE, zeroed, writable guest allocation (at least 1024 bytes),
// not an active parser, stack frame, asset buffer, or world object. The caller
// retains ownership and may release it only after this synchronous call returns.
// Exercises a synthetic empty EM1 scene, NOT an original playable level.
bool ProbeNativeSceneParser(PPCContext& ctx, uint8_t* base, uint32_t scratch,
                            size_t scratch_size, std::string& error);
// Same native decoder, actual adapted EM1 scene. Begins deferred and cancels
// BEFORE entity pumping; therefore no entity/component/world activation.
// Private zero scratch must hold1024 bytes plus the entire adapted scene.
bool ProbeNativeSceneHeader(PPCContext& ctx, uint8_t* base, uint32_t scratch,
                            size_t scratch_size, const SceneAdapterResult& scene,
                            std::string& error);
struct SceneConstructionReport {uint32_t placements=0,components=0;size_t navigation_instances=0,navigation_graphs=0;};
// Complete original adapted scene, including all definitions and placements.
// Pumps real entity/component constructors but NEVER inserts/admit a world.
// Scratch follows the header contract; all parser-owned objects are released.
// This does not prove component value parity, asset loading or gameplay.
bool ProbeNativeSceneConstruction(PPCContext&,uint8_t*,uint32_t scratch,size_t scratch_size,
    const SceneAdapterResult&,SceneConstructionReport&,std::string& error,std::span<const uint32_t> source_definitions={},bool registered_scope=false,std::string_view scene_name={});
}

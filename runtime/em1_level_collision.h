#pragma once
#include "em1_level_source.h"
#include "em1_havok_collision.h"
#include "em1_havok_collision_world.h"
#include <map>
#include <memory>
#include <set>
namespace em2::em1 {
struct CollisionPlacement {std::array<float,16> pose=kIdentityCollisionPlacement;float scale=1;};
struct LevelCollisionPlan {
 struct NativeHull {std::string origin;std::vector<uint8_t> bytes;
   std::shared_ptr<const AuthoredStaticCollision> acceleration_proof,fallback_geometry;};
 struct NativeBinding {size_t scene=0;uint32_t component=0;std::string path;std::shared_ptr<const NativeHull> resource;};
 std::map<uint32_t,std::shared_ptr<const AuthoredStaticCollision>> components;
 std::vector<NativeBinding> native_bindings;
 std::set<uint32_t> native_components;
 std::map<uint32_t,CollisionPlacement> placements;
 std::vector<std::string> unsupported;
  std::vector<std::string> native_fallbacks;
 size_t references=0,resources=0;
};
bool PrepareLevelCollision(std::shared_ptr<RvzSource>,const LevelContents&,LevelCollisionPlan&,std::string&,
    std::string_view native_triangle_path={},bool native_triangle_accelerated=false);
// Must succeed before selected accelerated resources are published to the
// native static component. No body or system is added to a world here.
bool ValidateNativeStaticCollisionAcceleration(PPCContext&,uint8_t*,LevelCollisionPlan&,std::string&,bool allow_fallback=false);
bool AttachRegisteredLevelCollision(PPCContext&,uint8_t*,std::string_view,std::string&);
bool ReleaseRegisteredLevelCollision(PPCContext&,uint8_t*,std::string&);
void UpdateRegisteredLevelCollisionActivity(PPCContext&,uint8_t*);
std::shared_ptr<const LevelCollisionPlan> RegisteredLevelCollision(std::string_view state);
}

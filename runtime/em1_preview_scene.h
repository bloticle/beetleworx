#pragma once
#include "em1_level_source.h"
#include <array>
namespace em2::em1 {
struct AuthoredRuntimeLight {
  enum class Kind { Ambient,Directional,Point,Spot };
  uint32_t entity=0;
  std::string name;
  Kind kind=Kind::Point;
  std::array<float,3> position{},direction{},ambient{},diffuse{},attenuation{};
  float dimmer=0,range_start=0,range_end=0,inner_degrees=0,outer_degrees=0,spot_exponent=0;
  bool subtractive=false,raycast=false;
};
struct ArchitectureScene {
  struct EventLink {
    uint32_t source_entity=0;
    std::string source_name,output,input,target,parameters,target_group;
    float delay=0;
    uint16_t flags=0;
  };
  struct GroupMember {uint32_t source_entity=0;std::string source_name,group;};
  std::vector<GroupMember> group_members;
  uint64_t group_fingerprint=0;
  SceneAdapterResult scene,trigger_prototype,trigger_native_prototype,gear_prototype,particle_prototype;
  std::string origin_key, entity_name, model_path;
  uint32_t original_definitions=0, original_placements=0, resolved_ancestors=0;
  uint32_t source_entity_id=0;
  uint32_t gear_source_entity_id=0,particle_source_entity_id=0;
  std::string gear_source_entity_name;
  // Authored EM1 world coordinates and normalized colors, before renderer/SH
  // conversion. These records do not imply native EM2 light registration.
  std::vector<AuthoredRuntimeLight> runtime_lights;
  // Original placed-entity connections, resolved through prefab inheritance.
  // Source IDs are authored IDs, not native entity handles. Retention does not
  // imply that the corresponding actor/trigger is admitted or dispatched.
  std::vector<EventLink> event_links;
  uint64_t event_link_fingerprint=0;
};
// Explicit exploration/render preview projection, not a campaign scene.
// Resolves inheritance against the COMPLETE original scene before retaining
// the main courtyard's transform/scenegraph components. No script is no-oped.
// Physics is admitted independently from the authored HKX, not a render mesh.
bool ReadCourtyardArchitectureScene(std::shared_ptr<RvzSource>,
    std::string_view published_model_name,ArchitectureScene&,std::string& error,
    std::string_view gear_model_name={},std::string_view gear_physics_name={},unsigned gear_number=1,uint32_t particle_entity=0);
}

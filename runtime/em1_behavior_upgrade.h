#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace em2::em1 {
struct BehaviorEnumItem { int32_t value=0; std::string name; };
struct BehaviorMember {
  std::string name,klass;
  uint8_t type=0,subtype=0;
  uint16_t array_size=0,flags=0,offset=0;
  std::vector<BehaviorEnumItem> enumeration;
  // Missing means no native reflected default. Present empty means zero default.
  std::optional<std::vector<uint8_t>> default_bytes;
};
struct NativeBehaviorClass {
  std::string name,parent;
  uint32_t size=0,version=0,flags=0,signature=0;
  bool signature_verified=false;
  std::vector<BehaviorMember> members;
};
struct NativeBehaviorSchemas {
  // Owned schema from the actual target registry; callers must validate origin.
  std::map<std::string,NativeBehaviorClass> classes;
};
struct BehaviorDependency {
  std::string owner_class,member,path;
  uint32_t array_index=UINT32_MAX;
  // Native data-section string location, for transactional closure rebinding.
  uint32_t native_string_offset=0;
};
struct BehaviorUpgradeResult {
  std::vector<uint8_t> pack;
  std::vector<BehaviorDependency> dependencies;
  uint32_t objects=0,semantic_migrations=0;
};
// Pure transactional conversion. No native calls, asset publication or runtime
// graph activation. Incomplete/unknown serialized semantics fail explicitly.
bool UpgradeBehaviorResource(std::span<const uint8_t> original,
    const NativeBehaviorSchemas&,BehaviorUpgradeResult&,std::string& error);
bool TestBehaviorUpgrade(std::span<const uint8_t> original,
    const NativeBehaviorSchemas&,std::string& error);
}

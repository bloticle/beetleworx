#pragma once
#include "em1_pack_source.h"
#include "em1_scene_adapter.h"
#include <utility>

namespace em2::em1 {
struct LevelScene {
  std::string source_gsa;
  std::string archive;
  std::string binary_scene;
  std::string authored_flag;
};
struct LevelPartitionResource {
  enum class Kind { Scene, Audio, Ignored };
  Kind kind=Kind::Scene;
  size_t index=0;
  std::string authored_flag;
};
struct LevelPartition {
  std::string name;
  std::vector<LevelScene> scenes;
  std::vector<std::string> audio;
  // Original parser retains tokens but does not enqueue types other than
  // GSA/bank. Keep those authored records and their mixed-list positions.
  std::vector<std::string> ignored;
  // Original mixed GSA/bank sequence; section headings are comments.
  std::vector<LevelPartitionResource> resources;
  // Authored comment metadata, not automatic streaming transitions.
  std::vector<std::string> interiors;
  std::vector<std::string> destinations;
  std::vector<std::pair<std::string, std::string>> section_notes;
};
struct LevelEntryPartition {
  enum class Mode { Initial, Interior };
  std::string name;
  Mode mode=Mode::Initial;
};
struct LevelPlan {
  std::string origin_key;
  std::string descriptor;
  // Original text fields, including names/authors in original byte encoding.
  // No invented defaults or silent deletion of unsupported authored fields.
  std::vector<std::pair<std::string, std::string>> fields;
  // Original loader processes initial(mode0) before interior(mode1),
  // independent of their order in the text descriptor.
  std::vector<LevelEntryPartition> entry_partitions;
  std::vector<LevelScene> shared_scenes;
  std::vector<LevelPartition> partitions;
};
struct AdaptedSceneResource {
  std::string origin_key;
  SceneAdapterResult scene;
};
// Enumerates actual descriptors from the mounted game's levels.pak.
// IDs are filenames, including .level, suitable for ReadLevelPlan.
bool ListLevelDescriptors(std::shared_ptr<RvzSource> source,
                          std::vector<std::string>& output,std::string& error);
struct LevelSceneResource {
  LevelScene request;
  AdaptedSceneResource resource;
};
struct LevelContents {
  LevelPlan plan;
  // Unique shared + authored entry-partition scenes, in authored order.
  // Other partitions remain in plan for subsequent streaming transitions.
  std::vector<LevelSceneResource> entry_scenes;
};
// Transactional source preparation: retains complete adapted scenes, not the
// courtyard projection. Native entity/resource activation is a separate step.
bool ReadLevelContents(std::shared_ptr<RvzSource> source,std::string_view level,
                       LevelContents& output,std::string& error);
// Resolves authored level/partition descriptors directly from the original
// levels.pak in RVZ. This does not yet construct the EM2 v9 registry objects.
bool ReadLevelPlan(std::shared_ptr<RvzSource> source, std::string_view level,
                   LevelPlan& output, std::string& error);
bool TestLevelPartitionClassification(std::string& error);
// Source origin is explicit; only this EM1 path applies the EM1 scene adapter.
// Component, mesh, Havok, and Lua compatibility is separate from this layout.
bool ReadAdaptedScene(std::shared_ptr<RvzSource> source, const LevelScene& request,
                      AdaptedSceneResource& output, std::string& error);
}

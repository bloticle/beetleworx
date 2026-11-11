#pragma once
#include "em1_pack_source.h"
#include "em1_level_source.h"
#include "em1_scene_files.h"
#include <map>
namespace em2::em1 {
struct OriginalResourceLocation {std::string archive,path;std::array<uint8_t,4> type{};};
struct OriginalResourceCatalog {
  std::shared_ptr<RvzSource> source;
  std::string source_identity;
  size_t packfiles=0;
  // All original packfiles/*.pak directories plus loose source filenames.
  // Empty archive denotes a loose file; payloads are not read or converted.
  std::map<std::string,std::vector<OriginalResourceLocation>> resources;
};
enum class OriginalResourcePresence {InScope,OutsideScope,NotFound};
struct OriginalResourceQuery {
  OriginalResourcePresence presence=OriginalResourcePresence::NotFound;
  std::vector<OriginalResourceLocation> locations;
};
// Transactional, exact source metadata only. Failure to index ANY packfile
// rejects the catalog; an incomplete scan must never imply original absence.
bool BuildOriginalResourceCatalog(std::shared_ptr<RvzSource>,OriginalResourceCatalog&,std::string& error);
// Uses the FIRST authored archive scope with a match. Other level locations
// are diagnostic only, never an arbitrary cross-level loading fallback.
// NotFound covers this documented catalog, not external formats/root locale
// PACKv1 containers, converted readiness or optional component behavior.
bool QueryOriginalResource(const OriginalResourceCatalog&,std::string_view logical_path,
    std::span<const std::string> authored_archives,OriginalResourceQuery&,std::string& error);
bool ReadOriginalBootstrapArchives(const OriginalResourceCatalog&,std::vector<std::string>&,std::string& error);
struct SceneResourceLookup {size_t scene_index=0;SceneFileReference reference;OriginalResourceQuery query;};
// Complete declared-asset planning only, with original boot manifest and
// explicit level scopes. Retains unavailable and out-of-scope requirements;
// neither category is replaced, dropped or treated as converted/ready.
bool PlanLevelResourceScope(const OriginalResourceCatalog&,const LevelContents&,
    std::vector<SceneResourceLookup>&,std::string& error);
}

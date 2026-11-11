#pragma once
#include "em1_model_source.h"

namespace em2::em1 {
struct ModelClosure {
  std::shared_ptr<RvzSource> source;
  AdaptedModelResource model;
  std::vector<std::string> texture_origins;
  // Legacy source files may themselves reference an absent image. These are
  // preserved as native missing-image references, never fabricated pixels.
  std::vector<std::string> missing_texture_origins;
};
// Resolves original textures from the owner/global archive and, when explicitly
// supplied, the complete level's authored archives. Conflicting sibling assets
// fail instead of selecting an unrelated level variant. No EM2/file fallback.
// Unsupported dependencies fail without publishing a partially adapted model.
// Original legacy missing images remain native missing-image references only
// after a complete original-source index proves their filenames absent.
bool ReadModelClosure(std::shared_ptr<RvzSource> source, std::string_view archive,
                      std::string_view resource, ModelClosure& output,
                      std::string& error,const ModelLighting* lighting=nullptr,
                      std::span<const std::string> level_archives={});
}

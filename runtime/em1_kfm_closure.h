#pragma once
#include "em1_kfm.h"
#include "em1_keyframe.h"
#include "em1_model_closure.h"
namespace em2::em1 {
struct KfmModelDependency {std::string source_path,archive,resource;ModelClosure closure;};
struct KfmKeyframeDependency {std::string source_path,archive,resource;KeyframeResource keyframe;};
struct KfmClosure {
  std::shared_ptr<RvzSource> source;
  std::string origin_key,resource;
  std::vector<uint8_t> original;
  KfmDocument document;
  std::vector<KfmModelDependency> models;
  std::vector<KfmKeyframeDependency> keyframes;
};
// Resolve only the actual owner then explicit original global archive. Models
// retain their complete original texture/geometry dependencies. No unrelated
// level/EM2 fallback or fabricated replacement for missing dependencies.
bool ReadKfmClosure(std::shared_ptr<RvzSource>,std::string_view archive,std::string_view resource,
    KfmClosure&,std::string& error,std::span<const std::string> level_archives={});
}

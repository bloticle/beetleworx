#pragma once
#include "em1_pack_source.h"
#include "em1_nif_meshes.h"
namespace em2::em1 {
struct ModelLighting { std::string_view resource; uint32_t entity=0; };
struct AdaptedModelResource {
  std::string origin_key;
  std::vector<uint8_t> bytes;
  std::vector<MeshVertexOrigins> mesh_origins;
  uint32_t textures = 0;
  uint32_t baked_meshes = 0;
  uint32_t converted_jigsaw_aggregates = 0;
};
// Runtime path: original RVZ -> bounded archive/resource decode -> static mesh
// streams -> Wii texture adaptation, with explicit original-source identity.
// Unsupported formats fail closed. This prepares model bytes; actual engine
// loading and baked-light/paint/skin behavior still need separate validation.
bool ReadAdaptedModel(std::shared_ptr<RvzSource> source, std::string_view archive,
                      std::string_view resource, AdaptedModelResource& output,
                      std::string& error,const ModelLighting* lighting=nullptr);
}

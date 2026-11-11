#pragma once
#include <array>
#include "em1_gx_mesh.h"
namespace em2::em1 {
struct MeshAttributeOrigin {
  uint32_t stream_block = 0, region_start = 0, component = 0;
  std::string semantic;
  uint32_t semantic_index = 0;
};
struct MeshVertexOrigins {
  uint32_t mesh_block = 0;
  std::vector<MeshAttributeOrigin> attributes;
  GxMeshTopology topology;
};
struct NifMeshVariant {
  uint32_t block=0,modifier_count=0,embedded_skin_bytes=0;
  std::array<uint32_t,6> cooked_skin_counts{};
  uint16_t submeshes=0;
  uint8_t instanced=0,skin=0;
  std::vector<std::string> modifier_types,semantics;
  std::vector<uint32_t> formats;
};
// Inspect original deformation/stream requirements without flattening meshes
// or treating preserved platform-specific tails as converted native data.
bool InspectNifMeshVariants(std::span<const uint8_t>,std::vector<NifMeshVariant>&,std::string& error);
struct NifMeshResult {
  std::vector<uint8_t> bytes;
  std::vector<MeshVertexOrigins> meshes;
  // Unknown deformation variants remain opaque and must be rejected by the
  // runtime provider. Indexed skin preparation validates palette/vertex links
  // and preserves all deformation arrays; it does not attach an animated rig.
  uint32_t unadapted_skin_meshes = 0;
  uint32_t unadapted_modifier_meshes = 0;
  uint32_t prepared_indexed_skin_meshes = 0;
  uint32_t prepared_indexed_static_meshes = 0;
  uint32_t prepared_indexed_morph_meshes = 0;
  uint32_t prepared_gx_morph_meshes = 0;
  uint32_t promoted_fixed_uv_streams = 0;
};
// Converts static Wii display lists and prepares audited ordinary indexed skin
// streams. Unknown deformation/multi-submesh layouts are not flattened.
// Unchanged opaque blocks and their IDs are preserved. The retained index
// maps are REQUIRED by future baked-light/paint adapters; this is not a claim
// that those gameplay systems or the entire resulting NIF are engine-compatible.
// Input must come from an explicitly EM1-origin resource. Failure keeps output.
bool AdaptNifMeshes(std::span<const uint8_t> original,
                    NifMeshResult& output, std::string& error);
bool TestNifFixedPointUvPromotion(std::string& error);
}

#pragma once
#include "em1_nif_container.h"
#include <array>
#include <span>
#include <string>
#include <vector>

namespace em2::em1 {
struct JigsawTriangleProof {
  std::array<std::array<float,3>,3> vertices{};
  uint32_t key=0;
};
struct JigsawAggregateInfo {
  uint32_t triangle_parts=0,convex_parts=0,convex_shapes=0,key_bits=0,mopp_bytes=0;
  std::vector<JigsawTriangleProof> triangles;
  std::vector<std::array<float,3>> convex_vertices;
  // These flags remain false until the caller obtains real native VM/ray proof.
  bool native_load_proven=false,mopp_key_parity_proven=false;
};
struct ConvertedJigsawAggregate {
  nif::Bytes pack;
  JigsawAggregateInfo info;
};
struct ConvertedConvexBodyShape {
  nif::Bytes pack;
  std::vector<std::array<float,3>> vertices;
  std::vector<std::array<float,4>> planes;
  uint32_t source_body_offset=0;
  uint8_t motion_type=0;
};
// Extract only the direct convex shape from a single original rigid body.
// Does not replace that body's motion, mass, constraints, or native ownership.
bool ConvertConvexBodyShape(std::span<const uint8_t>,ConvertedConvexBodyShape&,std::string& error);
struct ConvertedConvexBodyResource {
  nif::Bytes pack;
  uint32_t body_offset=0;
  std::vector<uint32_t> body_offsets;
};
// Preserves the authored container, scene metadata and single convex rigid body.
// Rejects additional physical objects rather than silently dropping them.
bool ConvertConvexBodyResource(std::span<const uint8_t>,ConvertedConvexBodyResource&,std::string& error);
// Complete container with multiple convex bodies; retains every body, shared
// shape, name, filter and system-array reference with explicit relocations.
bool ConvertConvexBodiesResource(std::span<const uint8_t>,ConvertedConvexBodyResource&,std::string& error);
bool TestConvexBodiesResourceConversion(std::span<const uint8_t>,std::string& error);
// Complete fixed triangle-body container, retaining original metadata/system
// ownership and importing native2011 StorageExtendedMesh graphs. The source
// MOPP is deliberately not activated: native geometry/rays need separate proof.
// Optional original MOPP import needs independent native key/fraction parity
// before activation. False preserves the validated unaccelerated output bytes.
// The active shape root must have zero authored userdata: the stock component
// treats nonzero root userdata as an owned allocation during cleanup. Inactive
// authored userdata is preserved, never erased to force native eligibility.
bool ConvertStaticTriangleBodyResource(std::span<const uint8_t>,ConvertedConvexBodyResource&,std::string& error,bool accelerated=false);
bool TestStaticTriangleBodyResourceConversion(std::span<const uint8_t>,std::string& error);
// Single authored rigid body with a MOPP/list of convex vertices shapes.
// Retains child order, per-child filters, enabled masks and original MOPP code.
bool ConvertCompoundBodyResource(std::span<const uint8_t>,ConvertedConvexBodyResource&,std::string& error);
// One original box body, optionally wrapped in exact convex translations.
// Keeps the authored shape graph, margins, userData and body/filter state;
// does not flatten the wrapper into a replacement floor or mesh.
bool ConvertBoxBodyResource(std::span<const uint8_t>,ConvertedConvexBodyResource&,std::string& error);
bool TestBoxBodyResourceConversion(std::span<const uint8_t>,std::string& error);
// Original HK7 BE32 MoppTree -> native Xbox2011 aggregate, retaining all authored
// subpart order, keys, indices, winding, materials, convex planes and MOPP bytes.
// Narrow supported closure: plain ExtendedMesh plus ConvexVertices children.
bool ConvertJigsawAggregate(std::span<const uint8_t> original,
    ConvertedJigsawAggregate& result,std::string& error);
// Resolves external physics buffers from original version4 NIF suffix bindings.
// These are independent of GX render tuple expansion; original suffix links
// and source physics stream blocks remain intact.
// Transactional: only changes JPSJigsawNode block replacements after every
// selected original version4 block validates. Prefix/tail are opaque-preserved.
// Does not change the node's class or publish anything into a world.
bool AdaptJigsawHavokBlocks(nif::Nif& model,
    std::vector<JigsawAggregateInfo>& info,std::string& error,
    std::vector<nif::Bytes>* converted_packs=nullptr);
// Pure original/converted structural and semantic payload checks, not native VM.
bool TestJigsawAggregateConversion(std::span<const uint8_t> original,
    std::string& error);
}

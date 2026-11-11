#pragma once
#include "em1_jigsaw_havok.h"
#include <cstddef>
struct PPCContext;
namespace em2::em1 {
struct AuthoredCollisionBody;
struct NativeJigsawProof {
  uint32_t loaded_aggregates=0,compared_rays=0,hit_rays=0,miss_rays=0;
  std::vector<uint32_t> triangle_parts_covered,convex_parts_covered;
  float largest_fraction_error=0;
  uint32_t verified_triangle_ties=0;
};
// Synchronous, developer-only, initialized game-thread check. No world admission
// or references escape. Caller supplies private aligned32 zero guest scratch;
// loaded objects borrow the scratch pack and must not be ordinary-deleted.
// MOPP root and original child collection must agree on fraction and full keys
// after accounting for the native MOPP root's extra zero child-selector key.
// An optional complete authored triangle oracle can prove shared-edge or
// duplicate-face ties without changing the MOPP's returned key or hierarchy,
// with hit coverage for every triangle/convex part; failures never set proof.
size_t NativeJigsawProbeScratchSize(const ConvertedJigsawAggregate& source);
bool ProbeNativeJigsawAggregate(PPCContext& ctx,uint8_t* base,uint32_t scratch,
    size_t scratch_size,const ConvertedJigsawAggregate& source,
    NativeJigsawProof& proof,std::string& error,const AuthoredCollisionBody* authored_geometry=nullptr);
}

#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <span>
struct PPCContext;
namespace em2::em1 {
struct AuthoredStaticCollision;
struct JigsawAggregateInfo;
struct NativeCollisionProof {
  uint32_t loaded_shapes=0,hit_rays=0,miss_rays=0;
  float largest_fraction_error=0;
};
// Developer-only synchronous game-thread check. Uses the real native in-place
// pack loader and collection raycast, with independent triangle intersections.
// No world admission, native bodies, gameplay input, or original asset writes.
// Scratch is caller-owned, private, zeroed, aligned32 guest memory. It remains
// occupied by borrowed in-place objects until the call returns; no references
// escape and the caller may then release the allocation. Do not ordinary-delete
// these array-backed pack objects. Native finish constructors used here allocate
// no persistent memory (82459050/82459CC8); loader temporary arrays are released.
size_t NativeCollisionProbeScratchSize(const AuthoredStaticCollision& source);
bool ProbeNativeAuthoredCollision(PPCContext& ctx,uint8_t* base,uint32_t scratch,
    size_t scratch_size,const AuthoredStaticCollision& source,
    NativeCollisionProof& proof,std::string& error);
// Borrowed shapes from a complete, live native resource or cloned system.
// Uses the same independent authored triangle rays without loading new packs.
// Caller retains every shape owner until this synchronous check returns.
bool ProbeNativeLoadedCollisionShapes(PPCContext&,uint8_t*,uint32_t scratch,
    std::span<const uint32_t> shapes,const AuthoredStaticCollision&,
    NativeCollisionProof&,std::string& error);
// Original MOPP tree, not a generated collection/proxy. Borrowed root must stay
// owned by its native HKX resource throughout the synchronous query.
// Fixed rays compare full native collection hits/key paths; pure-triangle trees
// additionally use independent intersections. Not exhaustive/world proof.
bool ProbeNativeJigsawRays(PPCContext&,uint8_t*,uint32_t root,uint32_t scratch,
    const JigsawAggregateInfo&,NativeCollisionProof&,std::string& error);
}

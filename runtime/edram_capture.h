#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>
namespace rex::graphics::d3d12 {class D3D12RenderTargetCache;}
namespace em2 {
class Em2CommandProcessor;
struct PackedEdramOwner {
    uint32_t begin_tile,end_tile,key,depth_unorm_key,depth_float_key;
    bool pixels_are_64_bit,is_depth;
};
struct PackedEdramSnapshot {
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    std::vector<PackedEdramOwner> owners;
    uint32_t scale_x=0,scale_y=0;
    uint64_t completed_submission=0;
};
struct AuxiliaryDepthTarget {
    uint32_t key=0;
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
};
struct AuxiliaryDepthSnapshot {
    std::vector<AuxiliaryDepthTarget> targets;
    std::vector<PackedEdramOwner> owners;
    uint32_t scale_x=0,scale_y=0;
    uint64_t completed_submission=0;
};
// Same worker/quiescence contract as packed capture. Copies every referenced
// auxiliary host depth target (including historical owners) independently,
// with its original format, dimensions, MSAA and full depth/stencil planes.
// Output changes only after success. Textures finish in COPY_DEST. This is
// capture only; scaling/restoring auxiliary depth is a separate operation.
bool CaptureAuxiliaryDepth(Em2CommandProcessor&,rex::graphics::d3d12::D3D12RenderTargetCache&,AuxiliaryDepthSnapshot&);
// After packed staging restore, restore referenced D24FS8 auxiliary targets at
// the staging cache scale and attach historical references without changing
// primary owners. Same-scale copies are exact; scaling uses per-sample nearest
// depth/stencil rendering. Caller discards staging cache on failure and retains
// the old renderer. Same worker/quiescence contract; source returns COPY_DEST.
bool RestoreAuxiliaryDepth(Em2CommandProcessor&,const AuxiliaryDepthSnapshot&,
    rex::graphics::d3d12::D3D12RenderTargetCache& staging_cache);
// GPU-worker only. Cache must belong to cp; CPU producers must be stopped by
// the caller. Consumes
// published commands, dumps host targets using the native SDK, checks every
// expected dispatch, copies to independent storage and awaits completion.
// Output is replaced only on success; failure requires retaining the old cache.
// Captures packed native EDRAM, not auxiliary host float32-depth precision,
// resolved textures, pipeline/descriptor state or a complete renderer snapshot.
// Returned buffer is COPY_DEST; transition it before subsequent GPU reads.
bool CapturePackedEdram(Em2CommandProcessor&,rex::graphics::d3d12::D3D12RenderTargetCache&,PackedEdramSnapshot&);
// Restore packed bits into an empty staging host cache, at its own scale.
// Uses canonical 32/64-bit owners, not original host formats or auxiliary depth.
// Caller retains the old cache until the complete renderer transaction commits.
// Snapshot must be COPY_DEST; successful completion preserves its state/content.
// Discard the staging cache on failure; failed GPU completion requires retaining
// resources and device recovery. Same worker/producer-quiescence requirements
// as capture. Not a complete restore.
bool RestorePackedEdram(Em2CommandProcessor&,const PackedEdramSnapshot&,
    rex::graphics::d3d12::D3D12RenderTargetCache& staging_cache);
// After canonical packed restore and auxiliary restore, transfer owned spans
// into their original native formats/MSAA. Requires a staging cache, stopped
// producers/invalidation and the same GPU-worker/lifetime contract as restore.
// Checks canonical source ownership, referenced auxiliary targets, native draw
// submission and resulting original owner keys. No rollback: discard staging
// on failure, retaining it until GPU completion. Draw presence is not a complete
// audit of every native stencil pass; full renderer publication is separate.
bool RehydratePackedEdramOwners(Em2CommandProcessor&,const PackedEdramSnapshot&,
    rex::graphics::d3d12::D3D12RenderTargetCache& staging_cache);
// Compare every owned packed bit at a matching scale using GPU reduction.
// Coverage is physical native tile intervals; zero-key unowned spans are ignored.
// Inputs finish COPY_DEST. Failure never publishes or mutates their contents.
bool VerifyPackedEdramSnapshots(Em2CommandProcessor&,const PackedEdramSnapshot& expected,
    const PackedEdramSnapshot& actual,const std::vector<PackedEdramOwner>& coverage);
bool TestPackedEdramCapture(Em2CommandProcessor&);
bool TestAuxiliaryDepthCapture(Em2CommandProcessor&);
}

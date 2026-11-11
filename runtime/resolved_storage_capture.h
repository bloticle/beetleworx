#pragma once
#include "resolve_coverage.h"
#include <d3d12.h>
#include <wrl/client.h>
namespace rex::graphics::d3d12 {class D3D12TextureCache;class D3D12SharedMemory;}
namespace em2 {
class Em2CommandProcessor;
struct ResolvedSnapshotSpan {
    ResolvedStorageRange native;
    uint32_t buffer_index;
    uint64_t offset;
};
struct ResolvedStorageSnapshot {
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> buffers;
    std::vector<ResolvedSnapshotSpan> spans;
    uint32_t scale_x=0,scale_y=0;
    uint64_t completed_submission=0;
};
// GPU-worker only. Caller stops CPU command producers/invalidation and retains
// the source cache on failure. Cache/shared memory must belong to cp. Captures
// current-valid ledger ranges, with no newly committed source backing. Native
// alias windows/barriers are used for scaled storage; 1x uses shared memory.
// Independent destination buffers finish COPY_DEST. Output changes only after
// completion; failed completion retains possible in-flight destinations.
// This does not rescale/restore data or publish a renderer transaction.
bool CaptureResolvedStorage(Em2CommandProcessor&,rex::graphics::d3d12::D3D12TextureCache&,
    rex::graphics::d3d12::D3D12SharedMemory&,const ResolveRangeLedger&,ResolvedStorageSnapshot&);
// GPU-worker only. Source buffers must be COPY_DEST and are restored to that
// state on success. Produces independent COPY_DEST buffers at the chosen scale,
// using opaque per-texel nearest conversion (direct copies for equal scale).
// Caller retains source/output lifetimes through any other outstanding queues.
// Only publishes output on success; failed completion retains submitted objects.
// Does not restore native cache storage or publish a renderer transaction.
bool ConvertResolvedStorage(Em2CommandProcessor&,const ResolvedStorageSnapshot&,uint32_t scale_x,uint32_t scale_y,ResolvedStorageSnapshot&);
// Commit-phase restoration into a staging cache belonging to cp/shared.
// Caller stops CPU producers/invalidation and retains destination/shared memory
// on failure until a successful GPU drain. All backing is preflighted; source
// buffers remain COPY_DEST on success. Native page validity and shared watches
// publish only after copying completes. Ledger replaces only on success.
// At1x, RequestRange preserves resident shared-memory bytes outside exact spans.
// Untyped never-initialized holes are not reconstructed from a scaled snapshot.
// This changes global validity and, at1x, the existing shared GPU buffer; it is
// not rollback-safe. Complete other renderer preparation before this commit.
bool RestoreResolvedStorage(Em2CommandProcessor&,const ResolvedStorageSnapshot&,
    rex::graphics::d3d12::D3D12TextureCache&,ResolveRangeLedger&);
bool TestResolvedStorageCapture(Em2CommandProcessor&);
bool TestResolvedStorageConversion(Em2CommandProcessor&);
}

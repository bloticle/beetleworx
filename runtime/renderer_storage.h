#pragma once
#include "edram_capture.h"
#include "resolved_storage_capture.h"
namespace em2 {
class GraphicsResourceCheckpoint;
struct RendererStorageSnapshot {
    PackedEdramSnapshot packed;
    AuxiliaryDepthSnapshot auxiliary;
    ResolvedStorageSnapshot resolved;
};
// GPU worker only, outside packet execution. Pins a published command prefix
// and blocks native watched-memory invalidation during all three captures.
// Captures independent storage from the processor's current caches and owned
// tracking ledger; no settings change or renderer publication. All scales must
// agree. Output changes only on success. Retain original caches on GPU failure.
// This excludes shader/pipeline/descriptors and untracked shared-memory padding.
bool CaptureRendererStorage(Em2CommandProcessor&,RendererStorageSnapshot&);
// Use the caller's checkpoint when capture, restoration and publication must
// share one pinned command prefix. Token must belong to this processor.
bool CaptureRendererStorage(Em2CommandProcessor&,const GraphicsResourceCheckpoint&,RendererStorageSnapshot&);
}

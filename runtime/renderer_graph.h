#pragma once
#include <rex/graphics/d3d12/pipeline_cache.h>
#include <rex/graphics/d3d12/texture_cache.h>
#include <filesystem>
#include "hdr_scene_encoding.h"
#include <memory>
#include <optional>
#include <vector>
namespace em2 {
class Em2CommandProcessor;class GraphicsResourceCheckpoint;class ResolveRangeLedger;struct RendererStorageSnapshot;
struct PipelineShaderRecord {
    rex::graphics::xenos::ShaderType type;
    uint64_t hash;
    std::vector<uint32_t> code_big_endian;
    bool analyzed=false;
};
struct PipelineShaderSnapshot {
    std::vector<PipelineShaderRecord> shaders;
    std::optional<uint64_t> active_vertex,active_pixel;
    std::filesystem::path storage_root;
    uint32_t storage_title=0;
    // Captured immutable scene mode; may be explicitly changed when building
    // a coordinated replacement graph. Never retag an existing pipeline cache.
    bool hdr_scene=false,material_capture=false,material_surfaces=false,material_surface_linear=false;
    HdrSceneEncoding hdr_encoding=HdrSceneEncoding::kCompatibility;
};
struct RendererGraph {
    RendererGraph()=default;
    RendererGraph(RendererGraph&&) noexcept;
    RendererGraph& operator=(RendererGraph&&) noexcept;
    RendererGraph(const RendererGraph&)=delete;
    RendererGraph& operator=(const RendererGraph&)=delete;
    // Reverse destruction releases pipelines before their referenced targets.
    std::unique_ptr<rex::graphics::d3d12::D3D12RenderTargetCache> targets;
    std::unique_ptr<rex::graphics::d3d12::D3D12TextureCache> textures;
    std::unique_ptr<rex::graphics::d3d12::PipelineCache> pipelines;
    rex::graphics::d3d12::D3D12Shader *active_vertex=nullptr,*active_pixel=nullptr;
    std::filesystem::path storage_root;
    uint32_t storage_title=0;
};
// GPU-worker only. Source registry belongs to this processor/device; it must
// not be undergoing storage preload. Output is independent of source lifetime.
bool CapturePipelineShaders(Em2CommandProcessor&,PipelineShaderSnapshot&);
// Explicit cache snapshots require their owning targets to preserve scene mode.
bool CapturePipelineShaders(Em2CommandProcessor&,rex::graphics::d3d12::PipelineCache&,
    const rex::graphics::d3d12::D3D12RenderTargetCache&,rex::graphics::Shader* vertex,rex::graphics::Shader* pixel,PipelineShaderSnapshot&);
// Build an unpublished graph under the caller's checkpoint. Does not restore
// storage, bind EDRAM system descriptors, open persistence or publish pointers.
// Native translations/PSOs are regenerated for the target scale as requested.
// Output must not have outstanding GPU work; failure preserves it.
bool BuildRendererGraph(Em2CommandProcessor&,const GraphicsResourceCheckpoint&,
    const PipelineShaderSnapshot&,uint32_t sx,uint32_t sy,RendererGraph&);
// Restore a coherent storage capture into a fresh unpublished graph. Converts
// resolved snapshots first, restores packed/auxiliary/original target formats,
// then commits resolved data. Target1x writes persistent shared memory. Native
// system views roll back to the current graph on success. Discard stage on
// ordinary failure; retain all caches if GPU completion fails. Output ledger
// replaces only on success. This does not publish the graph or open storage.
bool RestoreRendererGraph(Em2CommandProcessor&,const GraphicsResourceCheckpoint&,
    const RendererStorageSnapshot&,RendererGraph&,ResolveRangeLedger&);
// Consuming form for the live controller. Releases captured packed/auxiliary/
// resolved allocations after their respective GPU reads finish. Ownership and
// scale metadata remain, but payloads may be consumed even on later failure;
// retry requires a fresh capture. Pending GPU work still requires retention.
bool RestoreRendererGraph(Em2CommandProcessor&,const GraphicsResourceCheckpoint&,
    RendererStorageSnapshot&&,RendererGraph&,ResolveRangeLedger&);
// GPU-worker publication under a held checkpoint. Stage storage must already
// be restored and its ledger must describe that storage. Retired must be empty.
// Success consumes stage, returns the old graph in retired and its old ledger
// in ledger, remaps active shaders, resets native binding caches and transfers
// the single native persistence writer. Does not resume guest packets. Failure
// on GPU loss requires retaining both graphs and recovery before rendering.
bool PublishRendererGraph(Em2CommandProcessor&,const GraphicsResourceCheckpoint&,
    RendererGraph& stage,ResolveRangeLedger& ledger,RendererGraph& retired);
enum class LiveRenderScaleStatus {Idle,Queued,Applied,Busy,MemoryLimit,CaptureFailed,RestoreFailed,GpuFailure};
void QueueRendererScaleChange(uint64_t revision=0);
LiveRenderScaleStatus RendererScaleStatus();
// Command worker only, outside packet execution. Menu scales1..3. Failure
// leaves current caches selected; device failure retains pending resources.
LiveRenderScaleStatus ApplyRendererScale(Em2CommandProcessor&,uint32_t scale,uint64_t revision=0);
bool TestRendererScaleRequestOrdering();
bool TestRendererGraph(Em2CommandProcessor&,const std::filesystem::path&);
}

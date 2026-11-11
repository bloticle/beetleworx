#pragma once
#include <memory>
#include "hdr_scene_encoding.h"
#include "scene_view.h"
#include <cstdint>
#include <d3d12.h>
namespace rex::graphics::d3d12 {class D3D12RenderTargetCache;}
namespace rex::graphics::d3d12 {class DeferredCommandList;}
namespace em2 {
// Private source-built native transfer PSO variant. SDK key fields occupy only
// the low bits; no class/header layout is changed.
inline constexpr uint32_t kHdrTransferKeyBit=0x80000000u;
class HdrSceneTarget;
struct HdrSceneBinding {bool active=false;D3D12_CPU_DESCRIPTOR_HANDLE rtv{};std::shared_ptr<HdrSceneTarget> target;HdrSceneEncoding encoding=HdrSceneEncoding::kCompatibility;HdrGuestTransfer guest_transfer=HdrGuestTransfer::kRaw;};
struct HdrSceneTransfer {std::shared_ptr<HdrSceneTarget> source,destination;uint32_t descriptor_index=0;explicit operator bool()const{return source && destination;}};
struct HdrSceneCacheState;
struct HdrSceneCacheStats {uint64_t builds=0,worker_builds=0,bindings=0,clears=0,encoding_changes=0,transfers=0,native_linear_builds=0;};
// Register before any pipeline creation; mode is immutable for this cache.
// After compilation starts, mode stays registered until native cache shutdown
// even if this stats handle is dropped. Live switches must replace the
// coordinated renderer cache graph after workers join and GPU work drains.
class HdrSceneCacheRegistration {
public:
    static std::unique_ptr<HdrSceneCacheRegistration> Create(const rex::graphics::d3d12::D3D12RenderTargetCache&,ID3D12Device*,uint64_t budget=256ull*1024*1024,HdrSceneEncoding encoding=HdrSceneEncoding::kCompatibility);
    ~HdrSceneCacheRegistration();
    HdrSceneCacheStats stats()const;
    // A prepared renderer graph may not have requested its first PSO yet.
    // Commit its immutable mode to the cache before the builder handle drops.
    bool KeepUntilCacheShutdown();
    HdrSceneCacheRegistration(const HdrSceneCacheRegistration&)=delete;
private:
    HdrSceneCacheRegistration()=default;
    std::shared_ptr<HdrSceneCacheState> state_;
    const rex::graphics::d3d12::D3D12RenderTargetCache* cache_=nullptr;
};
void ShutdownHdrSceneCache(const rex::graphics::d3d12::D3D12RenderTargetCache*);
bool CurrentHdrScenePipeline();
HdrSceneEncoding CurrentHdrSceneEncoding();
HdrSceneEncoding GetHdrSceneEncoding(const rex::graphics::d3d12::D3D12RenderTargetCache*);
// Refresh attachment bindings when emulated gamma changes without an sRGB view change.
bool UpdateHdrSceneBindingPolicy(const rex::graphics::d3d12::D3D12RenderTargetCache*,bool srgb,bool native_gamma);
void CountHdrScenePipelineBuild(bool native_linear=false);
// GPU-worker only. Budget includes both the HDR surface and the legacy source
// pinned by it. Failure uses a correctly typed null MRT, retaining legacy output.
HdrSceneBinding AcquireHdrSceneBinding(const rex::graphics::d3d12::D3D12RenderTargetCache*,ID3D12Resource*,rex::graphics::d3d12::DeferredCommandList* initialize=nullptr,bool srgb=false,bool native_gamma=false);
void MirrorHdrSceneClear(const rex::graphics::d3d12::D3D12RenderTargetCache*,ID3D12Resource*,const float (&raw)[4],const D3D12_RECT&,rex::graphics::d3d12::DeferredCommandList&);
HdrSceneTransfer PrepareHdrSceneTransfer(const rex::graphics::d3d12::D3D12RenderTargetCache*,ID3D12Resource* source,ID3D12Resource* destination,rex::graphics::d3d12::DeferredCommandList&);
void InvalidateHdrSceneDestination(const rex::graphics::d3d12::D3D12RenderTargetCache*,ID3D12Resource*);
void CountHdrSceneTransfer(const rex::graphics::d3d12::D3D12RenderTargetCache*);
bool HasHdrSceneCache(const rex::graphics::d3d12::D3D12RenderTargetCache*);
// Resolve consumers may only read initialized, authoritative sidecars.
HdrSceneBinding FindHdrSceneSource(const rex::graphics::d3d12::D3D12RenderTargetCache*,ID3D12Resource*,bool& srgb);
bool ReserveHdrSceneMemory(const rex::graphics::d3d12::D3D12RenderTargetCache*,uint64_t bytes);
void ReleaseHdrSceneMemory(const rex::graphics::d3d12::D3D12RenderTargetCache*,uint64_t bytes);
}

namespace em2 {
// GPU owner only. Tag after successful geometry/AO recording; any subsequent
// non-inert draw, clear, invalidation or transfer into the source drops the tag.
// Already resolved images retain their immutable association until overwritten.
bool BindHdrSceneView(const rex::graphics::d3d12::D3D12RenderTargetCache*,ID3D12Resource*,std::shared_ptr<const SceneViewIdentity>);
void InvalidateHdrSceneView(const rex::graphics::d3d12::D3D12RenderTargetCache*,ID3D12Resource*);
std::shared_ptr<const SceneViewIdentity> FindHdrSceneView(const rex::graphics::d3d12::D3D12RenderTargetCache*,ID3D12Resource*);
}

#pragma once
#include <rex/graphics/util/draw.h>
#include "hdr_scene_encoding.h"
#include "scene_view.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <span>
namespace rex::graphics {class SharedMemory;class TextureCache;}
namespace rex::graphics::d3d12 {class D3D12RenderTargetCache;class D3D12CommandProcessor;}
namespace em2 {
class SceneAmbientBindings;class HdrGuestViewBindings;
enum class SceneResolveLayer:uint32_t{kHdr,kMaterialAmbient,kMaterialSurface};
// Native owner dispatch, relative to ResolveInfo::GetCopyEdramTileSpan's base.
struct HdrResolveOwner {
    ID3D12Resource* legacy=nullptr;
    uint32_t base=0,pitch=0,samples_log2=0;
    uint32_t offset=0,width_tiles=0,height_tiles=0;
};
class HdrResolvedScene {
public:
    const std::shared_ptr<const SceneViewIdentity>& view()const{return view_;}
    HdrSceneEncoding encoding()const{return encoding_;}
    bool lighting_applied()const{return lighting_applied_;}
    bool valid()const{return valid_;} // Caller holds the shared-memory lock.
    uint64_t submission()const{return submission_;}
    uint64_t frame()const{return frame_;}
    uint64_t revision()const{return revision_;}
    SceneResolveLayer layer()const{return layer_;}
    ID3D12Resource* texture()const{return texture_.Get();}
    ID3D12Resource* guest_texture()const;
    uint32_t guest_srv_identity()const;
    HdrGuestTransfer guest_transfer()const{return guest_transfer_;}
    // The image covers exactly this resolve rectangle, at its draw scale.
    // It is NOT an authoritative image of untouched destination texture areas.
    const rex::graphics::draw_util::ResolveInfo& info()const{return info_;}
    bool linear_srgb_view()const{return srgb_;}
    uint32_t srv_identity()const{return srv_identity_;}
private:
    friend struct HdrResolveContext;
    friend bool RefreshHdrSceneGuestView(rex::graphics::d3d12::D3D12RenderTargetCache*,rex::graphics::d3d12::D3D12CommandProcessor&,const std::shared_ptr<HdrResolvedScene>&);
    friend bool PublishSceneAmbientResult(rex::graphics::d3d12::D3D12RenderTargetCache*,rex::graphics::d3d12::D3D12CommandProcessor&,const std::shared_ptr<const HdrResolvedScene>&,const std::shared_ptr<const SceneAmbientBindings>&);
    friend std::shared_ptr<HdrResolvedScene> RecordHdrSceneResolve(rex::graphics::d3d12::D3D12RenderTargetCache*,rex::graphics::d3d12::D3D12CommandProcessor&,rex::graphics::SharedMemory&,const rex::graphics::draw_util::ResolveInfo&,std::span<const HdrResolveOwner>);
    friend void CommitHdrSceneResolve(rex::graphics::d3d12::D3D12RenderTargetCache*,const std::shared_ptr<HdrResolvedScene>&,bool);
    friend std::shared_ptr<const HdrResolvedScene> FindHdrSceneResolve(rex::graphics::d3d12::D3D12RenderTargetCache*,const rex::graphics::draw_util::ResolveInfo&);
    friend std::shared_ptr<HdrResolvedScene> RecordSceneResolve(rex::graphics::d3d12::D3D12RenderTargetCache*,rex::graphics::d3d12::D3D12CommandProcessor&,rex::graphics::SharedMemory&,const rex::graphics::draw_util::ResolveInfo&,std::span<const HdrResolveOwner>,SceneResolveLayer);
    friend void CommitSceneResolve(rex::graphics::d3d12::D3D12RenderTargetCache*,const std::shared_ptr<HdrResolvedScene>&,bool,SceneResolveLayer);
    friend std::shared_ptr<const HdrResolvedScene> FindMaterialSceneResolve(rex::graphics::d3d12::D3D12RenderTargetCache*,const rex::graphics::draw_util::ResolveInfo&);
    friend std::shared_ptr<const HdrResolvedScene> FindMaterialSurfaceResolve(rex::graphics::d3d12::D3D12RenderTargetCache*,const rex::graphics::draw_util::ResolveInfo&);
    friend void ShutdownMaterialSceneResolves(const rex::graphics::d3d12::D3D12RenderTargetCache*);
    friend std::shared_ptr<const HdrResolvedScene> FindSceneTexture(const rex::graphics::TextureCache*,const struct HdrTextureQuery&,SceneResolveLayer);
    std::shared_ptr<const SceneViewIdentity> view_;
    HdrSceneEncoding encoding_=HdrSceneEncoding::kCompatibility;
    SceneResolveLayer layer_=SceneResolveLayer::kHdr;
    Microsoft::WRL::ComPtr<ID3D12Resource> texture_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> uav_;
    rex::graphics::draw_util::ResolveInfo info_{};
    std::shared_ptr<HdrGuestViewBindings> guest_view_;
    HdrGuestTransfer guest_transfer_=HdrGuestTransfer::kRaw;
    uint64_t guest_revision_=0;uint32_t guest_srv_identity_=0;bool guest_disabled_=false;
    bool srgb_=false,valid_=false,lighting_applied_=false;
    uint32_t srv_identity_=0;
    uint64_t submission_=0,frame_=0,revision_=0;
    friend std::shared_ptr<const HdrResolvedScene> FindHdrSceneTexture(const rex::graphics::TextureCache*,const struct HdrTextureQuery&);
};
// Native GPU worker; serialize Record, the native memory write, and Commit
// with the shared-memory global critical region. Changes compute bindings.
// Resources stay pinned until native cache shutdown after GPU work drains.
std::shared_ptr<HdrResolvedScene> RecordHdrSceneResolve(rex::graphics::d3d12::D3D12RenderTargetCache*,rex::graphics::d3d12::D3D12CommandProcessor&,rex::graphics::SharedMemory&,const rex::graphics::draw_util::ResolveInfo&,std::span<const HdrResolveOwner>);
void CommitHdrSceneResolve(rex::graphics::d3d12::D3D12RenderTargetCache*,const std::shared_ptr<HdrResolvedScene>&,bool copied);
// Exact resolve metadata lookup only; no base-only or bounding-range matches.
// Shared memory's global critical region must be held through binding use.
std::shared_ptr<const HdrResolvedScene> FindHdrSceneResolve(rex::graphics::d3d12::D3D12RenderTargetCache*,const rex::graphics::draw_util::ResolveInfo&);
// Ambient uses the same GPU resolve engine with separate images, budget and
// validity. It must never replace the HDR scene-colour texture binding.
std::shared_ptr<HdrResolvedScene> RecordMaterialSceneResolve(rex::graphics::d3d12::D3D12RenderTargetCache*,rex::graphics::d3d12::D3D12CommandProcessor&,rex::graphics::SharedMemory&,const rex::graphics::draw_util::ResolveInfo&,std::span<const HdrResolveOwner>);
void CommitMaterialSceneResolve(rex::graphics::d3d12::D3D12RenderTargetCache*,const std::shared_ptr<HdrResolvedScene>&,bool copied);
std::shared_ptr<const HdrResolvedScene> FindMaterialSceneResolve(rex::graphics::d3d12::D3D12RenderTargetCache*,const rex::graphics::draw_util::ResolveInfo&);
// Raw sampled diffuse RGB and effective specular exponent. RGB follows the
// resolve's channel permutation; colour exponent bias never scales attributes.
// Alpha < 0 means unavailable. Any unavailable MSAA sample invalidates a pixel.
std::shared_ptr<HdrResolvedScene> RecordMaterialSurfaceResolve(rex::graphics::d3d12::D3D12RenderTargetCache*,rex::graphics::d3d12::D3D12CommandProcessor&,rex::graphics::SharedMemory&,const rex::graphics::draw_util::ResolveInfo&,std::span<const HdrResolveOwner>);
void CommitMaterialSurfaceResolve(rex::graphics::d3d12::D3D12RenderTargetCache*,const std::shared_ptr<HdrResolvedScene>&,bool copied);
std::shared_ptr<const HdrResolvedScene> FindMaterialSurfaceResolve(rex::graphics::d3d12::D3D12RenderTargetCache*,const rex::graphics::draw_util::ResolveInfo&);
void ShutdownMaterialSceneResolves(const rex::graphics::d3d12::D3D12RenderTargetCache*);
void ShutdownHdrSceneResolves(const rex::graphics::d3d12::D3D12RenderTargetCache*);
// Canonical full, single-level tiled RGBA8 texture identity. Native binding
// rejects incompatible dimensions, signs, mip layout and encoding beforehand.
struct HdrTextureQuery {uint32_t base,width,height,pitch,endian,scale_x,scale_y;};
void AttachHdrSceneTextureCache(const rex::graphics::d3d12::D3D12RenderTargetCache*,const rex::graphics::TextureCache*);
std::shared_ptr<const HdrResolvedScene> FindHdrSceneTexture(const rex::graphics::TextureCache*,const HdrTextureQuery&);
std::shared_ptr<const HdrResolvedScene> FindMaterialSceneTexture(const rex::graphics::TextureCache*,const HdrTextureQuery&);
std::shared_ptr<const HdrResolvedScene> FindMaterialSurfaceTexture(const rex::graphics::TextureCache*,const HdrTextureQuery&);
// Uses the native fetch decoder, independent of stale active shader bindings.
// Caller holds the global memory lock through use. Non-identity presentation
// RGB swizzles and unsupported layouts fall back to the native SDR swap path.
std::shared_ptr<const HdrResolvedScene> FindHdrSwapScene(const rex::graphics::TextureCache*,const rex::graphics::xenos::xe_gpu_texture_fetch_t&);
bool HdrTextureKeyBatchCurrent(const rex::graphics::TextureCache*,const void* keys,size_t count);
void RememberHdrTextureKeyBatch(const rex::graphics::TextureCache*,const void* keys,size_t count);
void ForgetHdrTextureCache(const rex::graphics::TextureCache*);
bool HasHdrTextureStates();
}

namespace em2 {
// GPU worker under the global memory lock. Copies a current matched composition
// into its owning HDR image without changing the resource/SRV identity or guest
// RGBA8 memory. Caller retains composition/pass through the submission fence.
// Validates and reserves command storage before any source-image transition.
// One publication per native resolve revision; a new resolve resets the flag.
bool PublishSceneAmbientResult(rex::graphics::d3d12::D3D12RenderTargetCache*,rex::graphics::d3d12::D3D12CommandProcessor&,const std::shared_ptr<const HdrResolvedScene>&,const std::shared_ptr<const SceneAmbientBindings>&);
}

namespace em2 {
// After optional lighting publication and before guest sampling. Keeps the
// canonical image untouched. Encoding/allocation failure declines guest view;
// ordinary native guest texture sampling remains available.
bool RefreshHdrSceneGuestView(rex::graphics::d3d12::D3D12RenderTargetCache*,rex::graphics::d3d12::D3D12CommandProcessor&,const std::shared_ptr<HdrResolvedScene>&);
}

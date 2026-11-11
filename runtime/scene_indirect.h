#pragma once
#include "scene_occlusion.h"
#include "hdr_scene_resolve.h"
namespace em2 {
enum class SceneIndirectEncoding {kUnknown,kLinearDiffuse};
struct SceneIndirectSettings {float world_radius=2,pixel_radius=16,thickness=.03f,strength=.25f;};
struct SceneIndirectTargets;
class SceneIndirectBindings {
public:
    ID3D12Resource* result()const;
    bool current()const;
    bool can_use_with(const HdrResolvedScene&)const;
    const std::shared_ptr<const HdrResolvedScene>& surface()const{return surface_;}
    const std::shared_ptr<const SceneOcclusionBindings>& geometry_view()const{return geometry_view_;}
private:
    friend class SceneIndirectPass;
    std::shared_ptr<const HdrResolvedScene> colour_,surface_;
    std::shared_ptr<const SceneOcclusionBindings> geometry_view_;
    std::shared_ptr<SceneIndirectTargets> targets_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors_;
    uint64_t output_revision_=0,colour_revision_=0,surface_revision_=0,geometry_revision_=0;
    uint32_t width_=0,height_=0;
};
// Half-resolution screen-space single-bounce diffuse transport. Requires proven
// linear diffuse inputs, matching native resolve/view/grid/revision identities.
// Uses world-space surface orientation/area/distance and segment visibility;
// no screen-colour blur or final image modulation. Off-screen geometry and
// view-independent emitter radiance are unavailable. Output RGB is additive
// indirect radiance, alpha marks receiver validity. No scene publication here.
// GPU worker only; retain pass/bindings through the submission fence.
class SceneIndirectPass {
public:
    bool Initialize(ID3D12Device*);
    std::shared_ptr<SceneIndirectBindings> CreateBindings(std::shared_ptr<const HdrResolvedScene>,std::shared_ptr<const HdrResolvedScene>,std::shared_ptr<const SceneOcclusionBindings>,SceneIndirectEncoding,std::shared_ptr<const SceneIndirectBindings> previous={})const;
    bool Record(rex::graphics::d3d12::D3D12CommandProcessor&,SceneIndirectBindings&,const SceneIndirectSettings&)const;
private:
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
};
}

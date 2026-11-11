#pragma once
#include "scene_occlusion.h"
#include "hdr_scene_resolve.h"
namespace em2 {
class SceneIndirectBindings;
enum class SceneAmbientEncoding {kUnknown,kLinear};
struct SceneAmbientSettings {float strength=1,plane_distance=.1f,normal_cosine=.5f,indirect_strength=1,material_distance=.1f;};
struct SceneAmbientTargets;
class SceneAmbientBindings {
public:
    ID3D12Resource* result()const;
    bool current()const;
    bool can_publish_to(const HdrResolvedScene&)const;
private:
    friend class SceneAmbientPass;
    std::shared_ptr<const HdrResolvedScene> colour_,ambient_;
    std::shared_ptr<const SceneOcclusionBindings> occlusion_;
    std::shared_ptr<const SceneIndirectBindings> indirect_;
    std::shared_ptr<SceneAmbientTargets> targets_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors_;
    uint64_t output_revision_=0;
    uint64_t colour_revision_=0,ambient_revision_=0,occlusion_revision_=0;
    uint32_t width_=0,height_=0;
};
// Linear, same-exposure material residual composition: colour-ambient*(1-AO).
// Caller must establish that geometry and resolved colour cover the SAME pixel
// grid, and hold the native shared-memory lock through CreateBindings/Record.
// Equal dimensions/frame alone do not prove that association or linearity.
// Native GPU worker only. Retain this pass and bindings through their fence;
// consume reused output before another Record overwrites it. No presentation.
class SceneAmbientPass {
public:
    bool Initialize(ID3D12Device*);
    std::shared_ptr<SceneAmbientBindings> CreateBindings(std::shared_ptr<const HdrResolvedScene>,std::shared_ptr<const HdrResolvedScene>,std::shared_ptr<const SceneOcclusionBindings>,SceneAmbientEncoding,std::shared_ptr<const SceneAmbientBindings> previous={},std::shared_ptr<const SceneIndirectBindings> indirect={})const;
    bool Record(rex::graphics::d3d12::D3D12CommandProcessor&,SceneAmbientBindings&,const SceneAmbientSettings&)const;
private:
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
};
}

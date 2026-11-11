#pragma once
#include "scene_geometry.h"
#include "scene_view.h"
namespace em2 {
struct SceneOcclusionTargets;
struct SceneOcclusionSettings {
    float world_radius=1,pixel_radius=32,bias=.01f,strength=1;
};
class SceneOcclusionBindings {
public:
    const std::shared_ptr<const SceneViewIdentity>& view()const{return view_;}
    ID3D12Resource* result()const;
    bool current()const;
    uint64_t submission()const{return submission_;}
    uint64_t frame()const{return frame_;}
    uint64_t revision()const{return revision_;}
    const std::shared_ptr<const SceneGeometryBindings>& geometry()const{return geometry_;}
private:
    friend class SceneOcclusionPass;
    std::shared_ptr<const SceneViewIdentity> view_;
    std::shared_ptr<const SceneGeometryBindings> geometry_;
    std::shared_ptr<SceneOcclusionTargets> targets_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors_;
    uint32_t width_=0,height_=0;
    uint64_t submission_=0,frame_=0,revision_=0;
};
// Geometry horizon search, half resolution. Output RGB is a bent unit normal,
// alpha is ambient visibility. Invalid geometry gives (0,0,0,1). Does not
// multiply the game's already-lit colour or affect direct lighting.
// GPU worker only. Retain pass/bindings through submission fence. Outputs end
// in PSR|NPSR. Reused outputs must be consumed before the next Record.
class SceneOcclusionPass {
public:
    bool Initialize(ID3D12Device*);
    std::shared_ptr<SceneOcclusionBindings> CreateBindings(std::shared_ptr<const SceneGeometryBindings>,std::shared_ptr<const SceneOcclusionBindings> previous={},std::shared_ptr<const SceneViewIdentity> view={})const;
    bool Record(rex::graphics::d3d12::D3D12CommandProcessor&,SceneOcclusionBindings&,const SceneOcclusionSettings&)const;
private:
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
};
}

#pragma once
#include "scene_draw_inputs.h"
#include "scene_depth_view.h"
#include <wrl/client.h>
namespace rex::graphics::d3d12 {class D3D12CommandProcessor;}
namespace em2 {
struct SceneGeometryTargets;
struct SceneGeometrySettings {
    std::array<SceneDrawInputs::Row,4> inverse_view_projection;
    SceneDrawInputs::Row clip_from_uv{};
    SceneDrawInputs::Row camera_and_discontinuity{0,0,0,.25f};
};
class SceneGeometryBindings {
public:
    SceneGeometryBindings()=default;
    SceneGeometryBindings(const SceneGeometryBindings&)=delete;
    SceneGeometryBindings& operator=(const SceneGeometryBindings&)=delete;
    ID3D12Resource* positions()const;
    ID3D12Resource* normals()const;
    bool recorded()const;
private:
    friend class SceneGeometryPass;
    friend class SceneRasterGeometryPass;
    std::shared_ptr<const SceneDepthView> source_;
    std::shared_ptr<SceneGeometryTargets> targets_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors_;
    uint32_t width_=0,height_=0;
};
// Produces float32 camera-relative world positions (alpha=valid) and FP16 unit normals
// (alpha=valid). Caller supplies exact UV->guest clip mapping; no fullscreen
// mapping is inferred from an arbitrary game texture. Native GPU worker only.
// Output read states are PIXEL|NON_PIXEL_SHADER_RESOURCE after Record.
// Retain pass and bindings through their native GPU submission fence; Record
// changes compute root/pipeline state. Bindings track output transition state.
class SceneGeometryPass {
public:
    bool Initialize(ID3D12Device*);
    // Optional previous binding reuses equal-size outputs/transition tracking;
    // the returned binding owns its source independently. Retain old bindings
    // through their fences. Reused outputs are overwritten in queue order, so
    // consume each result before the next Record using those outputs.
    std::shared_ptr<SceneGeometryBindings> CreateBindings(std::shared_ptr<const SceneDepthView>,std::shared_ptr<const SceneGeometryBindings> previous={})const;
    bool Record(rex::graphics::d3d12::D3D12CommandProcessor&,SceneGeometryBindings&,const SceneGeometrySettings&)const;
private:
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
};
}

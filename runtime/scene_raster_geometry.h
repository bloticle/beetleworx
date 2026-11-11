#pragma once
#include "scene_draw_inputs.h"
#include <span>
#include <wrl/client.h>
namespace em2 {
struct RasterGeometryConstants {
    std::array<SceneDrawInputs::Row,4> relative_inverse;
    SceneDrawInputs::Row forward_and_discontinuity;
};
bool PrepareRasterGeometryConstants(const SceneDrawInputs&,RasterGeometryConstants&);
// Native VS/GS bytecode is retained by the caller only during Initialize.
// Own the resulting kernel through its GPU fence; do not reinitialize it while
// a prior use is in flight. Root parameters:
// 0 system CBV b0(all),1 vertex float b1,2 fetch b3,3 shared SRV table t0,
// 4 unused shared UAV table u0,5 bool/loop b2(all),6 vertex descriptor CBV b4,
// 7 depth SRV t0(space1),8 original depth sampler s0(space1),
// 9 RasterGeometryConstants at b0(space1). Native vertex fetches use shared
// memory as SRV; bind an unused real UAV buffer for the native VS declaration.
// RTV0 float32 camera-relative positions/validity, RTV1 FP16 normals/validity.
class SceneRasterGeometryKernel {
public:
    bool Initialize(ID3D12Device*,std::span<const uint8_t> vertex,std::span<const uint32_t> geometry={});
    ID3D12RootSignature* root()const{return root_.Get();}
    ID3D12PipelineState* pipeline()const{return pipeline_.Get();}
private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
};
}

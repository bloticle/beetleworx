#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
namespace em2 {
class AuxiliaryDepthResampler {
public:
    bool Initialize(ID3D12Device*,unsigned samples);
    // Source table contains D32_FLOAT_S8X24 depth and stencil SRVs in that order.
    // Caller transitions source to PIXEL_SHADER_RESOURCE and destination to
    // DEPTH_WRITE. Same sample count; preserve each sample independently.
    // Caller retains all objects through completion and invalidates bindings.
    bool Record(ID3D12GraphicsCommandList*,ID3D12Resource* destination,
        D3D12_CPU_DESCRIPTOR_HANDLE dsv,D3D12_GPU_DESCRIPTOR_HANDLE source,
        unsigned sx,unsigned sy,unsigned dx,unsigned dy) const;
private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>,9> pipelines_;
    unsigned samples_=0;
};
}

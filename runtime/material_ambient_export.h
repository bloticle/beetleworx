#pragma once
#include <cstdint>
#include <span>
#include <vector>
#include <memory>
#include <d3d12.h>
#include <wrl/client.h>
namespace em2 {
// Only for pinned material-lighting difference programs after native translation.
// Preserve original SV_Target0 (and optional HDR4), remove diagnostic targets
// 1/2, and export the pre-gamma lighting-dependent residual to private FP target5.
// RGB uses scene color0 exposure; alpha is coverage1. Caller supplies matching
// MRT5 state, clears, depth/coverage, lifetime and material identity checks.
std::vector<uint8_t> ExportMaterialAmbient(std::span<const uint8_t>);
// Pinned two-output surface probe: capture native colour1 register before its
// exposure/gamma into private MRT6, preserving primary/HDR4. No colour-space
// conversion or roughness inference; RGB is sampled diffuse, alpha exponent.
std::vector<uint8_t> ExportMaterialSurface(std::span<const uint8_t>);
// Combined baseline1/residual2/surface3 probe: private ambient5 and surface6.
std::vector<uint8_t> ExportMaterialLightingSurface(std::span<const uint8_t>);
// Pinned world-material diffuse fetch1: only RGB whose native texture signs are
// all Gamma is proven decoded to linear by the native translator. Other
// combinations write RGB0/alpha-1, without modifying primary/HDR/ambient output.
std::vector<uint8_t> ExportLinearMaterialSurface(std::span<const uint8_t>);
std::vector<uint8_t> ExportLinearMaterialLightingSurface(std::span<const uint8_t>);
// Preserve primary/HDR output and discard/coverage; private RGB is zero and
// private source alpha matches colour0 for supported destination blend factors.
std::vector<uint8_t> ExportZeroMaterialAmbient(std::span<const uint8_t>);
// Private RGB supplies the HDR4 source-colour blend factor; alpha comes from
// colour0. The auxiliary PSO carries the existing ambient through source- or
// destination-colour multiplication without inventing a new ambient term.
std::vector<uint8_t> ExportHdrAttenuationMaterialAmbient(std::span<const uint8_t>);
class MaterialAmbientClearPipeline {
public:
    MaterialAmbientClearPipeline(const MaterialAmbientClearPipeline&)=delete;
    static bool Supports(const D3D12_GRAPHICS_PIPELINE_STATE_DESC&);
    static std::unique_ptr<MaterialAmbientClearPipeline> Create(const D3D12_GRAPHICS_PIPELINE_STATE_DESC&);
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc()const{return desc_;}
private:
    MaterialAmbientClearPipeline()=default;
    std::vector<uint8_t> pixel_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc_{};
};
class MaterialAmbientPipeline {
public:
    static bool Supports(const D3D12_GRAPHICS_PIPELINE_STATE_DESC&,DXGI_FORMAT ambient=DXGI_FORMAT_R16G16B16A16_FLOAT);
    static std::unique_ptr<MaterialAmbientPipeline> Create(const D3D12_GRAPHICS_PIPELINE_STATE_DESC&,DXGI_FORMAT ambient=DXGI_FORMAT_R16G16B16A16_FLOAT);
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc()const{return desc_;}
    MaterialAmbientPipeline(const MaterialAmbientPipeline&)=delete;
private:
    MaterialAmbientPipeline()=default;
    // Own replacement PS/root; other stage/layout/SO pointers remain borrowed
    // only through native PSO creation, matching HdrScenePipeline's contract.
    std::vector<uint8_t> pixel_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc_{};
};
}

namespace em2 {
class MaterialLightingSurfacePipeline {
public:
    MaterialLightingSurfacePipeline(const MaterialLightingSurfacePipeline&)=delete;
    static std::unique_ptr<MaterialLightingSurfacePipeline> Create(const D3D12_GRAPHICS_PIPELINE_STATE_DESC&,bool linear_only=false);
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc()const{return desc_;}
private:
    MaterialLightingSurfacePipeline()=default;
    std::vector<uint8_t> pixel_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc_{};
};
}

namespace em2 {
// Preserve existing outputs/discard/coverage; mark only covered pixels absent.
std::vector<uint8_t> ExportUnavailableMaterialSurface(std::span<const uint8_t>);
class MaterialSurfaceClearPipeline {
public:
    MaterialSurfaceClearPipeline(const MaterialSurfaceClearPipeline&)=delete;
    static std::unique_ptr<MaterialSurfaceClearPipeline> Create(const D3D12_GRAPHICS_PIPELINE_STATE_DESC&,bool preserve=false);
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc()const{return desc_;}
private:
    MaterialSurfaceClearPipeline()=default;
    std::vector<uint8_t> pixel_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc_{};
};
}

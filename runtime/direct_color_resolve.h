#pragma once
#include <rex/graphics/util/draw.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
namespace rex::graphics::d3d12 {class D3D12RenderTargetCache;class D3D12CommandProcessor;}
namespace em2 {
struct DirectColorResolveSource {
 ID3D12Resource* resource=nullptr;
 D3D12_CPU_DESCRIPTOR_HANDLE srv{};
 D3D12_RESOURCE_DESC description{};
 uint32_t base=0,pitch=0,samples=0,format=0;
};
struct DirectColorResolveProgram {
 Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
 Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
};
bool DirectColorResolveEnabled();
bool DirectColorResolveSurveyEnabled();
struct DirectColorResolveRegion {uint32_t offset,width,rows;};
bool DirectColorResolveCoverage(std::span<const DirectColorResolveRegion>,uint32_t width,uint32_t rows,uint32_t pitch);
void ObserveDirectColorResolve(const rex::graphics::d3d12::D3D12RenderTargetCache*,uint64_t frame,
 const rex::graphics::draw_util::ResolveInfo&,bool fast32,bool eligible,const char* reason,
 uint32_t scale_x,uint32_t scale_y,uint32_t native_draws);
// Ownership coverage must additionally be proved by the caller using the real
// ownership map. This validates only supported representations and bounds.
bool ValidateDirectColorResolve(const rex::graphics::draw_util::ResolveInfo&,
 const DirectColorResolveSource&,uint32_t scale_x,uint32_t scale_y,
 uint32_t dump_base,uint32_t dump_width,uint32_t dump_rows,uint32_t dump_pitch);
std::shared_ptr<DirectColorResolveProgram> PrepareDirectColorResolve(
 const rex::graphics::d3d12::D3D12RenderTargetCache*,ID3D12Device*);
void ShutdownDirectColorResolve(const rex::graphics::d3d12::D3D12RenderTargetCache*);
void RecordDirectColorResolve();
uint64_t DirectColorResolveDispatches();
bool TestDirectColorResolve(rex::graphics::d3d12::D3D12CommandProcessor&,const std::filesystem::path&);
}

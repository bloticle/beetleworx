#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <cstddef>
#include <vector>
namespace rex::graphics::d3d12 {class D3D12CommandProcessor;}
namespace em2 {
constexpr uint32_t kStencilValueTransferKeyBit=uint32_t(1)<<30;
constexpr uint32_t kFusedDepthStencilTransferKeyBit=uint32_t(1)<<28;
struct StencilTransferScratch {
 Microsoft::WRL::ComPtr<ID3D12Resource> texture;
 Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors;
 D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
};
std::shared_ptr<StencilTransferScratch> AcquireStencilTransferScratch(const void* owner,rex::graphics::d3d12::D3D12CommandProcessor&,ID3D12Resource* destination);
// Same GPU-drained shutdown boundary as native cache PSOs and render targets.
void ShutdownStencilTransfers(const void* owner);
uint64_t StencilValueTransferMeshes();
void CountStencilValueTransfer();
uint64_t FusedDepthStencilTransferMeshes();
void CountFusedDepthStencilTransfer();
uint64_t StencilScratchClearOmissions();
void CountStencilScratchClearOmission();
uint64_t StencilPlaneBarrierTransfers();
void CountStencilPlaneBarrierTransfer();
size_t CoalesceStencilCopyRectangles(std::vector<D3D12_RECT>&);
bool TestStencilCopyCoalescing();
uint64_t StencilCopyCommands(),StencilCopyCommandsSaved();
void CountStencilCopyCommands(size_t issued,size_t saved);
}

#pragma once
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
namespace em2::pixel_test {
using Microsoft::WRL::ComPtr;
using Float4=std::array<float,4>;
struct Fixture {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12DescriptorHeap> views,samplers,rtvs;
    ComPtr<ID3D12Resource> upload,readback,target;
    std::array<ComPtr<ID3D12Resource>,9> textures;
    std::array<ComPtr<ID3D12PipelineState>,13> pipelines;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    ~Fixture(){if(event)CloseHandle(event);}
};
inline bool Buffer(ID3D12Device* device,uint64_t bytes,D3D12_HEAP_TYPE type,ID3D12Resource** output) {
    D3D12_HEAP_PROPERTIES heap{};heap.Type=type;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;
    desc.Height=desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
        type==D3D12_HEAP_TYPE_READBACK?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(output)));
}
inline void Barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};list->ResourceBarrier(1,&barrier);
}
}

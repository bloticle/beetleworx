#pragma once
#include "hdr_scene_encoding.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>
namespace rex::graphics::d3d12 {class D3D12CommandProcessor;}
namespace em2 {
class HdrGuestViewBindings {
public:
    ID3D12Resource* result()const{return output_.Get();}
    static std::shared_ptr<HdrGuestViewBindings> Create(ID3D12Resource*,DXGI_FORMAT output_format=DXGI_FORMAT_R16G16B16A16_FLOAT);
private:
    friend class HdrGuestViewPass;
    Microsoft::WRL::ComPtr<ID3D12Resource> source_,output_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors_;
    uint32_t width_=0,height_=0;
    bool written_=false;
};
// Encodes only the guest read view. Retain pass/bindings through GPU fence;
// source remains in PIXEL|NON_PIXEL read state and is never overwritten.
class HdrGuestViewPass {
public:
    bool Initialize(ID3D12Device*);
    // Raw is accepted only for RGBA8 output: clamp/quantize an already encoded
    // guest view before the native gamma table performs unsigned indexing.
    bool Record(rex::graphics::d3d12::D3D12CommandProcessor&,HdrGuestViewBindings&,HdrGuestTransfer,int32_t exponent_bias=0);
private:
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
};
}

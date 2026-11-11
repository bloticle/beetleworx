#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <span>
#include <vector>
#include <cstdint>
namespace rex::graphics::d3d12 {class DeferredCommandList;}
namespace em2 {
class HdrSceneTarget {
public:
    HdrSceneTarget(const HdrSceneTarget&)=delete;
    HdrSceneTarget& operator=(const HdrSceneTarget&)=delete;
    static std::unique_ptr<HdrSceneTarget> Create(ID3D12Resource* legacy_color);
    ID3D12Resource* source()const{return source_.Get();}
    ID3D12Resource* texture()const{return texture_.Get();}
    D3D12_CPU_DESCRIPTOR_HANDLE rtv()const{return rtv_->GetCPUDescriptorHandleForHeapStart();}
    D3D12_CPU_DESCRIPTOR_HANDLE srv()const{return srv_->GetCPUDescriptorHandleForHeapStart();}
    uint64_t allocation_bytes()const{return allocation_bytes_;}
    bool Matches(const D3D12_GRAPHICS_PIPELINE_STATE_DESC&)const;
    // Texture remains in RENDER_TARGET state. Caller owns other transitions and
    // retains this object through the final native submission fence. Values are
    // linear scene values supplied by the caller, not encoded legacy clear bits.
    bool Clear(ID3D12GraphicsCommandList*,const float (&linear)[4],std::span<const D3D12_RECT> rects = {})const;
    bool Clear(rex::graphics::d3d12::DeferredCommandList&,const float (&linear)[4],std::span<const D3D12_RECT> rects = {})const;
private:
    HdrSceneTarget()=default;
    Microsoft::WRL::ComPtr<ID3D12Resource> source_,texture_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_,srv_;
    uint64_t allocation_bytes_=0;
};
class HdrScenePipeline {
public:
    HdrScenePipeline(const HdrScenePipeline&)=delete;
    HdrScenePipeline& operator=(const HdrScenePipeline&)=delete;
    static std::unique_ptr<HdrScenePipeline> Create(const D3D12_GRAPHICS_PIPELINE_STATE_DESC&,const HdrSceneTarget&);
    static std::unique_ptr<HdrScenePipeline> Create(const D3D12_GRAPHICS_PIPELINE_STATE_DESC&);
    // Strict native exposure capture before native gamma. Reject unsupported
    // programs rather than silently substituting a post-gamma colour mirror.
    static std::unique_ptr<HdrScenePipeline> CreateNativeLinear(const D3D12_GRAPHICS_PIPELINE_STATE_DESC&);
    bool native_gamma_bypassed()const{return native_linear_;}
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc()const{return desc_;}
private:
    static std::unique_ptr<HdrScenePipeline> Build(const D3D12_GRAPHICS_PIPELINE_STATE_DESC&,bool native_linear);
    HdrScenePipeline()=default;
    bool native_linear_=false;
    std::vector<uint8_t> pixel_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    // Other pointer fields remain borrowed from the input descriptor. Keep its
    // shader/layout/SO data alive through PSO creation, as in the native cache.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc_{};
};
}

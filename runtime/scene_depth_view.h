#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <rex/graphics/xenos.h>
#include <memory>
namespace rex::graphics::d3d12 {class D3D12TextureCache;}
namespace em2 {
class HdrResolvedScene;
class Em2CommandProcessor;
class SceneDepthView {
public:
    ID3D12Resource* resource()const{return resource_.Get();}
    D3D12_CPU_DESCRIPTOR_HANDLE srv()const{return heap_->GetCPUDescriptorHandleForHeapStart();}
    const D3D12_SHADER_RESOURCE_VIEW_DESC& description()const{return description_;}
    bool hdr_source()const{return bool(hdr_owner_);}
private:
    friend std::shared_ptr<const SceneDepthView> AcquireSceneDepthView(rex::graphics::d3d12::D3D12TextureCache&,const rex::graphics::xenos::xe_gpu_texture_fetch_t&,std::shared_ptr<const SceneDepthView>);
    Microsoft::WRL::ComPtr<ID3D12Resource> resource_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    D3D12_SHADER_RESOURCE_VIEW_DESC description_{};
    std::shared_ptr<const HdrResolvedScene> hdr_owner_;
};
// GPU worker, immediately after native RequestTextures/draw. Never loads a
// texture, transitions a resource or changes native descriptors. Single-level,
// unsigned-red, nonstacked 2D only; other sampling semantics reject. Caller
// retains returned resource/view through its GPU fence. Does not dispatch.
std::shared_ptr<const SceneDepthView> AcquireSceneDepthView(rex::graphics::d3d12::D3D12TextureCache&,const rex::graphics::xenos::xe_gpu_texture_fetch_t&,std::shared_ptr<const SceneDepthView> previous={});
std::shared_ptr<const SceneDepthView> AcquireCurrentSceneDepthView(Em2CommandProcessor&,const rex::graphics::xenos::xe_gpu_texture_fetch_t&,std::shared_ptr<const SceneDepthView> previous={});
}

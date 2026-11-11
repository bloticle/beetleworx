#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <filesystem>
#include <memory>
#include <cstdint>

namespace rex::ui::d3d12 {class D3D12Provider;}
namespace rex::graphics::d3d12 {class D3D12CommandProcessor;}
namespace em2 {
inline constexpr DXGI_FORMAT kHdrOutputFormat=DXGI_FORMAT_R16G16B16A16_FLOAT;
inline constexpr DXGI_COLOR_SPACE_TYPE kHdrOutputColorSpace=DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
struct HdrOutputSettings {float exposure=1.f,paper_white_nits=200.f,peak_nits=1000.f,bloom_strength=1.f;};
enum class HdrUiEncoding:uint32_t {None,LinearStraight,LinearPremultiplied,SrgbStraight,SrgbPremultiplied};
struct HdrDisplayInfo {
    bool available=false,desktop_hdr_enabled=false;
    uint32_t bits_per_color=0;
    float minimum_nits=0,peak_nits=0,full_frame_nits=0;
};
enum class HdrColorSpaceResult {Configured,InvalidFormat,Unsupported,Failed};
// FP16 transport does not enable Windows HDR or prove physical HDR output.
HdrColorSpaceResult ConfigureHdrSwapchain(IDXGISwapChain3*,HdrDisplayInfo* display=nullptr);
class HdrOutputBindings {
public:
    ID3D12Resource* scene()const{return scene_.Get();}
    ID3D12Resource* output()const{return output_.Get();}
private:
    friend class HdrOutputPass;
    friend bool TestHdrOutputPass(rex::ui::d3d12::D3D12Provider&,const std::filesystem::path&);
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> views_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> views_cpu_;
    Microsoft::WRL::ComPtr<ID3D12Resource> scene_,output_,bloom_,ui_;
    uint32_t width_=0,height_=0,bloom_width_=0,bloom_height_=0;
    HdrUiEncoding ui_encoding_=HdrUiEncoding::None;
};
// Scene input is linear RGB radiance (1 = reference diffuse white), not a
// tone-mapped SDR image or a depth texture. Output is linear scRGB (1 = 80nits).
// An absolute-max RGB shoulder preserves signed RGB ratios, diffuse white and
// black, bounds the display peak and retains wide-gamut negative components.
class HdrOutputPass {
public:
    bool Initialize(ID3D12Device*);
    // Optional bloom is linear floating-point radiance, bilinearly upsampled
    // at texel centers and added before scene exposure/tone mapping. Supply
    // a scene without authored bloom already combined to avoid double bloom.
    // UI is full-size RGBA with explicit encoding; composite after tone mapping
    // in linear light at paper white, independent of scene exposure. Premultiplied
    // sRGB is unassociated before decoding. Inputs must remain read-only.
    std::unique_ptr<HdrOutputBindings> CreateBindings(ID3D12Resource* scene,ID3D12Resource* output,ID3D12Resource* bloom=nullptr,ID3D12Resource* ui=nullptr,HdrUiEncoding ui_encoding=HdrUiEncoding::SrgbStraight)const;
    // Caller supplies NON_PIXEL_SHADER_RESOURCE input and UNORDERED_ACCESS
    // output states, and owns transitions/fences. Retain pass and bindings until
    // the submission completes. This changes compute root/PSO/descriptor state;
    // a native renderer must invalidate its corresponding binding caches.
    // Invalid settings/bindings reject before any command is recorded.
    bool Record(ID3D12GraphicsCommandList*,const HdrOutputBindings&,HdrOutputSettings)const;
    // Uses native one-use descriptors and deferred commands; no private heap
    // switch that would leave the renderer's descriptor cache out of sync.
    bool Record(rex::graphics::d3d12::D3D12CommandProcessor&,const HdrOutputBindings&,HdrOutputSettings)const;
private:
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
};
bool TestHdrOutputPass(rex::ui::d3d12::D3D12Provider&,const std::filesystem::path&);
bool TestHdrCompositionPass(rex::ui::d3d12::D3D12Provider&,const std::filesystem::path&);
}

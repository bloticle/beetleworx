#include "hdr_scene_target.h"
#include "hdr_shader_export.h"
#include <rex/graphics/d3d12/deferred_command_list.h>
#include <cmath>
#include <immintrin.h>
namespace {
struct FP{unsigned value=_mm_getcsr();~FP(){_mm_setcsr(value);}};
bool Color(DXGI_FORMAT f){return f==DXGI_FORMAT_R8G8B8A8_TYPELESS || f==DXGI_FORMAT_R8G8B8A8_UNORM || f==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;}
bool ClearValid(const float (&v)[4],std::span<const D3D12_RECT> rects){
    for(float c:v)if(!std::isfinite(c) || std::abs(c)>65504)return false;
    if(rects.size()>UINT_MAX)return false;
    for(const auto& r:rects)if(r.left>r.right || r.top>r.bottom)return false;
    return true;
}
}
std::unique_ptr<em2::HdrSceneTarget> em2::HdrSceneTarget::Create(ID3D12Resource* source){
    FP fp;if(!source)return {};auto d=source->GetDesc();
    if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || !d.Width || !d.Height || d.Width>16384 || d.Height>16384 ||
       d.DepthOrArraySize!=1 || d.MipLevels!=1 || !Color(d.Format) || !(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
       (d.SampleDesc.Count!=1 && d.SampleDesc.Count!=2 && d.SampleDesc.Count!=4 && d.SampleDesc.Count!=8) || d.SampleDesc.Quality)return {};
    Microsoft::WRL::ComPtr<ID3D12Device> device;if(FAILED(source->GetDevice(IID_PPV_ARGS(&device))))return {};
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS samples{DXGI_FORMAT_R16G16B16A16_FLOAT,d.SampleDesc.Count,D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE,0};
    if(FAILED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,&samples,sizeof(samples))) || !samples.NumQualityLevels)return {};
    d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;d.Alignment=0;
    const auto allocation=device->GetResourceAllocationInfo(0,1,&d);if(!allocation.SizeInBytes || allocation.SizeInBytes==UINT64_MAX)return {};
    auto target=std::unique_ptr<HdrSceneTarget>(new HdrSceneTarget);
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    const D3D12_CLEAR_VALUE clear{DXGI_FORMAT_R16G16B16A16_FLOAT,{0,0,0,0}};
    if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_RENDER_TARGET,&clear,IID_PPV_ARGS(&target->texture_))))return {};
    const D3D12_DESCRIPTOR_HEAP_DESC views{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    if(FAILED(device->CreateDescriptorHeap(&views,IID_PPV_ARGS(&target->rtv_))))return {};
    D3D12_RENDER_TARGET_VIEW_DESC view{};view.Format=d.Format;view.ViewDimension=d.SampleDesc.Count==1?D3D12_RTV_DIMENSION_TEXTURE2D:D3D12_RTV_DIMENSION_TEXTURE2DMS;
    device->CreateRenderTargetView(target->texture_.Get(),&view,target->rtv());
    const D3D12_DESCRIPTOR_HEAP_DESC srv_heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    if(FAILED(device->CreateDescriptorHeap(&srv_heap,IID_PPV_ARGS(&target->srv_))))return {};
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=d.Format;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension=d.SampleDesc.Count==1?D3D12_SRV_DIMENSION_TEXTURE2D:D3D12_SRV_DIMENSION_TEXTURE2DMS;if(d.SampleDesc.Count==1)srv.Texture2D.MipLevels=1;
    device->CreateShaderResourceView(target->texture_.Get(),&srv,target->srv());target->source_=source;target->allocation_bytes_=allocation.SizeInBytes;return target;
}
bool em2::HdrSceneTarget::Matches(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& p)const{
    if(!source_ || !texture_ || !p.NumRenderTargets || p.NumRenderTargets>4 ||
       (p.RTVFormats[0]!=DXGI_FORMAT_R8G8B8A8_UNORM && p.RTVFormats[0]!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))return false;
    const auto d=source_->GetDesc();return (d.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS || d.Format==p.RTVFormats[0]) &&
        d.SampleDesc.Count==p.SampleDesc.Count && d.SampleDesc.Quality==p.SampleDesc.Quality;
}
bool em2::HdrSceneTarget::Clear(ID3D12GraphicsCommandList* list,const float (&v)[4],std::span<const D3D12_RECT> rects)const{
    FP fp;if(!list || !texture_ || list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT || !ClearValid(v,rects))return false;
    Microsoft::WRL::ComPtr<ID3D12Device> a,b;if(FAILED(list->GetDevice(IID_PPV_ARGS(&a))) || FAILED(texture_->GetDevice(IID_PPV_ARGS(&b))) || a.Get()!=b.Get())return false;
    list->ClearRenderTargetView(rtv(),v,UINT(rects.size()),rects.data());return true;
}
bool em2::HdrSceneTarget::Clear(rex::graphics::d3d12::DeferredCommandList& list,const float (&v)[4],std::span<const D3D12_RECT> rects)const{
    FP fp;if(!texture_ || !ClearValid(v,rects))return false;list.D3DClearRenderTargetView(rtv(),v,UINT(rects.size()),rects.data());return true;
}
std::unique_ptr<em2::HdrScenePipeline> em2::HdrScenePipeline::Create(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& p,const HdrSceneTarget& target){
    if(!target.Matches(p))return {};return Create(p);
}
std::unique_ptr<em2::HdrScenePipeline> em2::HdrScenePipeline::Create(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& p){return Build(p,false);}
std::unique_ptr<em2::HdrScenePipeline> em2::HdrScenePipeline::CreateNativeLinear(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& p){return Build(p,true);}
std::unique_ptr<em2::HdrScenePipeline> em2::HdrScenePipeline::Build(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& p,bool native_linear){
    FP fp;if(!p.NumRenderTargets || p.NumRenderTargets>4 ||
       (p.RTVFormats[0]!=DXGI_FORMAT_R8G8B8A8_UNORM && p.RTVFormats[0]!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && !(native_linear && p.RTVFormats[0]==DXGI_FORMAT_R32G32B32A32_FLOAT)) ||
       !p.PS.pShaderBytecode || !p.PS.BytecodeLength || !p.pRootSignature ||
       !p.BlendState.RenderTarget[0].RenderTargetWriteMask || p.BlendState.RenderTarget[0].LogicOpEnable)return {};
    auto result=std::unique_ptr<HdrScenePipeline>(new HdrScenePipeline);result->native_linear_=native_linear;result->pixel_=native_linear?AddLinearHdrSceneExport({static_cast<const uint8_t*>(p.PS.pShaderBytecode),p.PS.BytecodeLength}):AddHdrSceneExport({static_cast<const uint8_t*>(p.PS.pShaderBytecode),p.PS.BytecodeLength});if(result->pixel_.empty())return {};
    result->root_=p.pRootSignature;result->desc_=p;auto& d=result->desc_;d.PS={result->pixel_.data(),result->pixel_.size()};
    d.CachedPSO={};d.NumRenderTargets=5;d.RTVFormats[4]=DXGI_FORMAT_R16G16B16A16_FLOAT;
    if(!p.BlendState.IndependentBlendEnable)for(unsigned i=1;i<4;++i)d.BlendState.RenderTarget[i]=p.BlendState.RenderTarget[0];
    d.BlendState.RenderTarget[4]=p.BlendState.RenderTarget[0];d.BlendState.IndependentBlendEnable=TRUE;
    for(unsigned i=1;i<4;++i)if(i>=p.NumRenderTargets || p.RTVFormats[i]==DXGI_FORMAT_UNKNOWN){
        d.RTVFormats[i]=DXGI_FORMAT_UNKNOWN;auto& b=d.BlendState.RenderTarget[i];b.BlendEnable=b.LogicOpEnable=FALSE;b.RenderTargetWriteMask=0;
    }
    return result;
}

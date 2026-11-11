#include "built_shaders.h"
#include "hdr_output.h"
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <cmath>
#include <immintrin.h>
#include <array>

using Microsoft::WRL::ComPtr;
namespace {
struct Fp {unsigned value=_mm_getcsr();~Fp(){_mm_setcsr(value);}};
bool SettingsValid(em2::HdrOutputSettings s) {
    return std::isfinite(s.exposure) && std::isfinite(s.paper_white_nits) && std::isfinite(s.peak_nits) && std::isfinite(s.bloom_strength) && s.bloom_strength>=0 && s.bloom_strength<=16 &&
        s.exposure>=1.f/256 && s.exposure<=256 && s.paper_white_nits>0 && s.paper_white_nits<=1000 &&
        s.peak_nits>=s.paper_white_nits && s.peak_nits<=10000;
}
bool Image(const D3D12_RESOURCE_DESC& d) {
    return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width && d.Width<=16384 && d.Height && d.Height<=16384 &&
        d.DepthOrArraySize==1 && d.MipLevels==1 && d.SampleDesc.Count==1;
}
}
em2::HdrColorSpaceResult em2::ConfigureHdrSwapchain(IDXGISwapChain3* swapchain,HdrDisplayInfo* display) {
    Fp fp;if(display)*display={};
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if(!swapchain || FAILED(swapchain->GetDesc1(&desc)) || desc.Format!=kHdrOutputFormat)return HdrColorSpaceResult::InvalidFormat;
    UINT support=0;
    if(FAILED(swapchain->CheckColorSpaceSupport(kHdrOutputColorSpace,&support)) ||
       !(support&DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))return HdrColorSpaceResult::Unsupported;
    if(FAILED(swapchain->SetColorSpace1(kHdrOutputColorSpace)))return HdrColorSpaceResult::Failed;
    if(display) {
        ComPtr<IDXGIOutput> output;ComPtr<IDXGIOutput6> advanced;DXGI_OUTPUT_DESC1 info{};
        if(SUCCEEDED(swapchain->GetContainingOutput(&output)) && SUCCEEDED(output.As(&advanced)) && SUCCEEDED(advanced->GetDesc1(&info))) {
            auto luminance=[](float value){return std::isfinite(value) && value>=0?value:0.f;};
            display->available=true;display->bits_per_color=info.BitsPerColor;
            display->desktop_hdr_enabled=info.ColorSpace==DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                info.ColorSpace==DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020;
            display->minimum_nits=luminance(info.MinLuminance);display->peak_nits=luminance(info.MaxLuminance);
            display->full_frame_nits=luminance(info.MaxFullFrameLuminance);
        }
    }
    return HdrColorSpaceResult::Configured;
}
bool em2::HdrOutputPass::Initialize(ID3D12Device* device) {
    Fp fp;
    if(!device)return false;
    if(pipeline_)return device_.Get()==device;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{kHdrOutputFormat};
    if(FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support))) ||
       !(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))return false;
    
    ComPtr<ID3DBlob> error,serialized;
    const auto* shader=em2::GetBuiltShader(em2::BuiltShader::HdrOutput);if(!shader)return false;
    // Native bindless one-use allocations may be scattered, so each view has
    // its own table instead of assuming four contiguous native descriptors.
    const D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,1,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,2,0,0}};
    D3D12_ROOT_PARAMETER parameters[5]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants={0,0,12};
    for(unsigned i=0;i<4;++i){parameters[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[i+1].DescriptorTable={1,ranges+i};}
    const D3D12_ROOT_SIGNATURE_DESC desc{5,parameters,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pipeline;
    if(FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error)) ||
       FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root))))return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=root.Get();pso.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
    if(FAILED(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&pipeline))))return false;
    device_=device;root_=std::move(root);pipeline_=std::move(pipeline);return true;
}
std::unique_ptr<em2::HdrOutputBindings> em2::HdrOutputPass::CreateBindings(ID3D12Resource* scene,ID3D12Resource* output,ID3D12Resource* bloom,ID3D12Resource* ui,HdrUiEncoding ui_encoding)const {
    Fp fp;
    if(!pipeline_ || !scene || !output || scene==output)return {};
    const auto input=scene->GetDesc(),target=output->GetDesc();
    if(!Image(input) || !Image(target) || input.Width!=target.Width || input.Height!=target.Height ||
       (input.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT && input.Format!=DXGI_FORMAT_R32G32B32A32_FLOAT) ||
       (input.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) || target.Format!=kHdrOutputFormat ||
       !(target.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))return {};
    ComPtr<ID3D12Device> a,b;
    if(FAILED(scene->GetDevice(IID_PPV_ARGS(&a))) || FAILED(output->GetDevice(IID_PPV_ARGS(&b))) ||
       a.Get()!=device_.Get() || b.Get()!=device_.Get())return {};
    if((bloom && bloom==output) || (ui && ui==output) || uint32_t(ui_encoding)>4 || (ui && ui_encoding==HdrUiEncoding::None))return {};
    if(bloom && bloom->GetDesc().Format!=DXGI_FORMAT_R16G16B16A16_FLOAT && bloom->GetDesc().Format!=DXGI_FORMAT_R32G32B32A32_FLOAT)return {};
    for(auto* resource:{bloom,ui})if(resource){const auto desc=resource->GetDesc();ComPtr<ID3D12Device> owner;
        const bool floating=desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT || desc.Format==DXGI_FORMAT_R32G32B32A32_FLOAT;
        if(!Image(desc) || (desc.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) || (!floating && !(resource==ui && desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM)) ||
           (resource==ui && (desc.Width!=input.Width || desc.Height!=input.Height)) || FAILED(resource->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get()!=device_.Get())return {};
    }
    auto result=std::make_unique<HdrOutputBindings>();
    const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,4,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    if(FAILED(device_->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&result->views_))))return {};
    auto cpu_heap=heap;cpu_heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if(FAILED(device_->CreateDescriptorHeap(&cpu_heap,IID_PPV_ARGS(&result->views_cpu_))))return {};
    auto cpu=result->views_cpu_->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=input.Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
    device_->CreateShaderResourceView(scene,&srv,cpu);cpu.ptr+=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=target.Format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    device_->CreateUnorderedAccessView(output,nullptr,&uav,cpu);
    const auto step=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for(auto* resource:{bloom,ui}){cpu.ptr+=step;srv.Format=resource?resource->GetDesc().Format:DXGI_FORMAT_R16G16B16A16_FLOAT;device_->CreateShaderResourceView(resource,&srv,cpu);}
    result->bloom_=bloom;result->ui_=ui;result->ui_encoding_=ui?ui_encoding:HdrUiEncoding::None;
    if(bloom){result->bloom_width_=uint32_t(bloom->GetDesc().Width);result->bloom_height_=bloom->GetDesc().Height;}

    result->device_=device_;result->scene_=scene;result->output_=output;
    device_->CopyDescriptorsSimple(4,result->views_->GetCPUDescriptorHandleForHeapStart(),result->views_cpu_->GetCPUDescriptorHandleForHeapStart(),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    result->width_=uint32_t(input.Width);result->height_=input.Height;return result;
}
bool em2::HdrOutputPass::Record(ID3D12GraphicsCommandList* list,const HdrOutputBindings& bindings,HdrOutputSettings settings)const {
    Fp fp;
    if(!list || !pipeline_ || !bindings.views_ || bindings.device_.Get()!=device_.Get() || !SettingsValid(settings))return false;
    if(list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT && list->GetType()!=D3D12_COMMAND_LIST_TYPE_COMPUTE)return false;
    struct Constants {uint32_t width,height;float exposure,white,peak,bloom_strength;uint32_t ui_encoding,flags,bloom_width,bloom_height,padding[2];};
    const Constants constants{bindings.width_,bindings.height_,settings.exposure,settings.paper_white_nits,settings.peak_nits,settings.bloom_strength,uint32_t(bindings.ui_encoding_),bindings.bloom_?1u:0u,bindings.bloom_width_,bindings.bloom_height_,{}};
    static_assert(sizeof(constants)==48);
    ID3D12DescriptorHeap* heaps[]={bindings.views_.Get()};list->SetDescriptorHeaps(1,heaps);
    list->SetComputeRootSignature(root_.Get());list->SetPipelineState(pipeline_.Get());
    list->SetComputeRoot32BitConstants(0,12,&constants,0);
    auto view=bindings.views_->GetGPUDescriptorHandleForHeapStart();for(unsigned i=0;i<4;++i){list->SetComputeRootDescriptorTable(i+1,view);view.ptr+=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);}
    list->Dispatch((bindings.width_+7)/8,(bindings.height_+7)/8,1);return true;
}
bool em2::HdrOutputPass::Record(rex::graphics::d3d12::D3D12CommandProcessor& cp,const HdrOutputBindings& bindings,HdrOutputSettings settings)const {
    Fp fp;
    if(!pipeline_ || !bindings.views_cpu_ || bindings.device_.Get()!=device_.Get() || cp.GetD3D12Provider().GetDevice()!=device_.Get() || !SettingsValid(settings))return false;
    rex::ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[4];
    if(!cp.RequestOneUseSingleViewDescriptors(4,descriptors))return false;
    auto cpu=bindings.views_cpu_->GetCPUDescriptorHandleForHeapStart();for(unsigned i=0;i<4;++i){device_->CopyDescriptorsSimple(1,descriptors[i].first,cpu,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);cpu.ptr+=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);}
    struct Constants {uint32_t width,height;float exposure,white,peak,bloom_strength;uint32_t ui_encoding,flags,bloom_width,bloom_height,padding[2];};
    const Constants constants{bindings.width_,bindings.height_,settings.exposure,settings.paper_white_nits,settings.peak_nits,settings.bloom_strength,uint32_t(bindings.ui_encoding_),bindings.bloom_?1u:0u,bindings.bloom_width_,bindings.bloom_height_,{}};
    static_assert(sizeof(constants)==48);
    auto& list=cp.GetDeferredCommandList();list.D3DSetComputeRootSignature(root_.Get());cp.SetExternalPipeline(pipeline_.Get());
    list.D3DSetComputeRoot32BitConstants(0,12,&constants,0);for(unsigned i=0;i<4;++i)list.D3DSetComputeRootDescriptorTable(i+1,descriptors[i].second);
    cp.SubmitBarriers();list.D3DDispatch((bindings.width_+7)/8,(bindings.height_+7)/8,1);return true;
}

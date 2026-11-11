#include "built_shaders.h"
#include "scene_occlusion.h"
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <cmath>
#include <immintrin.h>
namespace {
struct Fp{unsigned csr=_mm_getcsr();~Fp(){_mm_setcsr(csr);}};
constexpr auto read=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
}
struct em2::SceneOcclusionTargets {Microsoft::WRL::ComPtr<ID3D12Resource> result;bool written=false;uint64_t revision=0;};
ID3D12Resource* em2::SceneOcclusionBindings::result()const{return targets_?targets_->result.Get():nullptr;}
bool em2::SceneOcclusionBindings::current()const{return targets_ && revision_ && revision_==targets_->revision;}
bool em2::SceneOcclusionPass::Initialize(ID3D12Device* device){
    Fp fp;if(!device)return false;if(pipeline_)return device_.Get()==device;
    
    Microsoft::WRL::ComPtr<ID3DBlob> errors,serialized;
    const auto* shader=em2::GetBuiltShader(em2::BuiltShader::SceneOcclusion);if(!shader)return false;
    D3D12_ROOT_PARAMETER parameters[4]{};
    const D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,1,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};
    for(unsigned i=0;i<3;++i){parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[i].DescriptorTable={1,&ranges[i]};}
    parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[3].Constants={0,0,8};
    const D3D12_ROOT_SIGNATURE_DESC root{4,parameters};Microsoft::WRL::ComPtr<ID3D12RootSignature> signature;
    if(FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors)) || FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&signature))))return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=signature.Get();desc.CS={shader->GetBufferPointer(),shader->GetBufferSize()};Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if(FAILED(device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&pipeline))))return false;
    device_=device;root_=std::move(signature);pipeline_=std::move(pipeline);return true;
}
std::shared_ptr<em2::SceneOcclusionBindings> em2::SceneOcclusionPass::CreateBindings(std::shared_ptr<const SceneGeometryBindings> geometry,std::shared_ptr<const SceneOcclusionBindings> previous,std::shared_ptr<const SceneViewIdentity> view)const{
    if(!pipeline_ || !geometry || !geometry->positions() || !geometry->normals())return {};
    const auto position=geometry->positions()->GetDesc(),normal=geometry->normals()->GetDesc();
    if(position.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || !position.Width || position.Width>16384 || !position.Height || position.Height>16384 || position.Width!=normal.Width || position.Height!=normal.Height || position.Format!=DXGI_FORMAT_R32G32B32A32_FLOAT || normal.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT)return {};
    for(auto* resource:{geometry->positions(),geometry->normals()}){Microsoft::WRL::ComPtr<ID3D12Device> owner;if(FAILED(resource->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get()!=device_.Get())return {};}
    if(view && (view->width!=position.Width || view->height!=position.Height))return {};
    auto binding=std::make_shared<SceneOcclusionBindings>();binding->view_=std::move(view);binding->geometry_=std::move(geometry);binding->width_=uint32_t(position.Width);binding->height_=position.Height;
    D3D12_RESOURCE_DESC output{};output.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;output.Width=(position.Width+1)/2;output.Height=(position.Height+1)/2;output.DepthOrArraySize=output.MipLevels=output.SampleDesc.Count=1;output.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;output.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto bytes=device_->GetResourceAllocationInfo(0,1,&output).SizeInBytes;if(bytes>64ull*1024*1024)return {};
    if(previous && previous->targets_ && previous->width_==binding->width_ && previous->height_==binding->height_){Microsoft::WRL::ComPtr<ID3D12Device> owner;if(SUCCEEDED(previous->result()->GetDevice(IID_PPV_ARGS(&owner))) && owner.Get()==device_.Get())binding->targets_=previous->targets_;}
    if(!binding->targets_){binding->targets_=std::make_shared<SceneOcclusionTargets>();D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        if(FAILED(device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&binding->targets_->result))))return {};}
    const D3D12_DESCRIPTOR_HEAP_DESC views{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,3,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};if(FAILED(device_->CreateDescriptorHeap(&views,IID_PPV_ARGS(&binding->descriptors_))))return {};
    auto handle=binding->descriptors_->GetCPUDescriptorHandleForHeapStart();const auto step=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for(unsigned i=0;i<2;++i){D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=i?normal.Format:position.Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;device_->CreateShaderResourceView(i?binding->geometry_->normals():binding->geometry_->positions(),&srv,handle);handle.ptr+=step;}
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=output.Format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device_->CreateUnorderedAccessView(binding->result(),nullptr,&uav,handle);return binding;
}
bool em2::SceneOcclusionPass::Record(rex::graphics::d3d12::D3D12CommandProcessor& cp,SceneOcclusionBindings& binding,const SceneOcclusionSettings& settings)const{
    Fp fp;if(!pipeline_ || !binding.geometry_ || !binding.targets_ || !binding.geometry_->recorded() || cp.GetD3D12Provider().GetDevice()!=device_.Get())return false;
    if(binding.view_ && (binding.view_->frame!=cp.GetCurrentFrame() || binding.view_->submission!=cp.GetCurrentSubmission()))return false;
    for(float value:{settings.world_radius,settings.pixel_radius,settings.bias,settings.strength})if(!std::isfinite(value))return false;
    if(settings.world_radius<1e-5f || settings.world_radius>1e6f || settings.pixel_radius<1 || settings.pixel_radius>128 || settings.bias<0 || settings.bias>=settings.world_radius || settings.strength<0 || settings.strength>2)return false;
    std::array<rex::ui::d3d12::util::DescriptorCpuGpuHandlePair,3> descriptors;if(!cp.RequestOneUseSingleViewDescriptors(3,descriptors.data()))return false;
    auto source=binding.descriptors_->GetCPUDescriptorHandleForHeapStart();const auto step=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for(unsigned i=0;i<3;++i){device_->CopyDescriptorsSimple(1,descriptors[i].first,source,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);source.ptr+=step;}
    if(binding.targets_->written)cp.PushTransitionBarrier(binding.result(),read,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);cp.SubmitBarriers();auto& list=cp.GetDeferredCommandList();
    cp.SetExternalPipeline(pipeline_.Get());list.D3DSetComputeRootSignature(root_.Get());for(unsigned i=0;i<3;++i)list.D3DSetComputeRootDescriptorTable(i,descriptors[i].second);
    struct Constants{SceneOcclusionSettings settings;uint32_t width,height,out_width,out_height;} constants{settings,binding.width_,binding.height_,(binding.width_+1)/2,(binding.height_+1)/2};static_assert(sizeof(constants)==32);
    list.D3DSetComputeRoot32BitConstants(3,8,&constants,0);list.D3DDispatch((constants.out_width+7)/8,(constants.out_height+7)/8,1);
    cp.PushTransitionBarrier(binding.result(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,read);cp.SubmitBarriers();binding.targets_->written=true;binding.revision_=++binding.targets_->revision;binding.submission_=cp.GetCurrentSubmission();binding.frame_=cp.GetCurrentFrame();return true;
}

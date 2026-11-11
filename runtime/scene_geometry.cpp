#include "built_shaders.h"
#include "scene_geometry.h"
#include "scene_geometry_targets.h"
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <cmath>
#include <cstring>
#include <string>
#include <immintrin.h>
namespace {
struct Fp{unsigned csr=_mm_getcsr();~Fp(){_mm_setcsr(csr);}};
constexpr auto read=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
}
ID3D12Resource* em2::SceneGeometryBindings::positions()const{return targets_?targets_->positions.Get():nullptr;}
ID3D12Resource* em2::SceneGeometryBindings::normals()const{return targets_?targets_->normals.Get():nullptr;}
bool em2::SceneGeometryBindings::recorded()const{return targets_ && targets_->written;}
bool em2::SceneGeometryPass::Initialize(ID3D12Device* device){
    Fp fp;if(!device)return false;if(pipeline_)return device_.Get()==device;
    
    Microsoft::WRL::ComPtr<ID3DBlob> errors,serialized;
    const auto* shader=em2::GetBuiltShader(em2::BuiltShader::SceneGeometry);if(!shader)return false;
    D3D12_ROOT_PARAMETER parameters[4]{};
    const D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,1,0,0}};
    for(unsigned i=0;i<3;++i){parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[i].DescriptorTable={1,&ranges[i]};}
    parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[3].Constants={0,0,32};
    const D3D12_ROOT_SIGNATURE_DESC root{4,parameters};Microsoft::WRL::ComPtr<ID3D12RootSignature> signature;
    if(FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors)) || FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&signature))))return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=signature.Get();desc.CS={shader->GetBufferPointer(),shader->GetBufferSize()};Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    if(FAILED(device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&pipeline))))return false;
    device_=device;root_=std::move(signature);pipeline_=std::move(pipeline);return true;
}
std::shared_ptr<em2::SceneGeometryBindings> em2::SceneGeometryPass::CreateBindings(std::shared_ptr<const SceneDepthView> source,std::shared_ptr<const SceneGeometryBindings> previous)const{
    if(!pipeline_ || !source || !source->resource())return {};auto desc=source->resource()->GetDesc();
    if(!desc.Width || desc.Width>16384 || !desc.Height || desc.Height>16384)return {};
    Microsoft::WRL::ComPtr<ID3D12Device> source_device;if(FAILED(source->resource()->GetDevice(IID_PPV_ARGS(&source_device))) || source_device.Get()!=device_.Get())return {};
    auto result=std::make_shared<SceneGeometryBindings>();result->source_=std::move(source);result->width_=uint32_t(desc.Width);result->height_=desc.Height;
    if(previous && previous->targets_ && previous->width_==result->width_ && previous->height_==result->height_){Microsoft::WRL::ComPtr<ID3D12Device> old_device;
        if(SUCCEEDED(previous->positions()->GetDevice(IID_PPV_ARGS(&old_device))) && old_device.Get()==device_.Get())result->targets_=previous->targets_;}
    D3D12_RESOURCE_DESC outputs[2]{};uint64_t bytes=0;
    for(unsigned i=0;i<2;++i){auto& d=outputs[i];d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=desc.Width;d.Height=desc.Height;d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=i?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const auto size=device_->GetResourceAllocationInfo(0,1,&d).SizeInBytes;if(size>256ull*1024*1024-bytes)return {};bytes+=size;}
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    if(!result->targets_){result->targets_=std::make_shared<SceneGeometryTargets>();
        if(FAILED(device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&outputs[0],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&result->targets_->positions))) || FAILED(device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&outputs[1],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&result->targets_->normals))))return {};}
    const D3D12_DESCRIPTOR_HEAP_DESC views{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,3,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};if(FAILED(device_->CreateDescriptorHeap(&views,IID_PPV_ARGS(&result->descriptors_))))return {};
    auto handle=result->descriptors_->GetCPUDescriptorHandleForHeapStart();const auto step=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    device_->CopyDescriptorsSimple(1,handle,result->source_->srv(),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for(unsigned i=0;i<2;++i){handle.ptr+=step;D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=outputs[i].Format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device_->CreateUnorderedAccessView(i?result->normals():result->positions(),nullptr,&uav,handle);}
    return result;
}
bool em2::SceneGeometryPass::Record(rex::graphics::d3d12::D3D12CommandProcessor& cp,SceneGeometryBindings& binding,const SceneGeometrySettings& settings)const{
    Fp fp;if(!pipeline_ || !binding.source_ || !binding.targets_ || cp.GetD3D12Provider().GetDevice()!=device_.Get())return false;
    for(const auto& row:settings.inverse_view_projection)for(float v:row)if(!std::isfinite(v))return false;
    for(float v:settings.clip_from_uv)if(!std::isfinite(v))return false;
    for(float v:settings.camera_and_discontinuity)if(!std::isfinite(v))return false;
    if(!settings.clip_from_uv[0] || !settings.clip_from_uv[1] || settings.camera_and_discontinuity[3]<=0 || settings.camera_and_discontinuity[3]>1)return false;
    // Use camera-axis depth for silhouette rejection. Radial distance would
    // reject valid neighbours on a coarse, flat plane near the camera.
    double points[2][3]{};
    for(unsigned i=0;i<2;++i){const double z=i?.75:.25;const auto& rows=settings.inverse_view_projection;const double w=double(rows[3][2])*z+rows[3][3];if(!std::isfinite(w)||std::abs(w)<1e-7)return false;
        for(unsigned c=0;c<3;++c)points[i][c]=(double(rows[c][2])*z+rows[c][3])/w;}
    double direction[3],length2=0;for(unsigned c=0;c<3;++c){direction[c]=points[1][c]-points[0][c];length2+=direction[c]*direction[c];}if(!std::isfinite(length2)||length2<1e-20)return false;
    SceneDrawInputs::Row forward{};for(unsigned c=0;c<3;++c)forward[c]=float(direction[c]/std::sqrt(length2));
    auto relative_rows=settings.inverse_view_projection;auto relative_camera=settings.camera_and_discontinuity;
    for(unsigned row=0;row<3;++row){for(unsigned col=0;col<4;++col){relative_rows[row][col]=float(double(settings.inverse_view_projection[row][col])-double(settings.camera_and_discontinuity[row])*settings.inverse_view_projection[3][col]);if(!std::isfinite(relative_rows[row][col]))return false;}relative_camera[row]=0;}
    std::array<rex::ui::d3d12::util::DescriptorCpuGpuHandlePair,3> descriptors;if(!cp.RequestOneUseSingleViewDescriptors(3,descriptors.data()))return false;
    auto source=binding.descriptors_->GetCPUDescriptorHandleForHeapStart();const auto step=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for(unsigned i=0;i<3;++i){device_->CopyDescriptorsSimple(1,descriptors[i].first,source,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);source.ptr+=step;}
    if(binding.targets_->written){cp.PushTransitionBarrier(binding.positions(),read,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);cp.PushTransitionBarrier(binding.normals(),read,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);}
    cp.SubmitBarriers();auto& list=cp.GetDeferredCommandList();
    cp.SetExternalPipeline(pipeline_.Get());list.D3DSetComputeRootSignature(root_.Get());
    for(unsigned i=0;i<3;++i)list.D3DSetComputeRootDescriptorTable(i,descriptors[i].second);
    struct Constants{std::array<SceneDrawInputs::Row,4> rows;SceneDrawInputs::Row transform,camera,forward;uint32_t width,height,padding[2];} constants{relative_rows,settings.clip_from_uv,relative_camera,forward,binding.width_,binding.height_,{}};static_assert(sizeof(constants)==32*4);
    list.D3DSetComputeRoot32BitConstants(3,32,&constants,0);list.D3DDispatch((binding.width_+7)/8,(binding.height_+7)/8,1);
    cp.PushTransitionBarrier(binding.positions(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,read);cp.PushTransitionBarrier(binding.normals(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,read);cp.SubmitBarriers();binding.targets_->written=true;return true;
}

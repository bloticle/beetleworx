#include "built_shaders.h"
#include "scene_indirect.h"
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <cmath>
#include <algorithm>
#include <immintrin.h>
namespace {
constexpr auto read=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
struct Fp{unsigned csr=_mm_getcsr();~Fp(){_mm_setcsr(csr);}};
auto Key(const rex::graphics::draw_util::ResolveInfo& i){return std::array<uint32_t,9>{i.copy_dest_base,i.copy_dest_info.value,i.copy_dest_coordinate_info.packed,i.coordinate_info.width_div_8,i.height_div_8,i.coordinate_info.draw_resolution_scale_x,i.coordinate_info.draw_resolution_scale_y,i.copy_dest_extent_start,i.copy_dest_extent_length};}
bool Compatible(const em2::HdrResolvedScene& colour,const em2::HdrResolvedScene& surface,const em2::SceneOcclusionBindings& ao){return !colour.lighting_applied() && colour.view() && colour.view()==surface.view() && colour.view()==ao.view() && colour.view()->frame==colour.frame() && colour.view()->submission==colour.submission() && colour.encoding()==em2::HdrSceneEncoding::kNativeLinear && surface.encoding()==em2::HdrSceneEncoding::kNativeLinear && colour.valid() && surface.valid() && colour.layer()==em2::SceneResolveLayer::kHdr && surface.layer()==em2::SceneResolveLayer::kMaterialSurface && Key(colour.info())==Key(surface.info()) && colour.frame()==surface.frame() && colour.submission()==surface.submission() && colour.frame()==ao.frame() && colour.submission()==ao.submission() && ao.current();}
}
struct em2::SceneIndirectTargets {Microsoft::WRL::ComPtr<ID3D12Resource> result;bool written=false;uint64_t revision=0;};
ID3D12Resource* em2::SceneIndirectBindings::result()const{return targets_?targets_->result.Get():nullptr;}
bool em2::SceneIndirectBindings::current()const{return targets_ && output_revision_ && output_revision_==targets_->revision && colour_ && surface_ && geometry_view_ && Compatible(*colour_,*surface_,*geometry_view_) && colour_revision_==colour_->revision() && surface_revision_==surface_->revision() && geometry_revision_==geometry_view_->revision();}
bool em2::SceneIndirectBindings::can_use_with(const HdrResolvedScene& image)const{
    return current() && colour_.get()==&image && surface_ && geometry_view_ && Compatible(image,*surface_,*geometry_view_) && colour_revision_==image.revision() && surface_revision_==surface_->revision() && geometry_revision_==geometry_view_->revision();
}
bool em2::SceneIndirectPass::Initialize(ID3D12Device* device){
    Fp fp;if(!device)return false;if(pipeline_)return device_.Get()==device;
    
    Microsoft::WRL::ComPtr<ID3DBlob> errors,serialized;
    const auto* shader=em2::GetBuiltShader(em2::BuiltShader::SceneIndirect);if(!shader)return false;

    D3D12_ROOT_PARAMETER parameters[6]{};D3D12_DESCRIPTOR_RANGE ranges[5]{};
    for(unsigned i=0;i<5;++i){ranges[i]={i<4?D3D12_DESCRIPTOR_RANGE_TYPE_SRV:D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,i<4?i:0,0,0};parameters[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[i+1].DescriptorTable={1,&ranges[i]};}
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants={0,0,8};const D3D12_ROOT_SIGNATURE_DESC root{6,parameters};Microsoft::WRL::ComPtr<ID3D12RootSignature> signature;
    if(FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors)) || FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&signature))))return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=signature.Get();desc.CS={shader->GetBufferPointer(),shader->GetBufferSize()};Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;if(FAILED(device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&pipeline))))return false;
    device_=device;root_=std::move(signature);pipeline_=std::move(pipeline);return true;
}
std::shared_ptr<em2::SceneIndirectBindings> em2::SceneIndirectPass::CreateBindings(std::shared_ptr<const HdrResolvedScene> colour,std::shared_ptr<const HdrResolvedScene> surface,std::shared_ptr<const SceneOcclusionBindings> ao,SceneIndirectEncoding encoding,std::shared_ptr<const SceneIndirectBindings> previous)const{
    if(!pipeline_ || encoding!=SceneIndirectEncoding::kLinearDiffuse || !colour || !surface || !ao || !Compatible(*colour,*surface,*ao) || !ao->geometry() || !ao->geometry()->recorded())return {};
    const auto geometry=ao->geometry();const std::array<ID3D12Resource*,4> resources{colour->texture(),surface->texture(),geometry->positions(),geometry->normals()};if(!resources[0] || resources[0]==resources[1])return {};const auto first=resources[0]->GetDesc();
    const auto& info=colour->info();if(colour->view()->width!=first.Width || colour->view()->height!=first.Height || !first.Width || first.Width>16384 || !first.Height || first.Height>16384 || !info.copy_dest_extent_length || uint64_t(info.coordinate_info.width_div_8)*8*info.coordinate_info.draw_resolution_scale_x!=first.Width || uint64_t(info.height_div_8)*8*info.coordinate_info.draw_resolution_scale_y!=first.Height)return {};
    for(unsigned i=0;i<resources.size();++i){if(!resources[i])return {};const auto d=resources[i]->GetDesc();const auto format=i==2?DXGI_FORMAT_R32G32B32A32_FLOAT:DXGI_FORMAT_R16G16B16A16_FLOAT;Microsoft::WRL::ComPtr<ID3D12Device> owner;
        if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Format!=format || d.SampleDesc.Count!=1 || d.SampleDesc.Quality || d.DepthOrArraySize!=1 || d.MipLevels!=1 || d.Width!=first.Width || d.Height!=first.Height || FAILED(resources[i]->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get()!=device_.Get())return {};}
    auto binding=std::make_shared<SceneIndirectBindings>();binding->colour_=std::move(colour);binding->surface_=std::move(surface);binding->geometry_view_=std::move(ao);binding->width_=uint32_t(first.Width);binding->height_=first.Height;binding->colour_revision_=binding->colour_->revision();binding->surface_revision_=binding->surface_->revision();binding->geometry_revision_=binding->geometry_view_->revision();
    auto output=first;output.Width=(first.Width+1)/2;output.Height=(first.Height+1)/2;output.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;output.Alignment=0;output.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;const auto bytes=device_->GetResourceAllocationInfo(0,1,&output).SizeInBytes;if(!bytes || bytes==UINT64_MAX || bytes>64ull*1024*1024)return {};
    if(previous && previous->targets_ && std::find(resources.begin(),resources.end(),previous->result())==resources.end() && previous->width_==binding->width_ && previous->height_==binding->height_){Microsoft::WRL::ComPtr<ID3D12Device> owner;if(SUCCEEDED(previous->result()->GetDevice(IID_PPV_ARGS(&owner))) && owner.Get()==device_.Get())binding->targets_=previous->targets_;}
    if(!binding->targets_){binding->targets_=std::make_shared<SceneIndirectTargets>();D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;if(FAILED(device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&binding->targets_->result))))return {};}
    const D3D12_DESCRIPTOR_HEAP_DESC views{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,5,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};if(FAILED(device_->CreateDescriptorHeap(&views,IID_PPV_ARGS(&binding->descriptors_))))return {};
    auto handle=binding->descriptors_->GetCPUDescriptorHandleForHeapStart();const auto step=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for(unsigned i=0;i<4;++i){D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=resources[i]->GetDesc().Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;device_->CreateShaderResourceView(resources[i],&srv,handle);handle.ptr+=step;}
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=output.Format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device_->CreateUnorderedAccessView(binding->result(),nullptr,&uav,handle);return binding;
}
bool em2::SceneIndirectPass::Record(rex::graphics::d3d12::D3D12CommandProcessor& cp,SceneIndirectBindings& b,const SceneIndirectSettings& settings)const{
    Fp fp;if(!pipeline_ || !b.colour_ || !b.surface_ || !b.geometry_view_ || !b.targets_ || cp.GetD3D12Provider().GetDevice()!=device_.Get() || !Compatible(*b.colour_,*b.surface_,*b.geometry_view_) || b.colour_revision_!=b.colour_->revision() || b.surface_revision_!=b.surface_->revision() || b.geometry_revision_!=b.geometry_view_->revision() || b.colour_->frame()!=cp.GetCurrentFrame() || b.colour_->submission()!=cp.GetCurrentSubmission())return false;
    for(float v:{settings.strength,settings.world_radius,settings.pixel_radius,settings.thickness})if(!std::isfinite(v))return false;
    if(settings.strength<0 || settings.strength>1 || settings.world_radius<1e-4f || settings.world_radius>1e6f || settings.pixel_radius<1 || settings.pixel_radius>256 || settings.thickness<1e-5f || settings.thickness>settings.world_radius*.25f)return false;
    std::array<rex::ui::d3d12::util::DescriptorCpuGpuHandlePair,5> descriptors;if(!cp.RequestOneUseSingleViewDescriptors(5,descriptors.data()))return false;auto source=b.descriptors_->GetCPUDescriptorHandleForHeapStart();const auto step=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);for(unsigned i=0;i<5;++i){device_->CopyDescriptorsSimple(1,descriptors[i].first,source,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);source.ptr+=step;}
    // Descriptor allocation may change native submission/heap state. Do not
    // record work using a frame whose lifetime belongs to an older submission.
    if(b.colour_->frame()!=cp.GetCurrentFrame() || b.colour_->submission()!=cp.GetCurrentSubmission())return false;
    if(b.targets_->written)cp.PushTransitionBarrier(b.result(),read,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);cp.SubmitBarriers();auto& list=cp.GetDeferredCommandList();cp.SetExternalPipeline(pipeline_.Get());list.D3DSetComputeRootSignature(root_.Get());for(unsigned i=0;i<5;++i)list.D3DSetComputeRootDescriptorTable(i+1,descriptors[i].second);
    struct Constants{uint32_t width,height,out_width,out_height;float radius,pixel_radius,thickness,strength;}constants{b.width_,b.height_,(b.width_+1)/2,(b.height_+1)/2,settings.world_radius,settings.pixel_radius,settings.thickness,settings.strength};static_assert(sizeof(constants)==32);list.D3DSetComputeRoot32BitConstants(0,8,&constants,0);list.D3DDispatch(((b.width_+1)/2+7)/8,((b.height_+1)/2+7)/8,1);cp.PushTransitionBarrier(b.result(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,read);cp.SubmitBarriers();b.targets_->written=true;b.output_revision_=++b.targets_->revision;return true;
}

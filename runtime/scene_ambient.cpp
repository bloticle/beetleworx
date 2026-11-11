#include "built_shaders.h"
#include "scene_ambient.h"
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
bool Compatible(const em2::HdrResolvedScene& colour,const em2::HdrResolvedScene& ambient,const em2::SceneOcclusionBindings& ao){return !colour.lighting_applied() && colour.view() && colour.view()==ambient.view() && colour.view()==ao.view() && colour.view()->frame==colour.frame() && colour.view()->submission==colour.submission() && colour.encoding()==em2::HdrSceneEncoding::kNativeLinear && ambient.encoding()==em2::HdrSceneEncoding::kNativeLinear && colour.valid() && ambient.valid() && colour.layer()==em2::SceneResolveLayer::kHdr && ambient.layer()==em2::SceneResolveLayer::kMaterialAmbient && Key(colour.info())==Key(ambient.info()) && colour.frame()==ambient.frame() && colour.submission()==ambient.submission() && colour.frame()==ao.frame() && colour.submission()==ao.submission() && ao.current();}
}
struct em2::SceneAmbientTargets {Microsoft::WRL::ComPtr<ID3D12Resource> result;bool written=false;uint64_t revision=0;};
ID3D12Resource* em2::SceneAmbientBindings::result()const{return targets_?targets_->result.Get():nullptr;}
bool em2::SceneAmbientBindings::current()const{return targets_ && output_revision_ && output_revision_==targets_->revision;}
bool em2::SceneAmbientBindings::can_publish_to(const HdrResolvedScene& image)const{
    return current() && (!indirect_ || indirect_->can_use_with(image)) && colour_.get()==&image && ambient_ && occlusion_ && Compatible(image,*ambient_,*occlusion_) && colour_revision_==image.revision() && ambient_revision_==ambient_->revision() && occlusion_revision_==occlusion_->revision();
}
bool em2::SceneAmbientPass::Initialize(ID3D12Device* device){
    Fp fp;if(!device)return false;if(pipeline_)return device_.Get()==device;
    
    Microsoft::WRL::ComPtr<ID3DBlob> errors,serialized;
    const auto* shader=em2::GetBuiltShader(em2::BuiltShader::SceneAmbient);if(!shader)return false;

    D3D12_ROOT_PARAMETER parameters[9]{};D3D12_DESCRIPTOR_RANGE ranges[8]{};
    for(unsigned i=0;i<8;++i){ranges[i]={i<7?D3D12_DESCRIPTOR_RANGE_TYPE_SRV:D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,i<7?i:0,0,0};parameters[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[i+1].DescriptorTable={1,&ranges[i]};}
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants={0,0,12};const D3D12_ROOT_SIGNATURE_DESC root{9,parameters};Microsoft::WRL::ComPtr<ID3D12RootSignature> signature;
    if(FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors)) || FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&signature))))return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=signature.Get();desc.CS={shader->GetBufferPointer(),shader->GetBufferSize()};Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;if(FAILED(device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&pipeline))))return false;
    device_=device;root_=std::move(signature);pipeline_=std::move(pipeline);return true;
}
std::shared_ptr<em2::SceneAmbientBindings> em2::SceneAmbientPass::CreateBindings(std::shared_ptr<const HdrResolvedScene> colour,std::shared_ptr<const HdrResolvedScene> ambient,std::shared_ptr<const SceneOcclusionBindings> ao,SceneAmbientEncoding encoding,std::shared_ptr<const SceneAmbientBindings> previous,std::shared_ptr<const SceneIndirectBindings> indirect)const{
    if(!pipeline_ || encoding!=SceneAmbientEncoding::kLinear || !colour || !ambient || !ao || !Compatible(*colour,*ambient,*ao) || !ao->geometry() || !ao->geometry()->recorded())return {};
    if(indirect && (!indirect->can_use_with(*colour) || indirect->geometry_view()!=ao))return {};
    const auto geometry=ao->geometry();const std::array<ID3D12Resource*,7> resources{colour->texture(),ambient->texture(),geometry->positions(),geometry->normals(),ao->result(),indirect?indirect->result():colour->texture(),indirect?indirect->surface()->texture():colour->texture()};if(!resources[0])return {};const auto first=resources[0]->GetDesc();
    const auto& info=colour->info();if(colour->view()->width!=first.Width || colour->view()->height!=first.Height || !first.Width || first.Width>16384 || !first.Height || first.Height>16384 || !info.copy_dest_extent_length || uint64_t(info.coordinate_info.width_div_8)*8*info.coordinate_info.draw_resolution_scale_x!=first.Width || uint64_t(info.height_div_8)*8*info.coordinate_info.draw_resolution_scale_y!=first.Height)return {};
    for(unsigned i=0;i<resources.size();++i){if(!resources[i])return {};const auto d=resources[i]->GetDesc();const auto format=i==2?DXGI_FORMAT_R32G32B32A32_FLOAT:DXGI_FORMAT_R16G16B16A16_FLOAT;Microsoft::WRL::ComPtr<ID3D12Device> owner;
        if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Format!=format || d.SampleDesc.Count!=1 || d.SampleDesc.Quality || d.DepthOrArraySize!=1 || d.MipLevels!=1 || d.Width!=((i==4 || (i==5 && indirect))?(first.Width+1)/2:first.Width) || d.Height!=((i==4 || (i==5 && indirect))?(first.Height+1)/2:first.Height) || FAILED(resources[i]->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get()!=device_.Get())return {};}
    auto binding=std::make_shared<SceneAmbientBindings>();binding->colour_=std::move(colour);binding->ambient_=std::move(ambient);binding->occlusion_=std::move(ao);binding->indirect_=std::move(indirect);binding->width_=uint32_t(first.Width);binding->height_=first.Height;binding->colour_revision_=binding->colour_->revision();binding->ambient_revision_=binding->ambient_->revision();binding->occlusion_revision_=binding->occlusion_->revision();
    auto output=first;output.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;output.Alignment=0;output.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;const auto bytes=device_->GetResourceAllocationInfo(0,1,&output).SizeInBytes;if(!bytes || bytes==UINT64_MAX || bytes>64ull*1024*1024)return {};
    if(previous && previous->targets_ && std::find(resources.begin(),resources.end(),previous->result())==resources.end() && previous->width_==binding->width_ && previous->height_==binding->height_){Microsoft::WRL::ComPtr<ID3D12Device> owner;if(SUCCEEDED(previous->result()->GetDevice(IID_PPV_ARGS(&owner))) && owner.Get()==device_.Get())binding->targets_=previous->targets_;}
    if(!binding->targets_){binding->targets_=std::make_shared<SceneAmbientTargets>();D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;if(FAILED(device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&binding->targets_->result))))return {};}
    const D3D12_DESCRIPTOR_HEAP_DESC views{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,8,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};if(FAILED(device_->CreateDescriptorHeap(&views,IID_PPV_ARGS(&binding->descriptors_))))return {};
    auto handle=binding->descriptors_->GetCPUDescriptorHandleForHeapStart();const auto step=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for(unsigned i=0;i<7;++i){D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=resources[i]->GetDesc().Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;device_->CreateShaderResourceView(resources[i],&srv,handle);handle.ptr+=step;}
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=output.Format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device_->CreateUnorderedAccessView(binding->result(),nullptr,&uav,handle);return binding;
}
bool em2::SceneAmbientPass::Record(rex::graphics::d3d12::D3D12CommandProcessor& cp,SceneAmbientBindings& b,const SceneAmbientSettings& settings)const{
    Fp fp;if((b.indirect_ && (!b.colour_ || !b.indirect_->can_use_with(*b.colour_))) || !pipeline_ || !b.colour_ || !b.ambient_ || !b.occlusion_ || !b.targets_ || cp.GetD3D12Provider().GetDevice()!=device_.Get() || !Compatible(*b.colour_,*b.ambient_,*b.occlusion_) || b.colour_revision_!=b.colour_->revision() || b.ambient_revision_!=b.ambient_->revision() || b.occlusion_revision_!=b.occlusion_->revision() || b.colour_->frame()!=cp.GetCurrentFrame() || b.colour_->submission()!=cp.GetCurrentSubmission())return false;
    for(float v:{settings.strength,settings.plane_distance,settings.normal_cosine,settings.indirect_strength,settings.material_distance})if(!std::isfinite(v))return false;if(settings.strength<0 || settings.strength>1 || settings.plane_distance<1e-5f || settings.plane_distance>1e6f || settings.normal_cosine<0 || settings.normal_cosine>=.9999f || settings.indirect_strength<0 || settings.indirect_strength>1 || settings.material_distance<0 || settings.material_distance>1)return false;
    std::array<rex::ui::d3d12::util::DescriptorCpuGpuHandlePair,8> descriptors;if(!cp.RequestOneUseSingleViewDescriptors(8,descriptors.data()))return false;auto source=b.descriptors_->GetCPUDescriptorHandleForHeapStart();const auto step=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);for(unsigned i=0;i<8;++i){device_->CopyDescriptorsSimple(1,descriptors[i].first,source,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);source.ptr+=step;}
    // Descriptor allocation may change native submission/heap state. Do not
    // record work using a frame whose lifetime belongs to an older submission.
    if(b.colour_->frame()!=cp.GetCurrentFrame() || b.colour_->submission()!=cp.GetCurrentSubmission())return false;
    if(b.targets_->written)cp.PushTransitionBarrier(b.result(),read,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);cp.SubmitBarriers();auto& list=cp.GetDeferredCommandList();cp.SetExternalPipeline(pipeline_.Get());list.D3DSetComputeRootSignature(root_.Get());for(unsigned i=0;i<8;++i)list.D3DSetComputeRootDescriptorTable(i+1,descriptors[i].second);
    struct Constants{uint32_t width,height,ao_width,ao_height;float strength,plane_distance,normal_cosine,indirect_strength;float material_distance,padding[3];}constants{b.width_,b.height_,(b.width_+1)/2,(b.height_+1)/2,settings.strength,settings.plane_distance,settings.normal_cosine,b.indirect_?settings.indirect_strength:0,settings.material_distance,{}};static_assert(sizeof(constants)==48);list.D3DSetComputeRoot32BitConstants(0,12,&constants,0);list.D3DDispatch((b.width_+7)/8,(b.height_+7)/8,1);cp.PushTransitionBarrier(b.result(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,read);cp.SubmitBarriers();b.targets_->written=true;b.output_revision_=++b.targets_->revision;return true;
}

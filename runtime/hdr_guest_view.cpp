#include "built_shaders.h"
#include "hdr_guest_view.h"
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/deferred_command_list.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <immintrin.h>
namespace {
using GuestCP=rex::graphics::d3d12::D3D12CommandProcessor;
template<class Tag,auto Member>struct GuestAccess{friend auto MemberOf(Tag){return Member;}};
struct GuestSubmission{friend auto MemberOf(GuestSubmission);};template struct GuestAccess<GuestSubmission,&GuestCP::submission_open_>;
struct GuestBindless{friend auto MemberOf(GuestBindless);};template struct GuestAccess<GuestBindless,&GuestCP::bindless_resources_used_>;
struct GuestBindfulPool{friend auto MemberOf(GuestBindfulPool);};template struct GuestAccess<GuestBindfulPool,&GuestCP::view_bindful_heap_pool_>;
struct Fp{unsigned value=_mm_getcsr();~Fp(){_mm_setcsr(value);}};constexpr auto read=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;}
std::shared_ptr<em2::HdrGuestViewBindings> em2::HdrGuestViewBindings::Create(ID3D12Resource* source,DXGI_FORMAT output_format){
    Fp fp;if(!source || (output_format!=DXGI_FORMAT_R16G16B16A16_FLOAT && output_format!=DXGI_FORMAT_R8G8B8A8_UNORM))return {};const auto d=source->GetDesc();if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT || !d.Width || !d.Height || d.Width>16384 || d.Height>16384 || d.SampleDesc.Count!=1 || d.DepthOrArraySize!=1 || d.MipLevels!=1)return {};
    Microsoft::WRL::ComPtr<ID3D12Device> device;if(FAILED(source->GetDevice(IID_PPV_ARGS(&device))))return {};auto output=d;output.Format=output_format;output.Alignment=0;output.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;output.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto bytes=device->GetResourceAllocationInfo(0,1,&output).SizeInBytes;if(!bytes || bytes==UINT64_MAX || bytes>64ull*1024*1024)return {};
    auto result=std::make_shared<HdrGuestViewBindings>();result->source_=source;result->width_=uint32_t(d.Width);result->height_=d.Height;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&result->output_))))return {};
    const D3D12_DESCRIPTOR_HEAP_DESC views{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};if(FAILED(device->CreateDescriptorHeap(&views,IID_PPV_ARGS(&result->descriptors_))))return {};
    auto handle=result->descriptors_->GetCPUDescriptorHandleForHeapStart();D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=d.Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;device->CreateShaderResourceView(source,&srv,handle);handle.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=output.Format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(result->output_.Get(),nullptr,&uav,handle);return result;
}
bool em2::HdrGuestViewPass::Initialize(ID3D12Device* device){
    Fp fp;if(!device)return false;if(pipeline_)return device_.Get()==device;
    Microsoft::WRL::ComPtr<ID3DBlob> errors,serialized;const auto* shader=em2::GetBuiltShader(em2::BuiltShader::HdrGuestView);if(!shader)return false;
    const D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};D3D12_ROOT_PARAMETER parameters[3]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants={0,0,4};for(unsigned i=0;i<2;++i){parameters[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[i+1].DescriptorTable={1,ranges+i};}const D3D12_ROOT_SIGNATURE_DESC desc{3,parameters};Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    if(FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors)) || FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root))))return false;D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=root.Get();pso.CS={shader->GetBufferPointer(),shader->GetBufferSize()};Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;if(FAILED(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&pipeline))))return false;device_=device;root_=std::move(root);pipeline_=std::move(pipeline);return true;
}
bool em2::HdrGuestViewPass::Record(rex::graphics::d3d12::D3D12CommandProcessor& cp,HdrGuestViewBindings& b,HdrGuestTransfer transfer,int32_t exponent_bias){
    Fp fp;if(!(cp.*MemberOf(GuestSubmission{})) || (!(cp.*MemberOf(GuestBindless{})) && !(cp.*MemberOf(GuestBindfulPool{}))))return false;if(exponent_bias<-32 || exponent_bias>31)return false;if(!pipeline_ || !b.source_ || !b.output_ || (transfer!=HdrGuestTransfer::kRaw && transfer!=HdrGuestTransfer::kXenosPwl && transfer!=HdrGuestTransfer::kSrgb) || cp.GetD3D12Provider().GetDevice()!=device_.Get())return false;
    const bool packed=b.output_->GetDesc().Format==DXGI_FORMAT_R8G8B8A8_UNORM;if(packed!=(transfer==HdrGuestTransfer::kRaw) || (packed && exponent_bias))return false;const auto frame=cp.GetCurrentFrame(),submission=cp.GetCurrentSubmission();Microsoft::WRL::ComPtr<ID3D12Device> owner;if(FAILED(b.source_->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get()!=device_.Get())return false;
    rex::ui::d3d12::util::DescriptorCpuGpuHandlePair views[2];if(!cp.RequestOneUseSingleViewDescriptors(2,views))return false;auto source=b.descriptors_->GetCPUDescriptorHandleForHeapStart();const auto step=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);for(unsigned i=0;i<2;++i){device_->CopyDescriptorsSimple(1,views[i].first,source,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);source.ptr+=step;}
    if(frame!=cp.GetCurrentFrame() || submission!=cp.GetCurrentSubmission())return false;
    if(b.written_)cp.PushTransitionBarrier(b.result(),read,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);cp.SubmitBarriers();cp.SetExternalPipeline(pipeline_.Get());auto& list=cp.GetDeferredCommandList();list.D3DSetComputeRootSignature(root_.Get());const uint32_t constants[]={b.width_,b.height_,uint32_t(transfer),uint32_t(exponent_bias)};list.D3DSetComputeRoot32BitConstants(0,4,constants,0);list.D3DSetComputeRootDescriptorTable(1,views[0].second);list.D3DSetComputeRootDescriptorTable(2,views[1].second);list.D3DDispatch((b.width_+7)/8,(b.height_+7)/8,1);cp.PushTransitionBarrier(b.result(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,read);cp.SubmitBarriers();b.written_=true;return true;
}

#include "scene_raster_pass.h"
#include "scene_geometry_targets.h"
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/render_target_cache.h>
#include <rex/graphics/d3d12/shader.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <immintrin.h>
namespace {
using CP=rex::graphics::d3d12::D3D12CommandProcessor;
template<class Tag,auto Member>struct Access{friend auto MemberOf(Tag){return Member;}};
#define FIELD(tag,field) struct tag{friend auto MemberOf(tag);};template struct Access<tag,&CP::field>
FIELD(Bindless,bindless_resources_used_);FIELD(BindlessHeap,sampler_bindless_heap_current_);FIELD(BindfulHeap,sampler_bindful_heap_current_);FIELD(SamplerStart,sampler_bindless_heap_gpu_start_);FIELD(PixelSamplers,gpu_handle_samplers_pixel_);FIELD(SamplerIndices,current_sampler_bindless_indices_pixel_);FIELD(PixelDescriptors,cbuffer_binding_descriptor_indices_pixel_);FIELD(SamplersWritten,bindful_samplers_written_pixel_);FIELD(Targets,render_target_cache_);
#undef FIELD
struct Fp{unsigned csr=_mm_getcsr();~Fp(){_mm_setcsr(csr);}};
constexpr auto read=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
}
struct em2::SceneRasterProgram {SceneRasterGeometryKernel kernel;std::vector<uint8_t> vertex;std::vector<uint32_t> geometry;};
std::optional<em2::NativeSceneSampler> em2::CaptureNativeSceneSampler(CP& cp,const SceneReplayInputs& replay){
    if(!IsSceneReplayCurrent(cp,replay))return {};
    auto* pixel=static_cast<rex::graphics::d3d12::D3D12Shader*>(cp.active_pixel_shader());if(!pixel)return {};
    const auto& bindings=pixel->GetSamplerBindingsAfterTranslation();size_t slot=0;while(slot<bindings.size() && bindings[slot].fetch_constant!=0)++slot;if(slot==bindings.size())return {};
    NativeSceneSampler result;uint64_t index=slot;
    if(cp.*MemberOf(Bindless{})){if(!(cp.*MemberOf(PixelDescriptors{})).up_to_date)return {};const auto& indices=cp.*MemberOf(SamplerIndices{});if(slot>=indices.size())return {};index=indices[slot];result.heap=cp.*MemberOf(BindlessHeap{});result.handle=cp.*MemberOf(SamplerStart{});}
    else {if(!(cp.*MemberOf(SamplersWritten{})))return {};result.heap=cp.*MemberOf(BindfulHeap{});result.handle=cp.*MemberOf(PixelSamplers{});}
    if(!result.heap || !result.handle.ptr)return {};const auto desc=result.heap->GetDesc();if(desc.Type!=D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER || !(desc.Flags&D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))return {};
    const uint64_t step=cp.GetD3D12Provider().GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER),start=result.heap->GetGPUDescriptorHandleForHeapStart().ptr;
    if(result.handle.ptr<start || (result.handle.ptr-start)%step)return {};const uint64_t offset=(result.handle.ptr-start)/step;if(index>=desc.NumDescriptors || offset>desc.NumDescriptors-1-index)return {};
    result.handle.ptr+=index*step;return result;
}
bool em2::SceneRasterGeometryPass::Initialize(ID3D12Device* device){
    if(!device)return false;if(device_)return device_.Get()==device;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=4096;desc.Height=desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&buffer))))return false;device_=device;unused_uav_=std::move(buffer);return true;
}
bool em2::TestNativeSceneSampler(CP& cp,const SceneReplayInputs& replay){
    const bool mode=cp.*MemberOf(Bindless{}),written=cp.*MemberOf(SamplersWritten{});auto* bindless_heap=cp.*MemberOf(BindlessHeap{});auto* bindful_heap=cp.*MemberOf(BindfulHeap{});const auto start=cp.*MemberOf(SamplerStart{}),pixel_start=cp.*MemberOf(PixelSamplers{});const auto descriptor=cp.*MemberOf(PixelDescriptors{});auto indices=cp.*MemberOf(SamplerIndices{});
    struct Restore{CP& cp;bool mode,written;ID3D12DescriptorHeap* bindless_heap;ID3D12DescriptorHeap* bindful_heap;D3D12_GPU_DESCRIPTOR_HANDLE start,pixel_start;decltype(descriptor) descriptor;std::vector<uint32_t> indices;
        ~Restore(){cp.*MemberOf(Bindless{})=mode;cp.*MemberOf(SamplersWritten{})=written;cp.*MemberOf(BindlessHeap{})=bindless_heap;cp.*MemberOf(BindfulHeap{})=bindful_heap;cp.*MemberOf(SamplerStart{})=start;cp.*MemberOf(PixelSamplers{})=pixel_start;cp.*MemberOf(PixelDescriptors{})=descriptor;cp.*MemberOf(SamplerIndices{})=std::move(indices);}} restore{cp,mode,written,bindless_heap,bindful_heap,start,pixel_start,descriptor,std::move(indices)};
    const auto* shader=static_cast<rex::graphics::d3d12::D3D12Shader*>(cp.active_pixel_shader());const auto& bindings=shader->GetSamplerBindingsAfterTranslation();size_t slot=0;while(slot<bindings.size() && bindings[slot].fetch_constant!=0)++slot;if(slot==bindings.size() || bindings.size()>3)return false;
    auto* device=cp.GetD3D12Provider().GetDevice();Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;const D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,4,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};if(FAILED(device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&heap))))return false;
    const auto base=heap->GetGPUDescriptorHandleForHeapStart();const auto step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);cp.*MemberOf(BindfulHeap{})=heap.Get();cp.*MemberOf(BindlessHeap{})=heap.Get();cp.*MemberOf(SamplerStart{})=base;auto offset=base;offset.ptr+=step;cp.*MemberOf(PixelSamplers{})=offset;cp.*MemberOf(SamplersWritten{})=true;(cp.*MemberOf(PixelDescriptors{})).up_to_date=true;
    auto& native_indices=cp.*MemberOf(SamplerIndices{});native_indices.resize(bindings.size());for(size_t i=0;i<bindings.size();++i)native_indices[i]=uint32_t(i+1);
    for(bool bindless:{false,true}){cp.*MemberOf(Bindless{})=bindless;auto captured=CaptureNativeSceneSampler(cp,replay);if(!captured || captured->heap.Get()!=heap.Get() || captured->handle.ptr!=base.ptr+(slot+1)*step)return false;
        if(bindless){native_indices[slot]=99;if(CaptureNativeSceneSampler(cp,replay))return false;native_indices[slot]=uint32_t(slot+1);(cp.*MemberOf(PixelDescriptors{})).up_to_date=false;if(CaptureNativeSceneSampler(cp,replay))return false;(cp.*MemberOf(PixelDescriptors{})).up_to_date=true;}
        else{cp.*MemberOf(SamplersWritten{})=false;if(CaptureNativeSceneSampler(cp,replay))return false;cp.*MemberOf(SamplersWritten{})=true;auto bad=base;bad.ptr-=step;cp.*MemberOf(PixelSamplers{})=bad;if(CaptureNativeSceneSampler(cp,replay))return false;cp.*MemberOf(PixelSamplers{})=offset;}
    }
    REXLOG_INFO("Native scene sampler capture passed: original translated TF0 binding slot{}, bindful table offset and bindless sampler indices, retained real heap identity, dirty/wrong-range handle rejection; descriptor values mocked, no GPU sampling or game draw claimed",slot);return true;
}
std::shared_ptr<em2::SceneRasterBindings> em2::SceneRasterGeometryPass::CreateBindings(CP& cp,const SceneReplayInputs& replay,std::shared_ptr<const SceneDepthView> depth,std::shared_ptr<const SceneRasterBindings> previous){
    Fp fp;if(!device_ || cp.GetD3D12Provider().GetDevice()!=device_.Get() || !depth || !depth->resource() || !replay.scene.raster || !IsSceneReplayCurrent(cp,replay))return {};
    auto sampler=CaptureNativeSceneSampler(cp,replay);if(!sampler)return {};RasterGeometryConstants constants;if(!PrepareRasterGeometryConstants(replay.scene,constants))return {};
    Microsoft::WRL::ComPtr<ID3D12Device> owner;if(FAILED(depth->resource()->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get()!=device_.Get())return {};
    const auto& raster=*replay.scene.raster;const double right=std::min(double(raster.viewport.TopLeftX)+raster.viewport.Width,double(raster.scissor.right)),bottom=std::min(double(raster.viewport.TopLeftY)+raster.viewport.Height,double(raster.scissor.bottom));
    if(!std::isfinite(right) || !std::isfinite(bottom) || right<=0 || bottom<=0 || right>16384 || bottom>16384)return {};const uint32_t width=uint32_t(std::ceil(right)),height=uint32_t(std::ceil(bottom));
    auto found=std::find_if(programs_.begin(),programs_.end(),[&](const auto& program){return std::equal(program->vertex.begin(),program->vertex.end(),replay.vertex_shader.begin(),replay.vertex_shader.end()) && std::equal(program->geometry.begin(),program->geometry.end(),replay.geometry_shader.begin(),replay.geometry_shader.end());});
    std::shared_ptr<SceneRasterProgram> program;
    if(found!=programs_.end()){program=*found;programs_.erase(found);}
    else{program=std::make_shared<SceneRasterProgram>();if(!program->kernel.Initialize(device_.Get(),replay.vertex_shader,replay.geometry_shader))return {};++program_builds_;program->vertex.assign(replay.vertex_shader.begin(),replay.vertex_shader.end());program->geometry.assign(replay.geometry_shader.begin(),replay.geometry_shader.end());}
    programs_.push_back(program);if(programs_.size()>8)programs_.erase(programs_.begin());
    auto result=std::make_shared<SceneRasterBindings>();result->capture_={replay.submission,replay.frame,replay.command_end};result->program_=std::move(program);result->depth_fetch_=replay.scene.depth_fetch;result->sampler_=std::move(*sampler);result->unused_uav_=unused_uav_;result->geometry_=std::make_shared<SceneGeometryBindings>();auto& output=*result->geometry_;output.source_=std::move(depth);output.width_=width;output.height_=height;
    if(previous && previous->geometry_ && previous->geometry_->width_==width && previous->geometry_->height_==height){auto* resource=previous->geometry_->positions();Microsoft::WRL::ComPtr<ID3D12Device> old_owner;if(resource && (resource->GetDesc().Flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) && SUCCEEDED(resource->GetDevice(IID_PPV_ARGS(&old_owner))) && old_owner.Get()==device_.Get())output.targets_=previous->geometry_->targets_;}
    if(!output.targets_){output.targets_=std::make_shared<SceneGeometryTargets>();D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;uint64_t bytes=0;
        for(unsigned i=0;i<2;++i){D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;desc.Height=height;desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Format=i?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            const auto size=device_->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;if(size>256ull*1024*1024-bytes)return {};bytes+=size;auto& resource=i?output.targets_->normals:output.targets_->positions;
            if(FAILED(device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&resource))))return {};}}
    if(!replay.shared_memory || replay.shared_memory->GetGPUVirtualAddress()!=replay.shared_address)return {};result->shared_memory_=replay.shared_memory;
    const D3D12_DESCRIPTOR_HEAP_DESC views{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,3,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0},rtvs{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,2,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};if(FAILED(device_->CreateDescriptorHeap(&views,IID_PPV_ARGS(&result->depth_srv_))) || FAILED(device_->CreateDescriptorHeap(&rtvs,IID_PPV_ARGS(&result->rtvs_))))return {};
    auto memory_view=result->depth_srv_->GetCPUDescriptorHandleForHeapStart();const auto view_step=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);memory_view.ptr+=view_step;
    D3D12_SHADER_RESOURCE_VIEW_DESC raw{};raw.Format=DXGI_FORMAT_R32_TYPELESS;raw.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;raw.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;raw.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;raw.Buffer.NumElements=uint32_t(replay.shared_memory->GetDesc().Width/4);device_->CreateShaderResourceView(replay.shared_memory,&raw,memory_view);memory_view.ptr+=view_step;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R32_TYPELESS;uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;uav.Buffer.NumElements=1024;uav.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;device_->CreateUnorderedAccessView(unused_uav_.Get(),nullptr,&uav,memory_view);
    device_->CopyDescriptorsSimple(1,result->depth_srv_->GetCPUDescriptorHandleForHeapStart(),output.source_->srv(),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);auto rtv=result->rtvs_->GetCPUDescriptorHandleForHeapStart();device_->CreateRenderTargetView(output.positions(),nullptr,rtv);rtv.ptr+=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);device_->CreateRenderTargetView(output.normals(),nullptr,rtv);return result;
}
bool em2::SceneRasterGeometryPass::Record(CP& cp,SceneRasterBindings& binding,const SceneReplayInputs& replay)const{
    Fp fp;if(!device_ || !binding.geometry_ || !binding.program_ || !replay.scene.raster || cp.GetD3D12Provider().GetDevice()!=device_.Get() || !IsSceneReplayCurrent(cp,replay))return false;
    if(binding.capture_.submission!=replay.submission || binding.capture_.frame!=replay.frame || binding.capture_.stream_offset!=replay.command_end)return false;
    if(std::memcmp(&binding.depth_fetch_,&replay.scene.depth_fetch,sizeof(binding.depth_fetch_)) || !std::equal(binding.program_->vertex.begin(),binding.program_->vertex.end(),replay.vertex_shader.begin(),replay.vertex_shader.end()) || !std::equal(binding.program_->geometry.begin(),binding.program_->geometry.end(),replay.geometry_shader.begin(),replay.geometry_shader.end()))return false;
    auto sampler=CaptureNativeSceneSampler(cp,replay);if(!sampler || sampler->heap.Get()!=binding.sampler_.heap.Get() || sampler->handle.ptr!=binding.sampler_.handle.ptr)return false;
    RasterGeometryConstants constants;if(!PrepareRasterGeometryConstants(replay.scene,constants) || replay.shared_memory!=binding.shared_memory_.Get())return false;
    auto& geometry=*binding.geometry_;auto& targets=*geometry.targets_;const auto& raster=*replay.scene.raster;
    const double right=std::min(double(raster.viewport.TopLeftX)+raster.viewport.Width,double(raster.scissor.right)),bottom=std::min(double(raster.viewport.TopLeftY)+raster.viewport.Height,double(raster.scissor.bottom));if(!std::isfinite(right) || !std::isfinite(bottom) || right<=0 || bottom<=0 || right>geometry.width_ || bottom>geometry.height_)return false;
    std::array<rex::ui::d3d12::util::DescriptorCpuGpuHandlePair,3> views;if(!cp.RequestOneUseSingleViewDescriptors(3,views.data()))return false;auto source_view=binding.depth_srv_->GetCPUDescriptorHandleForHeapStart();const auto view_step=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);for(unsigned i=0;i<3;++i){device_->CopyDescriptorsSimple(1,views[i].first,source_view,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);source_view.ptr+=view_step;}
    for(auto* resource:{geometry.positions(),geometry.normals()})cp.PushTransitionBarrier(resource,targets.written?read:D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_RENDER_TARGET);cp.SubmitBarriers();auto& list=cp.GetDeferredCommandList();
    // Ensure the next native draw rebinds its render targets after our MRTs.
    static_cast<rex::graphics::d3d12::D3D12RenderTargetCache*>((cp.*MemberOf(Targets{})).get())->InvalidateCommandListRenderTargets();
    cp.SetExternalPipeline(binding.program_->kernel.pipeline());cp.SetExternalGraphicsRootSignature(binding.program_->kernel.root());cp.SetViewport(raster.viewport);cp.SetScissorRect(raster.scissor);cp.SetPrimitiveTopology(replay.topology);
    list.D3DSetGraphicsRootConstantBufferView(0,replay.constants[0]);list.D3DSetGraphicsRootConstantBufferView(1,replay.constants[1]);list.D3DSetGraphicsRootConstantBufferView(2,replay.constants[3]);list.D3DSetGraphicsRootConstantBufferView(5,replay.constants[2]);list.D3DSetGraphicsRootConstantBufferView(6,replay.constants[4]?replay.constants[4]:replay.constants[0]);
    list.D3DSetGraphicsRootDescriptorTable(3,views[1].second);list.D3DSetGraphicsRootDescriptorTable(4,views[2].second);list.D3DSetGraphicsRootDescriptorTable(7,views[0].second);list.D3DSetGraphicsRootDescriptorTable(8,binding.sampler_.handle);list.D3DSetGraphicsRoot32BitConstants(9,20,&constants,0);
    auto handle=binding.rtvs_->GetCPUDescriptorHandleForHeapStart();D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2]={handle,handle};rtvs[1].ptr+=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);const float clear[4]={};for(auto rtv:rtvs)list.D3DClearRenderTargetView(rtv,clear,0,nullptr);list.D3DOMSetRenderTargets(2,rtvs,FALSE,nullptr);
    const auto& draw=raster.last_draw;if(draw.indexed){list.D3DIASetIndexBuffer(&*draw.indices);list.D3DDrawIndexedInstanced(draw.count,draw.instances,draw.start,draw.base_vertex,draw.start_instance);}else list.D3DDrawInstanced(draw.count,draw.instances,draw.start,draw.start_instance);
    for(auto* resource:{geometry.positions(),geometry.normals()})cp.PushTransitionBarrier(resource,D3D12_RESOURCE_STATE_RENDER_TARGET,read);cp.SubmitBarriers();targets.written=true;return true;
}

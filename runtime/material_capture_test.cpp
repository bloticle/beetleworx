#include "material_capture.h"
#include "material_ambient_export.h"
#include "hdr_scene_cache.h"
#include "hdr_scene_target.h"
#include "scene_shader_audit.h"
#include "native_pixel_fixture.h"
#include <d3d12sdklayers.h>
#include "../private/material_shader_data.h"
#include "../private/scene_shader_data.h"
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/pipeline_cache.h>
#include <rex/graphics/d3d12/render_target_cache.h>
#include <rex/graphics/register_file.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/kernel/xmemory.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <chrono>
#include <thread>
#include <bit>
#include <fstream>
#include <cmath>
namespace {
using CP=rex::graphics::d3d12::D3D12CommandProcessor;
template<class Tag,auto Member>struct Access{friend auto MemberOf(Tag){return Member;}};
struct BindingMode{friend auto MemberOf(BindingMode);};template struct Access<BindingMode,&CP::bindless_resources_used_>;
struct Memory{friend auto MemberOf(Memory);};template struct Access<Memory,&rex::graphics::CommandProcessor::memory_>;
using Cache=rex::graphics::d3d12::D3D12RenderTargetCache;
using Base=rex::graphics::RenderTargetCache;
using List=rex::graphics::d3d12::DeferredCommandList;
template<class Tag,class T>struct TypeAccess{friend auto TypeOf(Tag){return std::type_identity<T>{};}};
#define TYPE(tag,type) struct tag{friend auto TypeOf(tag);};template struct TypeAccess<tag,type>
TYPE(PipelineTag,rex::graphics::d3d12::PipelineCache::Pipeline);TYPE(HostTag,Cache::D3D12RenderTarget);TYPE(KeyTag,Base::RenderTargetKey);TYPE(TargetTag,Base::RenderTarget);TYPE(HeaderTag,List::CommandHeader);TYPE(CommandTag,List::Command);TYPE(BindArgsTag,List::D3DOMSetRenderTargetsArguments);
#undef TYPE
using Host=decltype(TypeOf(HostTag{}))::type;using Key=decltype(TypeOf(KeyTag{}))::type;using Target=decltype(TypeOf(TargetTag{}))::type;using Header=decltype(TypeOf(HeaderTag{}))::type;using Command=decltype(TypeOf(CommandTag{}))::type;using BindArgs=decltype(TypeOf(BindArgsTag{}))::type;
using NativePipeline=decltype(TypeOf(PipelineTag{}))::type;
struct CpCache{friend auto MemberOf(CpCache);};template struct Access<CpCache,&CP::render_target_cache_>;
struct CpPipelines{friend auto MemberOf(CpPipelines);};template struct Access<CpPipelines,&CP::pipeline_cache_>;
struct GuestPipeline{friend auto MemberOf(GuestPipeline);};template struct Access<GuestPipeline,&CP::current_guest_pipeline_>;
struct Writer{friend auto MemberOf(Writer);};template struct Access<Writer,&List::WriteCommand>;
struct Stream{friend auto MemberOf(Stream);};template struct Access<Stream,&List::command_stream_>;
struct Bind{friend auto MemberOf(Bind);};template struct Access<Bind,&Cache::SetCommandListRenderTargets>;
struct Gamma{friend auto MemberOf(Gamma);};template struct Access<Gamma,&Base::last_update_accumulated_color_targets_are_gamma_>;
struct TargetFixture:em2::pixel_test::Fixture{std::unique_ptr<Host> host;std::shared_ptr<em2::HdrSceneTarget> ambient,hdr;};
bool CompletePending(CP& cp,Cache& cache,rex::graphics::d3d12::PipelineCache& pipelines,void* pipeline,const std::shared_ptr<em2::MaterialCaptureTestGate>& gate){
    using namespace em2;auto& owner=cp.*MemberOf(CpCache{});auto saved_owner=std::move(owner);auto& compiler=cp.*MemberOf(CpPipelines{});auto saved_compiler=std::move(compiler);void* saved_pipeline=cp.*MemberOf(GuestPipeline{});auto& stream=cp.GetDeferredCommandList().*MemberOf(Stream{});auto saved_stream=std::move(stream);stream.clear();
    struct Restore{decltype(owner) owner;decltype(saved_owner)& saved;decltype(compiler) compiler;decltype(saved_compiler)& saved_compiler;CP& cp;void* pipeline;decltype(stream) stream;decltype(saved_stream)& saved_stream;Cache& cache;~Restore(){owner.release();owner=std::move(saved);compiler.release();compiler=std::move(saved_compiler);cp.*MemberOf(GuestPipeline{})=pipeline;stream=std::move(saved_stream);cache.InvalidateCommandListRenderTargets();}} restore{owner,saved_owner,compiler,saved_compiler,cp,saved_pipeline,stream,saved_stream,cache};
    owner.reset(&cache);compiler.reset(&pipelines);cp.*MemberOf(GuestPipeline{})=pipeline;Target* empty[5]{};cache.InvalidateCommandListRenderTargets();(cache.*MemberOf(Bind{}))(empty);stream.clear();
    std::atomic<bool> observed=false;std::jthread releaser([&]{observed=gate->waiting.try_acquire_for(std::chrono::seconds(5));gate->release.release();});
    const auto before=GetMaterialCaptureStats(&cache).pending;cp.GetDeferredCommandList().D3DDrawInstanced(0,1,0,0);releaser.join();
    return observed && GetMaterialCaptureStats(&cache).pending==before+1 && !pipelines.IsCreatingPipelines() && pipelines.GetD3D12PipelineByHandle(pipeline);
}
bool TestTargets(CP& cp,Cache& cache,ID3D12PipelineState* pipeline,ID3D12PipelineState* fallback,bool limited,std::weak_ptr<em2::HdrSceneTarget>& lifetime,const std::filesystem::path& output,ID3D12PipelineState* surface_pipeline=nullptr){
    using namespace em2;using namespace em2::pixel_test;
    auto f=std::make_unique<TargetFixture>();auto* device=cp.GetD3D12Provider().GetDevice();ComPtr<ID3D12InfoQueue> validation;device->QueryInterface(IID_PPV_ARGS(&validation));const auto first=validation?validation->GetNumStoredMessagesAllowedByRetrievalFilter():0;
    if(!f->event || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f->allocator))) || FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f->allocator.Get(),nullptr,IID_PPV_ARGS(&f->list))) || FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f->fence))) || !Buffer(device,3*8192,D3D12_HEAP_TYPE_READBACK,&f->readback))return false;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=desc.Height=32;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=limited?4:1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&f->target))))return false;
    auto pool=std::make_shared<rex::ui::d3d12::D3D12CpuDescriptorPool>(cp.GetD3D12Provider(),D3D12_DESCRIPTOR_HEAP_TYPE_RTV,3);auto draw=pool->AllocateDescriptor();if(!draw)return false;device->CreateRenderTargetView(f->target.Get(),nullptr,draw.GetHandle());Key key{};key.pitch_tiles_at_32bpp=1;key.resource_format=0;key.msaa_samples=limited?rex::graphics::xenos::MsaaSamples::k4X:rex::graphics::xenos::MsaaSamples::k1X;
    f->host=std::make_unique<Host>(key,f->target.Get(),std::move(draw),decltype(draw){},decltype(draw){},decltype(draw){},decltype(draw){},D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto& stream=cp.GetDeferredCommandList().*MemberOf(Stream{});auto saved=std::move(stream);stream.clear();
    struct RestoreStream{decltype(stream) stream;decltype(saved)& saved;Cache& cache;~RestoreStream(){stream=std::move(saved);cache.InvalidateCommandListRenderTargets();}} restore{stream,saved,cache};
    auto bind=[&](unsigned expected){cache.InvalidateCommandListRenderTargets();Target* targets[5]={nullptr,f->host.get(),nullptr,nullptr,nullptr};(cache.*MemberOf(Bind{}))(targets);bool found=false;size_t cursor=0;while(cursor<stream.size()){Header h;std::memcpy(&h,&stream[cursor++],sizeof(h));if(h.arguments_size_elements>stream.size()-cursor)return false;if(h.command==Command::kD3DOMSetRenderTargets){if(h.arguments_size_elements*sizeof(uintmax_t)<sizeof(BindArgs))return false;BindArgs args;std::memcpy(&args,&stream[cursor],sizeof(args));if(args.num_render_target_descriptors!=expected)return false;found=true;}cursor+=h.arguments_size_elements;}return found;};
    cache.*MemberOf(Gamma{})=0;SetMaterialCaptureDraw(&cache,nullptr);if(!bind(5))return false;
    unsigned recorded_draws=0;
    auto record_draw=[&](ID3D12PipelineState* pso,bool indexed,bool external,unsigned expected_bind){
        auto& owner=cp.*MemberOf(CpCache{});auto saved_owner=std::move(owner);const auto saved_pipeline=cp.*MemberOf(GuestPipeline{});
        struct RestoreNative{decltype(owner) owner;decltype(saved_owner)& saved;CP& cp;void* pipeline;~RestoreNative(){owner.release();owner=std::move(saved);cp.*MemberOf(GuestPipeline{})=pipeline;}} restore_native{owner,saved_owner,cp,saved_pipeline};
        owner.reset(&cache);NativePipeline native{};native.state=pso;cp.*MemberOf(GuestPipeline{})=external?nullptr:&native;
        const auto begin=stream.size();if(indexed)cp.GetDeferredCommandList().D3DDrawIndexedInstanced(0,1,0,0,0);else cp.GetDeferredCommandList().D3DDrawInstanced(0,1,0,0);
        size_t cursor=begin,draw_at=stream.size();unsigned bindings=0;
        while(cursor<stream.size()){const auto position=cursor;Header h;std::memcpy(&h,&stream[cursor++],sizeof(h));if(h.arguments_size_elements>stream.size()-cursor)return false;if(h.command==Command::kD3DOMSetRenderTargets){BindArgs args;std::memcpy(&args,&stream[cursor],sizeof(args));if(args.num_render_target_descriptors!=expected_bind)return false;++bindings;}
            if(h.command==Command::kD3DDrawInstanced || h.command==Command::kD3DDrawIndexedInstanced){if(cursor+h.arguments_size_elements!=stream.size())return false;draw_at=position;}cursor+=h.arguments_size_elements;}
        if(draw_at==stream.size() || bindings!=(expected_bind?1u:0u))return false;++recorded_draws;
        if(!external && pso==pipeline){const auto source=CaptureSceneViewTarget(cp);if(source.cache!=&cache || source.legacy!=f->target.Get())return false;}
        // Exercise the real writer/callback, but remove the zero-count draw
        // before GPU execution: this fixture checks binding/clear ownership.
        stream.resize(draw_at);return true;
    };
    const auto prepares_before=GetMaterialCaptureStats(&cache).prepares;
    if(!record_draw(pipeline,false,false,7) || !record_draw(pipeline,true,false,0) || !record_draw(pipeline,false,true,0))return false;

    auto binding=AcquireMaterialCapture(&cache,f->target.Get(),cp.GetDeferredCommandList());if(!binding.active || !binding.rtv.ptr || bool(binding.target)==limited)return false;
    const auto used=GetMaterialCaptureStats(&cache).used;auto again=AcquireMaterialCapture(&cache,f->target.Get(),cp.GetDeferredCommandList());if(binding.target!=again.target || GetMaterialCaptureStats(&cache).used!=used || bool(used)==limited)return false;
    std::shared_ptr<HdrSceneTarget> unsupported_surface;
    if(!limited && surface_pipeline){
        const D3D12_RECT full{0,0,32,32};ClearMaterialCapture(&cache,f->target.Get(),full,cp.GetDeferredCommandList());
        SetMaterialCaptureDraw(&cache,surface_pipeline);unsupported_surface=AcquireMaterialSurface(&cache,f->target.Get(),cp.GetDeferredCommandList()).target;
        if(!unsupported_surface)return false;SetMaterialCaptureDraw(&cache,pipeline);
    }
    f->ambient=binding.target;lifetime=f->ambient;f->hdr=AcquireHdrSceneBinding(&cache,f->target.Get()).target;if(!f->hdr)return false;
    SceneViewIdentity metadata;metadata.frame=cp.GetCurrentFrame();metadata.submission=cp.GetCurrentSubmission();metadata.width=metadata.height=32;const auto view=std::make_shared<const SceneViewIdentity>(metadata);
    if(BindHdrSceneView(&cache,f->target.Get(),view)==limited)return false;
    if(!limited){if(FindHdrSceneView(&cache,f->target.Get())!=view || !record_draw(pipeline,true,false,0) || FindHdrSceneView(&cache,f->target.Get()))return false;if(unsupported_surface && FindMaterialSurfaceSource(&cache,f->target.Get()).target!=unsupported_surface)return false;bool srgb=false;if(!FindHdrSceneSource(&cache,f->target.Get(),srgb).target)return false;}
    const float color[4]={.25f,.5f,2,1};const D3D12_RECT middle{8,8,24,24};
    if(f->ambient){if(!f->ambient->Clear(cp.GetDeferredCommandList(),color))return false;ClearMaterialCapture(&cache,f->target.Get(),middle,cp.GetDeferredCommandList());}
    cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();
    auto copy=[&](unsigned image){Barrier(f->list.Get(),f->ambient->texture(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=f->ambient->texture();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=image*8192;to.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,32,32,1,256};f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),f->ambient->texture(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);};
    if(f->ambient)copy(0);
    if(!record_draw(fallback,false,false,5) || GetMaterialCaptureStats(&cache).prepares!=prepares_before+(limited?3:4))return false;const D3D12_RECT corner{0,0,8,8};ClearMaterialCapture(&cache,f->target.Get(),corner,cp.GetDeferredCommandList());cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();if(f->ambient)copy(1);
    bool invalid_hdr_srgb=false;if(FindMaterialCaptureSource(&cache,f->target.Get()).target || FindHdrSceneSource(&cache,f->target.Get(),invalid_hdr_srgb).target)return false;
    const D3D12_RECT full{0,0,32,32};ClearMaterialCapture(&cache,f->target.Get(),full,cp.GetDeferredCommandList());if(FindMaterialCaptureSource(&cache,f->target.Get()).target!=f->ambient)return false;
    cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();if(f->ambient)copy(2);
    // A secondary-only write must invalidate a surface that previously had
    // valid primary-colour captures, even though colour0 is now absent.
    const float cleared_hdr[4]={};MirrorHdrSceneClear(&cache,f->target.Get(),cleared_hdr,full,cp.GetDeferredCommandList());if(!AcquireHdrSceneBinding(&cache,f->target.Get(),&cp.GetDeferredCommandList()).target)return false;
    if(!limited && !BindHdrSceneView(&cache,f->target.Get(),view))return false;
    Target* secondary[5]={nullptr,nullptr,f->host.get(),nullptr,nullptr};cache.InvalidateCommandListRenderTargets();(cache.*MemberOf(Bind{}))(secondary);
    if(!record_draw(fallback,false,false,2) || FindMaterialCaptureSource(&cache,f->target.Get()).target || FindHdrSceneSource(&cache,f->target.Get(),invalid_hdr_srgb).target || FindHdrSceneView(&cache,f->target.Get()))return false;
    Target* empty[5]{};cache.InvalidateCommandListRenderTargets();(cache.*MemberOf(Bind{}))(empty);cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();
    const bool captured=bool(f->ambient);binding.target.reset();again.target.reset();f->ambient.reset();if(captured && lifetime.expired())return false;
    if(FAILED(f->list->Close()))return false;ID3D12CommandList* lists[]={f->list.Get()};cp.GetD3D12Provider().GetDirectQueue()->ExecuteCommandLists(1,lists);
    if(FAILED(cp.GetD3D12Provider().GetDirectQueue()->Signal(f->fence.Get(),1)) || FAILED(f->fence->SetEventOnCompletion(1,f->event)) || WaitForSingleObject(f->event,15000)!=WAIT_OBJECT_0){f->ambient=lifetime.lock();f.release();return false;}
    unsigned bad=0;double error=0;if(captured){const uint8_t* bytes=nullptr;D3D12_RANGE range{0,24576};if(FAILED(f->readback->Map(0,&range,reinterpret_cast<void**>(const_cast<uint8_t**>(&bytes)))))return false;for(unsigned image=0;image<3;++image)for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x)for(unsigned c=0;c<4;++c){uint16_t bits;std::memcpy(&bits,bytes+image*8192+y*256+x*8+c*2,2);const unsigned e=(bits>>10)&31,m=bits&1023;double value=e?std::ldexp(1.+m/1024.,int(e)-15):std::ldexp(double(m),-24);if(bits&32768)value=-value;const bool cleared=image==2||(x>=8&&x<24&&y>=8&&y<24)||(image&&x<8&&y<8);const double difference=std::abs(value-(cleared?0:color[c]));error=std::max(error,difference);bad+=e==31||difference!=0;}D3D12_RANGE none{};f->readback->Unmap(0,&none);binding.target.reset();again.target.reset();f->ambient.reset();bad+=lifetime.expired();}
    const auto guard_stream_size=stream.size();const auto guard_used=GetMaterialCaptureStats(&cache).used;
    if(PrepareMaterialCaptureTransfer(&cache,f->target.Get(),f->target.Get(),cp.GetDeferredCommandList()) || PrepareMaterialCaptureTransfer(&cache,f->hdr->texture(),f->target.Get(),cp.GetDeferredCommandList()))return false;
    if(PrepareMaterialSurfaceTransfer(&cache,f->target.Get(),f->target.Get(),cp.GetDeferredCommandList()) || PrepareMaterialSurfaceTransfer(&cache,f->hdr->texture(),f->target.Get(),cp.GetDeferredCommandList()))return false;
    SetMaterialCaptureDraw(&cache,pipeline);auto invalid=AcquireMaterialCapture(&cache,f->target.Get(),cp.GetDeferredCommandList());SetMaterialCaptureDraw(&cache,nullptr);
    if(!invalid.active || invalid.target || !invalid.rtv.ptr || FindMaterialCaptureSource(&cache,f->target.Get()).target || ReserveMaterialCaptureMemory(&cache,UINT64_MAX) || stream.size()!=guard_stream_size || GetMaterialCaptureStats(&cache).used!=guard_used)return false;
    unsigned errors=0;if(validation)for(uint64_t i=first;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;validation->GetMessage(i,nullptr,&size);std::vector<uint8_t> bytes(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());if(SUCCEEDED(validation->GetMessage(i,message,&size)) && message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;REXLOG_ERROR("Material target validation: {}",message->pDescription);}}
    std::filesystem::create_directories(output);std::ofstream report(output/(limited?"null-budget-target-results.json":"material-target-results.json"));report<<"{\"transfer_guard_checks\":5,\"surface_transfer_guard_checks\":2,\"predraw_prepares\":"<<(GetMaterialCaptureStats(&cache).prepares-prepares_before)<<",\"recorded_zero_draws\":"<<recorded_draws<<",\"external_skips\":1,\"limited\":"<<(limited?"true":"false")<<",\"components\":"<<(limited?0:12288)<<",\"invalid\":"<<bad<<",\"max_error\":"<<error<<",\"device_errors\":"<<errors<<",\"scope\":\"Actual native cache seven-target HDR4/ambient5/surface6 binding, five-target detach, deferred clears and own-queue fence; 1x readback or 4x-MSAA typed-null budget fallback. Partial clears preserve surrounding FP16 values; clear while detached, reuse, bounded null fallback and cache-held lifetime. Real deferred draw writer records select/rebind before indexed/nonindexed draws, skips external passes and detaches fallback; zero-count draws removed before execution. Controlled clears, unsupported draw invalidates source; full clear restores lookup and clears all pixels. Native target capture and camera tag binding/drop checked; secondary-only writes invalidate primary sidecars and refresh framebuffer with no colour0. Recognized non-surface material draws retain the surface buffer through their per-pixel unavailable export; truly unhandled fallback and secondary writes still invalidate. No material draw or live capture.\"}\n";return !bad&&!errors&&report.good();
}
bool TestSurfaceTargets(CP& cp,Cache& cache,ID3D12PipelineState* pipeline,bool limited,std::weak_ptr<em2::HdrSceneTarget>& lifetime,const std::filesystem::path& output,ID3D12PipelineState* stale){
    using namespace em2;using namespace em2::pixel_test;
    auto f=std::make_unique<TargetFixture>();auto* device=cp.GetD3D12Provider().GetDevice();ComPtr<ID3D12InfoQueue> validation;device->QueryInterface(IID_PPV_ARGS(&validation));const auto first=validation?validation->GetNumStoredMessagesAllowedByRetrievalFilter():0;
    if(!f->event || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f->allocator))) || FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f->allocator.Get(),nullptr,IID_PPV_ARGS(&f->list))) || FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f->fence))) || !Buffer(device,3*8192,D3D12_HEAP_TYPE_READBACK,&f->readback))return false;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=desc.Height=32;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=limited?4:1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&f->target))))return false;
    auto pool=std::make_shared<rex::ui::d3d12::D3D12CpuDescriptorPool>(cp.GetD3D12Provider(),D3D12_DESCRIPTOR_HEAP_TYPE_RTV,3);auto draw=pool->AllocateDescriptor();if(!draw)return false;device->CreateRenderTargetView(f->target.Get(),nullptr,draw.GetHandle());Key key{};key.pitch_tiles_at_32bpp=1;key.resource_format=0;key.msaa_samples=limited?rex::graphics::xenos::MsaaSamples::k4X:rex::graphics::xenos::MsaaSamples::k1X;
    f->host=std::make_unique<Host>(key,f->target.Get(),std::move(draw),decltype(draw){},decltype(draw){},decltype(draw){},decltype(draw){},D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto& stream=cp.GetDeferredCommandList().*MemberOf(Stream{});auto saved=std::move(stream);stream.clear();
    struct Restore{decltype(stream) stream;decltype(saved)& saved;Cache& cache;~Restore(){stream=std::move(saved);cache.InvalidateCommandListRenderTargets();SetMaterialCaptureDraw(&cache,nullptr);}}restore{stream,saved,cache};
    if(!HasMaterialSurfaceCapture(&cache) || !SetMaterialCaptureDraw(&cache,pipeline))return false;
    auto material=AcquireMaterialCapture(&cache,f->target.Get(),cp.GetDeferredCommandList());const auto before=GetMaterialCaptureStats(&cache).used;
    if(!limited){
        const auto description=material.target->texture()->GetDesc();const auto allocation=device->GetResourceAllocationInfo(0,1,&description);
        const uint64_t reserve=128ull*1024*1024-before-allocation.SizeInBytes+1;
        if(!ReserveMaterialCaptureMemory(&cache,reserve))return false;
        const auto guard=AcquireMaterialSurface(&cache,f->target.Get(),cp.GetDeferredCommandList());
        if(!guard.active || guard.target || !guard.rtv.ptr || GetMaterialCaptureStats(&cache).used!=before+reserve)return false;
        ReleaseMaterialCaptureMemory(&cache,reserve);
    }
    auto surface=AcquireMaterialSurface(&cache,f->target.Get(),cp.GetDeferredCommandList());
    if(!surface.active || !surface.rtv.ptr || bool(surface.target)==limited || bool(material.target)==limited)return false;
    const auto after=GetMaterialCaptureStats(&cache).used;auto again=AcquireMaterialSurface(&cache,f->target.Get(),cp.GetDeferredCommandList());
    if(surface.target!=again.target || GetMaterialCaptureStats(&cache).used!=after)return false;
    if(surface.target){const auto description=surface.target->texture()->GetDesc();const auto info=device->GetResourceAllocationInfo(0,1,&description);if(after-before!=info.SizeInBytes)return false;}
    else if(after!=before)return false;
    if(!limited){
        if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&f->textures[0]))))return false;
        const auto material_destination=AcquireMaterialCapture(&cache,f->textures[0].Get(),cp.GetDeferredCommandList());if(!material_destination.target)return false;
        const auto used=GetMaterialCaptureStats(&cache).used;const uint64_t reserve=128ull*1024*1024-used-(after-before)+1;
        if(!ReserveMaterialCaptureMemory(&cache,reserve))return false;const auto commands=stream.size();
        if(PrepareMaterialSurfaceTransfer(&cache,f->target.Get(),f->textures[0].Get(),cp.GetDeferredCommandList()) || FindMaterialSurfaceSource(&cache,f->textures[0].Get()).target || stream.size()!=commands || GetMaterialCaptureStats(&cache).used!=used+reserve)return false;
        ReleaseMaterialCaptureMemory(&cache,reserve);const D3D12_RECT whole{0,0,32,32};ClearMaterialCapture(&cache,f->textures[0].Get(),whole,cp.GetDeferredCommandList());
        const auto transfer=PrepareMaterialSurfaceTransfer(&cache,f->target.Get(),f->textures[0].Get(),cp.GetDeferredCommandList());
        if(!transfer || transfer.source!=surface.target || FindMaterialSurfaceSource(&cache,f->textures[0].Get()).target!=transfer.destination || GetMaterialCaptureStats(&cache).used-used!=after-before)return false;
    }
    f->ambient=surface.target;lifetime=surface.target;
    if(stale){SetMaterialCaptureDraw(&cache,stale);const auto old=AcquireMaterialSurface(&cache,f->target.Get(),cp.GetDeferredCommandList());if(old.active || old.target || old.rtv.ptr)return false;SetMaterialCaptureDraw(&cache,pipeline);}

    Target* targets[5]={nullptr,f->host.get(),nullptr,nullptr,nullptr};cache.InvalidateCommandListRenderTargets();(cache.*MemberOf(Bind{}))(targets);
    bool seven=false;size_t cursor=0;
    while(cursor<stream.size()){Header h;std::memcpy(&h,&stream[cursor++],sizeof(h));if(h.arguments_size_elements>stream.size()-cursor)return false;if(h.command==Command::kD3DOMSetRenderTargets){BindArgs args;std::memcpy(&args,&stream[cursor],sizeof(args));seven=args.num_render_target_descriptors==7 && args.render_target_descriptors[5].ptr==material.rtv.ptr && args.render_target_descriptors[6].ptr==surface.rtv.ptr;}cursor+=h.arguments_size_elements;}
    if(!seven)return false;
    auto copy=[&](unsigned image){if(!surface.target)return;Barrier(f->list.Get(),surface.target->texture(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=surface.target->texture();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=image*8192;to.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,32,32,1,256};f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),surface.target->texture(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);};
    cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();copy(0);
    const float colour[4]={.25f,.5f,2,8};const D3D12_RECT middle{8,8,24,24},full{0,0,32,32};
    if(surface.target && (!surface.target->Clear(cp.GetDeferredCommandList(),colour) || FindMaterialSurfaceSource(&cache,f->target.Get()).target!=surface.target))return false;
    ClearMaterialCapture(&cache,f->target.Get(),middle,cp.GetDeferredCommandList());cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();copy(1);
    InvalidateMaterialSurface(&cache,f->target.Get());if(FindMaterialSurfaceSource(&cache,f->target.Get()).target || AcquireMaterialSurface(&cache,f->target.Get(),cp.GetDeferredCommandList()).target)return false;
    ClearMaterialCapture(&cache,f->target.Get(),middle,cp.GetDeferredCommandList());if(FindMaterialSurfaceSource(&cache,f->target.Get()).target)return false;
    ClearMaterialCapture(&cache,f->target.Get(),full,cp.GetDeferredCommandList());if(FindMaterialSurfaceSource(&cache,f->target.Get()).target!=surface.target)return false;
    cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();copy(2);
    Target* empty[5]{};cache.InvalidateCommandListRenderTargets();(cache.*MemberOf(Bind{}))(empty);cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();
    if(FAILED(f->list->Close()))return false;ID3D12CommandList* lists[]={f->list.Get()};cp.GetD3D12Provider().GetDirectQueue()->ExecuteCommandLists(1,lists);
    if(FAILED(cp.GetD3D12Provider().GetDirectQueue()->Signal(f->fence.Get(),1)) || FAILED(f->fence->SetEventOnCompletion(1,f->event)) || WaitForSingleObject(f->event,15000)!=WAIT_OBJECT_0){f.release();return false;}
    unsigned bad=0;if(surface.target){const uint8_t* bytes=nullptr;D3D12_RANGE range{0,24576};if(FAILED(f->readback->Map(0,&range,reinterpret_cast<void**>(const_cast<uint8_t**>(&bytes)))))return false;
        for(unsigned image=0;image<3;++image)for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x)for(unsigned c=0;c<4;++c){uint16_t bits;std::memcpy(&bits,bytes+image*8192+y*256+x*8+c*2,2);const unsigned e=(bits>>10)&31,m=bits&1023;double value=e?std::ldexp(1.+m/1024.,int(e)-15):std::ldexp(double(m),-24);if(bits&32768)value=-value;const bool absent=image!=1 || (x>=8&&x<24&&y>=8&&y<24);bad+=e==31 || value!=(absent?(c==3?-1:0):colour[c]);}
        D3D12_RANGE none{};f->readback->Unmap(0,&none);surface.target.reset();again.target.reset();f->ambient.reset();bad+=lifetime.expired();}
    unsigned errors=0;if(validation)for(uint64_t i=first;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;validation->GetMessage(i,nullptr,&size);std::vector<uint8_t> bytes(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());if(SUCCEEDED(validation->GetMessage(i,m,&size)) && m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;REXLOG_ERROR("Surface target validation: {}",m->pDescription);}}
    std::filesystem::create_directories(output);std::ofstream report(output/(limited?"surface-null-budget-results.json":"surface-target-results.json"));report<<"{\"transfer_budget_checks\":"<<(limited?0:1)<<",\"tight_budget_checks\":"<<(limited?0:1)<<",\"stale_pipeline_checks\":"<<(stale?1:0)<<",\"limited\":"<<(limited?"true":"false")<<",\"components\":"<<(limited?0:12288)<<",\"invalid\":"<<bad<<",\"device_errors\":"<<errors<<",\"surface_allocation_bytes\":"<<(after-before)<<",\"scope\":\"Native seven-target HDR4/material5/surface6 bind, shared budget including one-byte-short surface allocation rejection, old-cache pipeline tag rejection, initial invalid sentinel, reuse, partial clear, invalidation and full-clear recovery, own-queue fenced readback and cache-held lifetime.4xMSAA zero-budget typed-null path. No material draw, surface resolve or displayed lighting.\"}\n";
    return !bad && !errors && report.good();
}


}
namespace em2 {bool TestHdrGuestViewMath(rex::graphics::d3d12::D3D12CommandProcessor&,const std::filesystem::path&);bool TestHdrSceneEncoding(CP&,Cache&,const std::filesystem::path&);}
extern "C" void* EM2OriginalDeferredWriteCommand(List*,uint32_t,size_t);
bool em2::TestNativeMaterialCapture(CP& cp,const std::filesystem::path& output){
    namespace g=rex::graphics;namespace d=g::d3d12;
    auto* memory=cp.*MemberOf(Memory{});if(!memory)return false;auto* device=cp.GetD3D12Provider().GetDevice();
    struct Restore{int32_t workers=REXCVAR_GET(d3d12_pipeline_creation_threads);~Restore(){REXCVAR_SET(d3d12_pipeline_creation_threads,workers);}} restore;REXCVAR_SET(d3d12_pipeline_creation_threads,2);
    const bool prior_mode=cp.*MemberOf(BindingMode{});
    unsigned replacements=0,compiles=0;em2::pixel_test::ComPtr<ID3D12PipelineState> previous_cache_pipeline,previous_surface_pipeline;
    for(unsigned mode=0;mode<2;++mode){
        struct RestoreMode{CP& cp;bool prior;~RestoreMode(){cp.*MemberOf(BindingMode{})=prior;}} restore_mode{cp,cp.*MemberOf(BindingMode{})};cp.*MemberOf(BindingMode{})=mode!=0;
        auto registers=std::make_unique<g::RegisterFile>();std::memset(registers.get(),0,sizeof(*registers));
        registers->values[g::reg::RB_MODECONTROL::register_index]=uint32_t(g::xenos::EdramMode::kColorDepth);
        g::reg::RB_SURFACE_INFO surface{};surface.surface_pitch=32;registers->values[g::reg::RB_SURFACE_INFO::register_index]=surface.value;
        g::reg::VGT_DRAW_INITIATOR draw{};draw.prim_type=g::xenos::PrimitiveType::kTriangleList;draw.num_indices=3;registers->values[g::reg::VGT_DRAW_INITIATOR::register_index]=draw.value;
        g::reg::RB_BLENDCONTROL blend{};blend.color_srcblend=blend.alpha_srcblend=g::xenos::BlendFactor::kOne;registers->values[g::reg::RB_BLENDCONTROL::register_index]=blend.value;
        g::TraceWriter trace(memory->physical_membase());d::D3D12RenderTargetCache targets(*registers,*memory,trace,1,1,cp,mode!=0);if(!targets.Initialize() || HdrSceneCacheRegistration::Create(targets,device,256ull*1024*1024,HdrSceneEncoding::kNativeLinear) || HdrSceneCacheRegistration::Create(targets,device,0,static_cast<HdrSceneEncoding>(99)) || RegisterMaterialCapture(&targets,device,0,false,true) || !RegisterMaterialCapture(&targets,device,mode?0:128ull*1024*1024,true) || RegisterMaterialCapture(&targets,device))return false;
        // This fixture bypasses the native framebuffer update that normally
        // initializes gamma tracking. Its synthetic target has a linear RTV.
        targets.*MemberOf(Gamma{})=0;
        std::weak_ptr<HdrSceneTarget> lifetime,surface_lifetime;
        auto hdr=HdrSceneCacheRegistration::Create(targets,device,256ull*1024*1024,HdrSceneEncoding::kNativeLinear);if(!hdr)return false;
        {
            d::PipelineCache pipelines(cp,*registers,targets,mode!=0);if(!pipelines.Initialize())return false;
            constexpr uint32_t tiny_vertex[]={0,0x1001C200,0x20000000,0x000F803E,0,0x02000000};
            std::vector<uint32_t> vs(std::begin(tiny_vertex),std::end(tiny_vertex)),ps(material_shader_data::Gen3_Char_Oswald.begin(),material_shader_data::Gen3_Char_Oswald.end());for(auto& w:vs)w=std::byteswap(w);for(auto& w:ps)w=std::byteswap(w);
            auto* vertex=pipelines.LoadShader(g::xenos::ShaderType::kVertex,vs.data(),uint32_t(vs.size()));auto* pixel=pipelines.LoadShader(g::xenos::ShaderType::kPixel,ps.data(),uint32_t(ps.size()));if(!vertex || !pixel)return false;pipelines.AnalyzeShaderUcode(*vertex);pipelines.AnalyzeShaderUcode(*pixel);const auto pixel_hash=pixel->ucode_data_hash();
            auto* vt=static_cast<d::D3D12Shader::D3D12Translation*>(vertex->GetOrCreateTranslation(pipelines.GetCurrentVertexShaderModification(*vertex,g::Shader::HostVertexShaderType::kVertex,0).value));
            auto* pt=static_cast<d::D3D12Shader::D3D12Translation*>(pixel->GetOrCreateTranslation(pipelines.GetCurrentPixelShaderModification(*pixel,0,0,g::reg::RB_DEPTHCONTROL{}).value));
            g::PrimitiveProcessor::ProcessingResult shape{};shape.guest_primitive_type=shape.host_primitive_type=g::xenos::PrimitiveType::kTriangleList;shape.host_vertex_shader_type=g::Shader::HostVertexShaderType::kVertex;shape.host_draw_vertex_count=3;
            const uint32_t formats[5]={};void* handle=nullptr;ID3D12RootSignature* signature=nullptr;
            auto gate=std::make_shared<MaterialCaptureTestGate>();SetMaterialCaptureTestGate(&targets,gate);struct ReleaseGate{Cache& cache;std::shared_ptr<MaterialCaptureTestGate> gate;~ReleaseGate(){SetMaterialCaptureTestGate(&cache,{});gate->release.release();}} release_gate{targets,gate};
            const bool configured=pipelines.ConfigurePipeline(vt,pt,shape,g::reg::RB_DEPTHCONTROL{},15,2,formats,&handle,&signature);
            if(!configured || !handle || !gate->entered.try_acquire_for(std::chrono::seconds(5)) || !CompletePending(cp,targets,pipelines,handle,gate))return false;
            SetMaterialCaptureTestGate(&targets,{});SetMaterialCaptureDraw(&targets,nullptr);

            const auto probe=GetMaterialCaptureStats(&targets);REXLOG_INFO("Material hook probe mode{} configured{} busy{} handle{} root{} pipeline{} pixelValid{} translations{} replacements{} failures{}",mode,configured,pipelines.IsCreatingPipelines(),bool(handle),bool(signature),handle&&bool(pipelines.GetD3D12PipelineByHandle(handle)),pt->is_valid(),probe.translations,probe.replacements,probe.failures);
            if(!configured || pipelines.IsCreatingPipelines() || !handle || !signature || !pipelines.GetD3D12PipelineByHandle(handle) || !pt->is_valid() || ExportMaterialAmbient(pt->translated_binary()).empty())return false;
            if(pixel->ucode_data_hash()!=pixel_hash || pixel->ucode_data().size()!=material_shader_data::Gen3_Char_Oswald.size() || !std::equal(pixel->ucode_data().begin(),pixel->ucode_data().end(),material_shader_data::Gen3_Char_Oswald.begin()))return false;
            const auto stats=GetMaterialCaptureStats(&targets);if(!stats.translations || stats.replacements!=1 || stats.failures)return false;
            auto* captured_pipeline=pipelines.GetD3D12PipelineByHandle(handle);
            if(previous_cache_pipeline && SetMaterialCaptureDraw(&targets,previous_cache_pipeline.Get()))return false;
            if(!SetMaterialCaptureDraw(&targets,captured_pipeline) || SetMaterialCaptureDraw(&targets,captured_pipeline) || !SetMaterialCaptureDraw(&targets,nullptr))return false;
            ID3D12PipelineState* fallback_pipeline=nullptr;
            for(unsigned variant=0;variant<5;++variant){
                auto altered=blend;if(variant==1){altered.color_srcblend=g::xenos::BlendFactor::kSrcAlpha;altered.color_destblend=g::xenos::BlendFactor::kOneMinusSrcAlpha;}if(variant==2)altered.color_destblend=g::xenos::BlendFactor::kDstColor;if(variant==3)altered.color_destblend=g::xenos::BlendFactor::kSrcColor;if(variant==4){altered.color_srcblend=g::xenos::BlendFactor::kDstColor;altered.color_destblend=g::xenos::BlendFactor::kZero;}registers->values[g::reg::RB_BLENDCONTROL::register_index]=altered.value;
                void* fallback_handle=nullptr;ID3D12RootSignature* fallback_root=nullptr;const bool ready=pipelines.ConfigurePipeline(vt,pt,shape,g::reg::RB_DEPTHCONTROL{},variant?15:7,2,formats,&fallback_handle,&fallback_root);
                const auto stop=std::chrono::steady_clock::now()+std::chrono::seconds(30);while(pipelines.IsCreatingPipelines() && std::chrono::steady_clock::now()<stop)std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if(!ready || pipelines.IsCreatingPipelines() || !fallback_handle || !fallback_root || !pipelines.GetD3D12PipelineByHandle(fallback_handle))return false;
                auto* selected=pipelines.GetD3D12PipelineByHandle(fallback_handle);if(variant==2)fallback_pipeline=selected;
                SetMaterialCaptureDraw(&targets,nullptr);if(!SetMaterialCaptureDraw(&targets,captured_pipeline) || SetMaterialCaptureDraw(&targets,selected)!=(variant==2) || SetMaterialCaptureDraw(&targets,selected))return false;
            }
            registers->values[g::reg::RB_BLENDCONTROL::register_index]=blend.value;
            if(GetMaterialCaptureStats(&targets).fallbacks!=5 || GetMaterialCaptureStats(&targets).clear_pipelines!=4)return false;
            std::vector<uint32_t> ordinary(material_shader_data::Gen3_Char_Oswald_LOD0.begin(),material_shader_data::Gen3_Char_Oswald_LOD0.end());for(auto& word:ordinary)word=std::byteswap(word);auto* ordinary_pixel=pipelines.LoadShader(g::xenos::ShaderType::kPixel,ordinary.data(),uint32_t(ordinary.size()));if(!ordinary_pixel)return false;pipelines.AnalyzeShaderUcode(*ordinary_pixel);auto* ordinary_translation=static_cast<d::D3D12Shader::D3D12Translation*>(ordinary_pixel->GetOrCreateTranslation(pipelines.GetCurrentPixelShaderModification(*ordinary_pixel,0,0,g::reg::RB_DEPTHCONTROL{}).value));void* ordinary_handle=nullptr;ID3D12RootSignature* ordinary_root=nullptr;
            if(!pipelines.ConfigurePipeline(vt,ordinary_translation,shape,g::reg::RB_DEPTHCONTROL{},15,2,formats,&ordinary_handle,&ordinary_root))return false;pipelines.EndSubmission();SetMaterialCaptureDraw(&targets,nullptr);if(!ordinary_handle || !ordinary_root || !SetMaterialCaptureDraw(&targets,pipelines.GetD3D12PipelineByHandle(ordinary_handle)) || GetMaterialCaptureStats(&targets).clear_pipelines!=5)return false;SetMaterialCaptureDraw(&targets,nullptr);
            std::vector<uint32_t> fog_vs(scene_shader_data::depth_fog_vertex_linked_example.begin(),scene_shader_data::depth_fog_vertex_linked_example.end()),fog_ps(scene_shader_data::ScreenSpace_DepthFog_0.begin(),scene_shader_data::ScreenSpace_DepthFog_0.end());for(auto& word:fog_vs)word=std::byteswap(word);for(auto& word:fog_ps)word=std::byteswap(word);
            auto* fog_vertex=pipelines.LoadShader(g::xenos::ShaderType::kVertex,fog_vs.data(),uint32_t(fog_vs.size()));auto* fog_pixel=pipelines.LoadShader(g::xenos::ShaderType::kPixel,fog_ps.data(),uint32_t(fog_ps.size()));if(!fog_vertex || !fog_pixel)return false;pipelines.AnalyzeShaderUcode(*fog_vertex);pipelines.AnalyzeShaderUcode(*fog_pixel);
            auto* fog_vt=static_cast<d::D3D12Shader::D3D12Translation*>(fog_vertex->GetOrCreateTranslation(pipelines.GetCurrentVertexShaderModification(*fog_vertex,g::Shader::HostVertexShaderType::kVertex,0).value));auto* fog_pt=static_cast<d::D3D12Shader::D3D12Translation*>(fog_pixel->GetOrCreateTranslation(pipelines.GetCurrentPixelShaderModification(*fog_pixel,0,0,g::reg::RB_DEPTHCONTROL{}).value));
            auto fog_blend=blend;fog_blend.color_srcblend=fog_blend.alpha_srcblend=g::xenos::BlendFactor::kSrcAlpha;fog_blend.color_destblend=fog_blend.alpha_destblend=g::xenos::BlendFactor::kOneMinusSrcAlpha;registers->values[g::reg::RB_BLENDCONTROL::register_index]=fog_blend.value;void* fog_handle=nullptr;ID3D12RootSignature* fog_root=nullptr;
            if(!pipelines.ConfigurePipeline(fog_vt,fog_pt,shape,g::reg::RB_DEPTHCONTROL{},15,2,formats,&fog_handle,&fog_root))return false;pipelines.EndSubmission();SetMaterialCaptureDraw(&targets,nullptr);if(!fog_handle || !fog_root || !SetMaterialCaptureDraw(&targets,pipelines.GetD3D12PipelineByHandle(fog_handle)) || GetMaterialCaptureStats(&targets).clear_pipelines!=6)return false;SetMaterialCaptureDraw(&targets,nullptr);registers->values[g::reg::RB_BLENDCONTROL::register_index]=blend.value;
            {MaterialPipelineScope scope(&targets);const auto& code=fog_pt->translated_binary();if(!CurrentMaterialSurfacePreserving({code.data(),code.size()}))return false;auto unrelated=code;unrelated.back()^=1;if(CurrentMaterialSurfacePreserving({unrelated.data(),unrelated.size()}))return false;const auto& other=ordinary_translation->translated_binary();if(CurrentMaterialSurfacePreserving({other.data(),other.size()}))return false;}
            REXLOG_INFO("Original depth-fog vertex/pixel pair supports native-linear HDR plus ambient attenuation in mode{}; PSO only",mode);
            std::vector<uint32_t> mickey_vs(material_shader_data::mickey_full_vertex_original.begin(),material_shader_data::mickey_full_vertex_original.end()),mickey_ps(material_shader_data::Gen3_Char_Mickey.begin(),material_shader_data::Gen3_Char_Mickey.end());for(auto& word:mickey_vs)word=std::byteswap(word);for(auto& word:mickey_ps)word=std::byteswap(word);
            auto* mickey_vertex=pipelines.LoadShader(g::xenos::ShaderType::kVertex,mickey_vs.data(),uint32_t(mickey_vs.size()));auto* mickey_pixel=pipelines.LoadShader(g::xenos::ShaderType::kPixel,mickey_ps.data(),uint32_t(mickey_ps.size()));if(!mickey_vertex || !mickey_pixel || mickey_pixel==pixel)return false;pipelines.AnalyzeShaderUcode(*mickey_vertex);pipelines.AnalyzeShaderUcode(*mickey_pixel);
            auto* mickey_vt=static_cast<d::D3D12Shader::D3D12Translation*>(mickey_vertex->GetOrCreateTranslation(pipelines.GetCurrentVertexShaderModification(*mickey_vertex,g::Shader::HostVertexShaderType::kVertex,0).value));auto* mickey_pt=static_cast<d::D3D12Shader::D3D12Translation*>(mickey_pixel->GetOrCreateTranslation(pipelines.GetCurrentPixelShaderModification(*mickey_pixel,0,0,g::reg::RB_DEPTHCONTROL{}).value));void* mickey_handle=nullptr;ID3D12RootSignature* mickey_root=nullptr;
            if(!pipelines.ConfigurePipeline(mickey_vt,mickey_pt,shape,g::reg::RB_DEPTHCONTROL{},15,2,formats,&mickey_handle,&mickey_root))return false;pipelines.EndSubmission();SetMaterialCaptureDraw(&targets,nullptr);
            if(!mickey_handle || !mickey_root || !mickey_vt->is_valid() || !mickey_pt->is_valid() || ExportMaterialAmbient(mickey_pt->translated_binary()).empty() || !SetMaterialCaptureDraw(&targets,pipelines.GetD3D12PipelineByHandle(mickey_handle)) || GetMaterialCaptureStats(&targets).replacements!=2 || GetMaterialCaptureStats(&targets).failures || mickey_pixel->ucode_data().size()!=material_shader_data::Gen3_Char_Mickey.size() || !std::equal(mickey_pixel->ucode_data().begin(),mickey_pixel->ucode_data().end(),material_shader_data::Gen3_Char_Mickey.begin()))return false;SetMaterialCaptureDraw(&targets,nullptr);
            std::vector<uint32_t> world_vs(material_shader_data::world_vertex_original.begin(),material_shader_data::world_vertex_original.end()),world_ps(material_shader_data::Gen3_StaticEnv_DL.begin(),material_shader_data::Gen3_StaticEnv_DL.end());for(auto& word:world_vs)word=std::byteswap(word);for(auto& word:world_ps)word=std::byteswap(word);
            auto* world_vertex=pipelines.LoadShader(g::xenos::ShaderType::kVertex,world_vs.data(),uint32_t(world_vs.size()));auto* world_pixel=pipelines.LoadShader(g::xenos::ShaderType::kPixel,world_ps.data(),uint32_t(world_ps.size()));if(!world_vertex || !world_pixel)return false;pipelines.AnalyzeShaderUcode(*world_vertex);pipelines.AnalyzeShaderUcode(*world_pixel);
            auto* world_vt=static_cast<d::D3D12Shader::D3D12Translation*>(world_vertex->GetOrCreateTranslation(pipelines.GetCurrentVertexShaderModification(*world_vertex,g::Shader::HostVertexShaderType::kVertex,0).value));auto* world_pt=static_cast<d::D3D12Shader::D3D12Translation*>(world_pixel->GetOrCreateTranslation(pipelines.GetCurrentPixelShaderModification(*world_pixel,0,0,g::reg::RB_DEPTHCONTROL{}).value));void* world_handle=nullptr;ID3D12RootSignature* world_root=nullptr;
            if(!pipelines.ConfigurePipeline(world_vt,world_pt,shape,g::reg::RB_DEPTHCONTROL{},15,2,formats,&world_handle,&world_root))return false;pipelines.EndSubmission();SetMaterialCaptureDraw(&targets,nullptr);
            if(!world_handle || !world_root || !world_vt->is_valid() || !world_pt->is_valid() || ExportMaterialAmbient(world_pt->translated_binary()).empty() || !SetMaterialCaptureDraw(&targets,pipelines.GetD3D12PipelineByHandle(world_handle)) || GetMaterialCaptureStats(&targets).replacements!=3 || GetMaterialCaptureStats(&targets).failures || world_pixel->ucode_data().size()!=material_shader_data::Gen3_StaticEnv_DL.size() || !std::equal(world_pixel->ucode_data().begin(),world_pixel->ucode_data().end(),material_shader_data::Gen3_StaticEnv_DL.begin()))return false;SetMaterialCaptureDraw(&targets,nullptr);
            REXLOG_INFO("Original standard environment vertex/pixel pair supports native-linear HDR and baked residual capture in mode{}",mode);
            std::vector<uint32_t> base_world_vs(material_shader_data::world_base_vertex_original.begin(),material_shader_data::world_base_vertex_original.end()),base_world_ps(material_shader_data::Gen3_StaticEnv.begin(),material_shader_data::Gen3_StaticEnv.end());for(auto& word:base_world_vs)word=std::byteswap(word);for(auto& word:base_world_ps)word=std::byteswap(word);
            auto* base_world_vertex=pipelines.LoadShader(g::xenos::ShaderType::kVertex,base_world_vs.data(),uint32_t(base_world_vs.size()));auto* base_world_pixel=pipelines.LoadShader(g::xenos::ShaderType::kPixel,base_world_ps.data(),uint32_t(base_world_ps.size()));if(!base_world_vertex || !base_world_pixel)return false;pipelines.AnalyzeShaderUcode(*base_world_vertex);pipelines.AnalyzeShaderUcode(*base_world_pixel);
            auto* base_world_vt=static_cast<d::D3D12Shader::D3D12Translation*>(base_world_vertex->GetOrCreateTranslation(pipelines.GetCurrentVertexShaderModification(*base_world_vertex,g::Shader::HostVertexShaderType::kVertex,0).value));auto* base_world_pt=static_cast<d::D3D12Shader::D3D12Translation*>(base_world_pixel->GetOrCreateTranslation(pipelines.GetCurrentPixelShaderModification(*base_world_pixel,0,0,g::reg::RB_DEPTHCONTROL{}).value));void* base_world_handle=nullptr;ID3D12RootSignature* base_world_root=nullptr;
            if(!pipelines.ConfigurePipeline(base_world_vt,base_world_pt,shape,g::reg::RB_DEPTHCONTROL{},15,2,formats,&base_world_handle,&base_world_root))return false;pipelines.EndSubmission();SetMaterialCaptureDraw(&targets,nullptr);
            if(!base_world_handle || !base_world_root || !base_world_vt->is_valid() || !base_world_pt->is_valid() || ExportMaterialLightingSurface(base_world_pt->translated_binary()).empty() || !SetMaterialCaptureDraw(&targets,pipelines.GetD3D12PipelineByHandle(base_world_handle)) || GetMaterialCaptureStats(&targets).replacements!=4 || GetMaterialCaptureStats(&targets).failures || base_world_pixel->ucode_data().size()!=material_shader_data::Gen3_StaticEnv.size() || !std::equal(base_world_pixel->ucode_data().begin(),base_world_pixel->ucode_data().end(),material_shader_data::Gen3_StaticEnv.begin()))return false;SetMaterialCaptureDraw(&targets,nullptr);
            REXLOG_INFO("Original base environment vertex/pixel pair supports baked residual capture in mode{}",mode);
            replacements+=unsigned(GetMaterialCaptureStats(&targets).replacements);
            REXLOG_INFO("Original full Mickey vertex/pixel pair supports native-linear HDR and material residual capture in mode{}; original shader identities preserved",mode);
            previous_cache_pipeline=captured_pipeline;
            if(!TestSurfaceTargets(cp,targets,pipelines.GetD3D12PipelineByHandle(base_world_handle),mode!=0,surface_lifetime,output,previous_surface_pipeline.Get()))return false;
            if(hdr->stats().native_linear_builds!=11)return false;if(!TestTargets(cp,targets,captured_pipeline,fallback_pipeline,mode!=0,lifetime,output,pipelines.GetD3D12PipelineByHandle(base_world_handle)))return false;if(GetMaterialCaptureStats(&targets).unsafe_draws!=1)return false;if(!mode && !TestHdrSceneEncoding(cp,targets,output))return false;compiles+=unsigned(GetMaterialCaptureStats(&targets).translations);previous_surface_pipeline=pipelines.GetD3D12PipelineByHandle(base_world_handle);
            REXLOG_INFO("Native material hook mode{}: translations{}, replacement{}, original ucode preserved, native HDR+ambient PSO ready",mode,stats.translations,stats.replacements);
        }
        targets.Shutdown();if(!lifetime.expired() || !surface_lifetime.expired())return false;
    }
    if((cp.*MemberOf(BindingMode{}))!=prior_mode || !TestHdrGuestViewMath(cp,output/"guest-view"))return false;
    List scratch(cp);auto& commands=scratch.*MemberOf(Stream{});constexpr unsigned iterations=131072,rounds=8;commands.reserve(iterations*2);std::array<std::array<double,rounds>,2> timings{};
    for(unsigned round=0;round<rounds;++round)for(unsigned order=0;order<2;++order){const unsigned mode=(round+order)&1;commands.clear();const auto begin=std::chrono::steady_clock::now();for(unsigned i=0;i<iterations;++i){if(mode)(scratch.*MemberOf(Writer{}))(Command::kD3DOMSetStencilRef,sizeof(uint32_t));else EM2OriginalDeferredWriteCommand(&scratch,uint32_t(Command::kD3DOMSetStencilRef),sizeof(uint32_t));}timings[mode][round]=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-begin).count()/iterations;}
    for(auto& samples:timings)std::sort(samples.begin(),samples.end());std::filesystem::create_directories(output);std::ofstream timing(output/"default-writer-timing.json");timing<<"{\"iterations_per_round\":"<<iterations<<",\"rounds\":"<<rounds<<",\"native_ns\":"<<timings[0][rounds/2]<<",\"bridge_ns\":"<<timings[1][rounds/2]<<",\"scope\":\"Median interleaved CPU-only native versus disabled-bridge command writes into reserved scratch stream. No GPU submission or game FPS claim.\"}\n";if(!timing.good())return false;

    std::filesystem::create_directories(output);std::ofstream report(output/"native-material-hook-results.json");report<<"{\"modes\":2,\"native_linear_pipelines\":22,\"surface_modes\":2,\"pending_waits\":2,\"fallbacks\":10,\"clear_pipelines\":12,\"unsafe_draws\":2,\"translations\":"<<compiles<<",\"replacements\":"<<replacements<<",\"scope\":\"Actual pinned native pipeline-cache translation calls and worker PSO creation, bindful/bindless, HDR plus private ambient target5; original full Mickey/Oswald shader identities/ucode and binding layouts preserved. Character, base environment and dynamic-light environment original vertex/pixel pairs create capture PSOs; base environment uses combined private surface6 in both modes; pixel substitution uses an exact full-code whitelist. Masked/alpha original-shader fallbacks now write private zero ambient; ordinary LOD shader also receives zero export. Both HDR source- and destination-colour multiplication preserve capture; destination-colour factor detaches/invalidate. Six auxiliary pipelines (including original depth-fog vertex/pixel pair with alpha blend) and five fallbacks per mode; actual PSO tags select/detach capture and reject a PSO retained from the previous cache. Deterministic real worker gate verifies native pre-draw completion of asynchronous PSOs in both modes. No material draw or live game activation; separate native-target clear/readback and budget-fallback tests run alongside this PSO check.\"}\n";return report.good();
}

#include "renderer_graph.h"
#include "hdr_scene_cache.h"
#include "material_capture.h"
#include "hdr_presenter.h"
#include "renderer_colour_mode.h"
#include "scene_lighting.h"
#include "scene_lighting_settings.h"
#include <rex/graphics/graphics_system.h>
#include "gpu_constants_internal.h"
#include "gpu_sync.h"
#include "renderer_storage.h"
#include "native_pixel_fixture.h"
#include "shadow_kernel.h"
#include "bloom_kernel.h"
#include "../private/shadow_shader_test_data.h"
#include "../private/bloom_shader_test_data.h"
#include <rex/graphics/flags.h>
#include <rex/graphics/trace_writer.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <thread>
#include <fstream>
#include <type_traits>
namespace {
using CP=rex::graphics::d3d12::D3D12CommandProcessor;
using Base=rex::graphics::CommandProcessor;
template<class Tag,auto Member>struct Access {friend auto MemberOf(Tag){return Member;}};
struct RegistryTag{friend auto MemberOf(RegistryTag);};template struct Access<RegistryTag,&rex::graphics::d3d12::PipelineCache::shaders_>;
struct RegsTag{friend auto MemberOf(RegsTag);};template struct Access<RegsTag,&Base::register_file_>;
struct GraphicsTag{friend auto MemberOf(GraphicsTag);};template struct Access<GraphicsTag,&Base::graphics_system_>;
struct MemoryTag{friend auto MemberOf(MemoryTag);};template struct Access<MemoryTag,&Base::memory_>;
struct RenderTag{friend auto MemberOf(RenderTag);};template struct Access<RenderTag,&CP::render_target_cache_>;
template<class Tag,class T>struct TypeAccess {friend auto TypeOf(Tag){return std::type_identity<T>{};}};
struct NativeKeyTag{friend auto TypeOf(NativeKeyTag);};template struct TypeAccess<NativeKeyTag,rex::graphics::RenderTargetCache::RenderTargetKey>;
struct NativeTargetTag{friend auto TypeOf(NativeTargetTag);};template struct TypeAccess<NativeTargetTag,rex::graphics::d3d12::D3D12RenderTargetCache::D3D12RenderTarget>;
struct CreateTargetTag{friend auto MemberOf(CreateTargetTag);};template struct Access<CreateTargetTag,&rex::graphics::RenderTargetCache::GetOrCreateRenderTarget>;
struct OwnTag{friend auto MemberOf(OwnTag);};template struct Access<OwnTag,&rex::graphics::RenderTargetCache::ChangeOwnership>;
struct BeginTag{friend auto MemberOf(BeginTag);};template struct Access<BeginTag,&CP::BeginSubmission>;
struct PipelineTag{friend auto MemberOf(PipelineTag);};template struct Access<PipelineTag,&CP::pipeline_cache_>;
struct FailedPipelinesTag{friend auto MemberOf(FailedPipelinesTag);};template struct Access<FailedPipelinesTag,&rex::graphics::d3d12::PipelineCache::pipelines_>;
struct ModeTag{friend auto MemberOf(ModeTag);};template struct Access<ModeTag,&CP::bindless_resources_used_>;
constexpr std::array<uint32_t,6> vertex={0,0x1001C200,0x20000000,0x000F803E,0,0x02000000};
constexpr std::array<uint32_t,6> pixel={0,0x1001C400,0x20000000,0x000F8000,0,0x02000000};
void InjectFailedPipeline(rex::graphics::d3d12::PipelineCache& cache) {
    auto& entries=cache.*MemberOf(FailedPipelinesTag{});
    using Pipeline=std::remove_pointer_t<std::remove_reference_t<decltype(entries)>::mapped_type>;
    uint64_t key=UINT64_MAX;while(entries.contains(key))--key;
    auto failed=std::make_unique<Pipeline>();failed->state=nullptr;
    entries.emplace(key,failed.get());failed.release();
}
}
bool em2::TestRendererGraph(Em2CommandProcessor& cp,const std::filesystem::path& directory) {
    namespace g=rex::graphics;namespace d=g::d3d12;
    if(!cp.IsGraphicsWorker() || !cp.DrainForResourceChange())return false;
    const auto shadow=NativeShadowSamples(),bloom=NativeBloomSampling();const auto workers=REXCVAR_GET(d3d12_pipeline_creation_threads);
    auto* registers=cp.*MemberOf(RegsTag{});auto saved=std::make_unique<g::RegisterFile>(*registers);
    struct Restore {g::RegisterFile& regs;std::unique_ptr<g::RegisterFile>& saved;unsigned shadow,bloom;int workers;
        ~Restore(){regs=*saved;ConfigureShadowKernel(shadow);ConfigureBloomSampling(bloom);REXCVAR_SET(d3d12_pipeline_creation_threads,workers);}}restore{*registers,saved,shadow,bloom,workers};
    registers->values[g::reg::RB_MODECONTROL::register_index]=uint32_t(g::xenos::EdramMode::kColorDepth);
    g::reg::RB_SURFACE_INFO surface{};surface.surface_pitch=32;registers->values[g::reg::RB_SURFACE_INFO::register_index]=surface.value;
    g::reg::VGT_DRAW_INITIATOR draw{};draw.prim_type=g::xenos::PrimitiveType::kTriangleList;draw.num_indices=3;
    registers->values[g::reg::VGT_DRAW_INITIATOR::register_index]=draw.value;
    PipelineShaderSnapshot primary;if(!CapturePipelineShaders(cp,primary))return false;
    auto regs=std::make_unique<g::RegisterFile>(*registers);auto* memory=cp.*MemberOf(MemoryTag{});
    auto trace=std::make_unique<g::TraceWriter>(memory->physical_membase());const bool mode=cp.*MemberOf(ModeTag{});
    auto targets=std::make_unique<d::D3D12RenderTargetCache>(*regs,*memory,*trace,1,1,cp,mode);
    if(!targets->Initialize())return false;
    auto source=std::make_unique<d::PipelineCache>(cp,*regs,*targets,mode);if(!source->Initialize())return false;
    std::filesystem::create_directories(directory);source->InitializeShaderStorage(directory,0x42560816,true);
    auto load=[&](g::xenos::ShaderType type,std::span<const uint32_t> code) {
        std::vector<uint32_t> big(code.begin(),code.end());for(auto& word:big)word=std::byteswap(word);
        return source->LoadShader(type,big.data(),uint32_t(big.size()));
    };
    ConfigureShadowKernel(8);ConfigureBloomSampling(2);
    auto* vs=load(g::xenos::ShaderType::kVertex,vertex);auto* ps=load(g::xenos::ShaderType::kPixel,pixel);
    auto* game_vs=load(g::xenos::ShaderType::kVertex,shadow_test_data::vertex);
    auto* game_ps=load(g::xenos::ShaderType::kPixel,shadow_test_data::original);
    if(!vs || !ps || !game_vs || !game_ps || !load(g::xenos::ShaderType::kPixel,bloom_test_data::horizontal_original))return false;
    const auto game_vs_hash=game_vs->ucode_data_hash(),game_ps_hash=game_ps->ucode_data_hash();
    source->AnalyzeShaderUcode(*vs);source->AnalyzeShaderUcode(*ps);
    PipelineShaderSnapshot snapshot;if(!CapturePipelineShaders(cp,*source,*targets,vs,ps,snapshot) || snapshot.shaders.size()!=5 ||
       snapshot.storage_root!=directory || snapshot.storage_title!=0x42560816)return false;
    auto retained=snapshot;
    if(CapturePipelineShaders(cp,*source,*targets,ps,ps,snapshot) || snapshot.active_vertex!=retained.active_vertex || snapshot.shaders.size()!=5)return false;
    ConfigureShadowKernel(16);ConfigureBloomSampling(0);REXCVAR_SET(d3d12_pipeline_creation_threads,2);
        auto configure=[&](d::PipelineCache& cache,d::D3D12Shader* vertex_shader,d::D3D12Shader* pixel_shader,uint32_t mask,uint32_t bound) {
            cache.AnalyzeShaderUcode(*vertex_shader);cache.AnalyzeShaderUcode(*pixel_shader);
            auto* vt=static_cast<d::D3D12Shader::D3D12Translation*>(vertex_shader->GetOrCreateTranslation(
                cache.GetCurrentVertexShaderModification(*vertex_shader,g::Shader::HostVertexShaderType::kVertex,0).value));
            auto* pt=static_cast<d::D3D12Shader::D3D12Translation*>(pixel_shader->GetOrCreateTranslation(
                cache.GetCurrentPixelShaderModification(*pixel_shader,0,0,g::reg::RB_DEPTHCONTROL{}).value));
            g::PrimitiveProcessor::ProcessingResult primitive{};primitive.guest_primitive_type=primitive.host_primitive_type=g::xenos::PrimitiveType::kTriangleList;
            primitive.host_vertex_shader_type=g::Shader::HostVertexShaderType::kVertex;primitive.host_draw_vertex_count=3;
            const uint32_t formats[5]={};void* handle=nullptr;ID3D12RootSignature* signature=nullptr;
            if(!cache.ConfigurePipeline(vt,pt,primitive,g::reg::RB_DEPTHCONTROL{},mask,bound,formats,&handle,&signature) || !handle || !signature)return false;
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(cache.IsCreatingPipelines() && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return !cache.IsCreatingPipelines() && cache.GetD3D12PipelineByHandle(handle) && vt->is_valid() && pt->is_valid();
        };
    unsigned tested=0;
    for(const auto scale:std::array<std::array<uint32_t,2>,4>{{{1,1},{2,2},{3,3},{2,3}}}) {
        GraphicsResourceCheckpoint checkpoint(cp);if(!checkpoint.valid())return false;
        RendererGraph graph;
        if(!BuildRendererGraph(cp,checkpoint,snapshot,scale[0],scale[1],graph) || !graph.active_vertex || !graph.active_pixel ||
           graph.storage_root!=directory || graph.storage_title!=0x42560816 || graph.targets->draw_resolution_scale_x()!=scale[0] ||
           graph.textures->draw_resolution_scale_y()!=scale[1])return false;
        if(!tested && !BuildRendererGraph(cp,checkpoint,snapshot,scale[0],scale[1],graph))return false; // Replace an initialized graph in dependency order.
        if(source) {
            if(graph.active_vertex==vs || graph.active_pixel==ps)return false;
            InjectFailedPipeline(*source);
            source.reset();targets.reset();trace.reset();regs.reset();
        }
        PipelineShaderSnapshot cloned;
        if(!CapturePipelineShaders(cp,*graph.pipelines,*graph.targets,graph.active_vertex,graph.active_pixel,cloned) ||
           !cloned.storage_root.empty() || cloned.storage_title || cloned.active_vertex!=snapshot.active_vertex ||
           cloned.active_pixel!=snapshot.active_pixel || cloned.shaders.size()!=snapshot.shaders.size())return false;
        for(size_t i=0;i<snapshot.shaders.size();++i)if(cloned.shaders[i].type!=snapshot.shaders[i].type ||
           cloned.shaders[i].hash!=snapshot.shaders[i].hash || cloned.shaders[i].code_big_endian!=snapshot.shaders[i].code_big_endian)return false;
        auto corrupt=snapshot;corrupt.shaders.front().code_big_endian.front()^=1;
        auto* kept=graph.pipelines.get();
        if(BuildRendererGraph(cp,checkpoint,corrupt,2,2,graph) || BuildRendererGraph(cp,checkpoint,snapshot,2,1,graph) || graph.pipelines.get()!=kept)return false;
        auto& cache=*graph.pipelines;
        auto& registry=cache.*MemberOf(RegistryTag{});auto gv=registry.find(game_vs_hash),gp=registry.find(game_ps_hash);
        if(gv==registry.end() || gp==registry.end() || !configure(cache,graph.active_vertex,graph.active_pixel,0,0) ||
           !configure(cache,gv->second,gp->second,15,2) || gp->second->texture_bindings().size()!=27)return false;
        cache.EndSubmission();++tested;
    }
    {
        GraphicsResourceCheckpoint checkpoint(cp);REXLOG_INFO("HDR graph stage checkpoint={} source_mode={}",checkpoint.valid(),snapshot.hdr_scene);if(!checkpoint.valid() || snapshot.hdr_scene)return false;
        auto inconsistent=snapshot;inconsistent.material_surfaces=true;RendererGraph rejected_surface;if(BuildRendererGraph(cp,checkpoint,inconsistent,1,1,rejected_surface))return false;
        auto inconsistent_linear=snapshot;inconsistent_linear.material_surface_linear=true;RendererGraph rejected_linear;if(BuildRendererGraph(cp,checkpoint,inconsistent_linear,1,1,rejected_linear))return false;
        auto requested=snapshot;requested.hdr_scene=true;RendererGraph hdr;
        const bool built=BuildRendererGraph(cp,checkpoint,requested,1,1,hdr);REXLOG_INFO("HDR graph built={} registered={}",built,HasHdrSceneCache(hdr.targets.get()));if(!built || !HasHdrSceneCache(hdr.targets.get()))return false;
        const bool configured=configure(*hdr.pipelines,hdr.active_vertex,hdr.active_pixel,15,1);REXLOG_INFO("HDR graph pipeline configured={}",configured);if(!configured)return false;
        InjectFailedPipeline(*hdr.pipelines);
        PipelineShaderSnapshot captured;if(!CapturePipelineShaders(cp,*hdr.pipelines,*hdr.targets,hdr.active_vertex,hdr.active_pixel,captured) || !captured.hdr_scene || captured.shaders.size()!=snapshot.shaders.size())return false;
        RendererGraph resized;if(!BuildRendererGraph(cp,checkpoint,captured,2,2,resized) || !HasHdrSceneCache(resized.targets.get()) ||
           !configure(*resized.pipelines,resized.active_vertex,resized.active_pixel,15,1))return false;
        auto lighting_request=requested;lighting_request.material_capture=true;lighting_request.material_surfaces=true;lighting_request.material_surface_linear=true;lighting_request.hdr_encoding=HdrSceneEncoding::kNativeLinear;RendererGraph lighting;
        if(!BuildRendererGraph(cp,checkpoint,lighting_request,1,1,lighting) || !HasMaterialCapture(lighting.targets.get()) || GetHdrSceneEncoding(lighting.targets.get())!=HdrSceneEncoding::kNativeLinear || !configure(*lighting.pipelines,lighting.active_vertex,lighting.active_pixel,15,1))return false;
        PipelineShaderSnapshot lighting_snapshot;if(!CapturePipelineShaders(cp,*lighting.pipelines,*lighting.targets,lighting.active_vertex,lighting.active_pixel,lighting_snapshot) || !lighting_snapshot.material_capture || !lighting_snapshot.material_surfaces || !lighting_snapshot.material_surface_linear || lighting_snapshot.hdr_encoding!=HdrSceneEncoding::kNativeLinear)return false;
        RendererGraph lighting_resized;if(!BuildRendererGraph(cp,checkpoint,lighting_snapshot,2,2,lighting_resized) || !HasMaterialCapture(lighting_resized.targets.get()) || !HasMaterialSurfaceCapture(lighting_resized.targets.get()) || !HasLinearMaterialSurfaceCapture(lighting_resized.targets.get()) || GetHdrSceneEncoding(lighting_resized.targets.get())!=HdrSceneEncoding::kNativeLinear)return false;
        REXLOG_INFO("Renderer graph preserves native-linear HDR, material and surface mode through capture and1x->2x rebuild; no publication");
        PipelineShaderSnapshot native;if(!CapturePipelineShaders(cp,native) || native.hdr_scene!=primary.hdr_scene)return false;
        REXLOG_INFO("HDR renderer graph staging: register mode before pipeline workers, native PSO creation, capture and1x->2x clone preserve HDR; current renderer unchanged; no publication or scene pixels");
    }
    {
        auto* original_vertex=cp.active_vertex_shader_;auto* original_pixel=cp.active_pixel_shader_;
        auto* original_expected=cp.live_pixel_expected_;auto original_source=cp.live_pixel_source_;
        const auto original_address=cp.live_pixel_address_,original_shadow=cp.live_shadow_,original_bloom=cp.live_bloom_;
        struct RestoreLive {Em2CommandProcessor& cp;g::Shader *vertex,*pixel,*expected;std::vector<uint32_t>& source;
            uint32_t address,shadow,bloom;const PipelineShaderSnapshot& original;
            ~RestoreLive(){cp.active_vertex_shader_=vertex;cp.active_pixel_shader_=pixel;cp.live_pixel_expected_=expected;
                cp.live_pixel_source_=std::move(source);cp.live_pixel_address_=address;cp.live_shadow_=shadow;cp.live_bloom_=bloom;
                auto& cache=*(cp.*MemberOf(PipelineTag{}));cache.ShutdownShaderStorage();
                if(!original.storage_root.empty())cache.InitializeShaderStorage(original.storage_root,original.storage_title,true);
            }}restore_live{cp,original_vertex,original_pixel,original_expected,original_source,original_address,original_shadow,original_bloom,primary};
        auto& current=*(cp.*MemberOf(PipelineTag{}));current.ShutdownShaderStorage();
        const auto live_directory=directory/"publication";
        current.InitializeShaderStorage(live_directory,0x42560816,true);
        ConfigureShadowKernel(8);
        auto native_load=[&](g::xenos::ShaderType type,std::span<const uint32_t> code) {
            std::vector<uint32_t> big(code.begin(),code.end());for(auto& word:big)word=std::byteswap(word);
            return cp.LoadShader(type,0,big.data(),uint32_t(big.size()));
        };
        cp.active_vertex_shader_=native_load(g::xenos::ShaderType::kVertex,shadow_test_data::vertex);
        cp.active_pixel_shader_=native_load(g::xenos::ShaderType::kPixel,shadow_test_data::original);
        if(!cp.active_vertex_shader_ || !cp.active_pixel_shader_ || cp.live_pixel_expected_!=cp.active_pixel_shader_)return false;
        current.AnalyzeShaderUcode(*cp.active_vertex_shader_);current.AnalyzeShaderUcode(*cp.active_pixel_shader_);
        PipelineShaderSnapshot live_source;if(!CapturePipelineShaders(cp,live_source))return false;
        GraphicsResourceCheckpoint checkpoint(cp);if(!checkpoint.valid())return false;
        RendererStorageSnapshot storage;if(!CaptureRendererStorage(cp,checkpoint,storage))return false;
        RendererGraph stage,retired;ResolveRangeLedger ledger;
        struct Undo {Em2CommandProcessor& cp;const GraphicsResourceCheckpoint& checkpoint;RendererGraph& stage;RendererGraph& retired;ResolveRangeLedger& ledger;
            ~Undo(){if(retired.targets && !PublishRendererGraph(cp,checkpoint,retired,ledger,stage)) {
                retired.pipelines.release();retired.textures.release();retired.targets.release();
            }if(!cp.DrainForResourceChange()){stage.pipelines.release();stage.textures.release();stage.targets.release();}}}undo{cp,checkpoint,stage,retired,ledger};
        if(!BuildRendererGraph(cp,checkpoint,live_source,2,2,stage) || !RestoreRendererGraph(cp,checkpoint,storage,stage,ledger))return false;
        auto* old_vertex=cp.active_vertex_shader_;auto* old_pixel=cp.active_pixel_shader_;
        auto* new_vertex=stage.active_vertex;auto* new_pixel=stage.active_pixel;
        auto invalid=stage.storage_root;stage.storage_root=directory/"wrong-path";
        if(PublishRendererGraph(cp,checkpoint,stage,ledger,retired) || retired.targets || cp.active_pixel_shader_!=old_pixel)return false;
        stage.storage_root=std::move(invalid);
        if(!PublishRendererGraph(cp,checkpoint,stage,ledger,retired) || cp.active_vertex_shader_!=new_vertex ||
           cp.active_pixel_shader_!=new_pixel || new_vertex==old_vertex || new_pixel==old_pixel || cp.live_pixel_expected_!=new_pixel)return false;
        PipelineShaderSnapshot published;
        if(!CapturePipelineShaders(cp,published) || published.storage_root!=live_directory || published.storage_title!=0x42560816 ||
           published.active_vertex!=live_source.active_vertex || published.active_pixel!=live_source.active_pixel)return false;
        ConfigureShadowKernel(16);if(!cp.RefreshLivePixelShader() || cp.active_pixel_shader_==new_pixel)return false;
        ConfigureShadowKernel(8);if(!cp.RefreshLivePixelShader() || cp.active_pixel_shader_!=new_pixel)return false;
        auto& published_cache=*(cp.*MemberOf(PipelineTag{}));
        if(!configure(published_cache,static_cast<d::D3D12Shader*>(cp.active_vertex_shader_),static_cast<d::D3D12Shader*>(cp.active_pixel_shader_),15,2))return false;
        published_cache.EndSubmission();
        if(!PublishRendererGraph(cp,checkpoint,retired,ledger,stage) || cp.active_vertex_shader_!=old_vertex ||
           cp.active_pixel_shader_!=old_pixel || cp.live_pixel_expected_!=old_pixel)return false;
        if(!CapturePipelineShaders(cp,published) || published.storage_root!=live_directory || published.active_pixel!=live_source.active_pixel)return false;
        REXLOG_INFO("Renderer graph publication passed: actual processor cache/shader pointer replacement, preserved watched-ledger object with new scale, active controlled shader remapping and live8->16->8 reload, new native shadow PSO device accepted, native shader storage close/reopen handoff and reverse publication, mismatched persistence path rejection; no guest title or draw");
    }
    {
        GraphicsResourceCheckpoint pinned(cp);if(!pinned.valid() || ApplyRendererScale(cp,2)!=LiveRenderScaleStatus::Busy)return false;
    }
    // This request-path fixture owns its content, independent of earlier GPU
    // diagnostics (which may leave unowned native EDRAM).
    {
        using Key=typename decltype(TypeOf(NativeKeyTag{}))::type;
        using Target=typename decltype(TypeOf(NativeTargetTag{}))::type;
        auto& targets=*(cp.*MemberOf(RenderTag{}));Key key;
        key.pitch_tiles_at_32bpp=16;key.resource_format=uint32_t(g::xenos::ColorRenderTargetFormat::k_32_FLOAT);
        auto* target=static_cast<Target*>((targets.*MemberOf(CreateTargetTag{}))(key));
        if(!target || !(cp.*MemberOf(BeginTag{}))(false))return false;
        (targets.*MemberOf(OwnTag{}))(key,0,2048,nullptr,nullptr);
        cp.PushTransitionBarrier(target->resource(),target->SetResourceState(D3D12_RESOURCE_STATE_RENDER_TARGET),D3D12_RESOURCE_STATE_RENDER_TARGET);cp.SubmitBarriers();
        const float value[]={.8125f,0,0,1};cp.GetDeferredCommandList().D3DClearRenderTargetView(target->descriptor_draw().GetHandle(),value,0,nullptr);
        if(!cp.DrainForResourceChange())return false;
    }
    PackedEdramSnapshot before_cycle,after_cycle;
    if(!CapturePackedEdram(cp,*(cp.*MemberOf(RenderTag{})),before_cycle))return false;
    for(unsigned requested:{2u,3u,1u}) {
        QueueRendererScaleChange();if(RendererScaleStatus()!=LiveRenderScaleStatus::Queued ||
            ApplyRendererScale(cp,requested)!=LiveRenderScaleStatus::Applied || RendererScaleStatus()!=LiveRenderScaleStatus::Applied)return false;
        RendererStorageSnapshot current;
        if(!CaptureRendererStorage(cp,current) || current.packed.scale_x!=requested || current.resolved.scale_y!=requested)return false;
        if(requested==1)after_cycle=std::move(current.packed);
    }
    if(!VerifyPackedEdramSnapshots(cp,before_cycle,after_cycle,before_cycle.owners)){REXLOG_ERROR("Owned content changed across full scale request cycle");return false;}
    if(ApplyRendererScale(cp,0)!=LiveRenderScaleStatus::RestoreFailed || ApplyRendererScale(cp,1)!=LiveRenderScaleStatus::Applied)return false;
    {
        GraphicsResourceCheckpoint pinned(cp);if(!pinned.valid())return false;
        PackedEdramSnapshot a,b;auto& targets=*(cp.*MemberOf(RenderTag{}));
        if(!CapturePackedEdram(cp,targets,a) || !CapturePackedEdram(cp,targets,b))return false;
        const auto found=std::find_if(a.owners.begin(),a.owners.end(),[](const auto& owner){return owner.key && owner.end_tile-owner.begin_tile>=2;});
        if(found==a.owners.end())return false;
        auto owner=*found;owner.end_tile=owner.begin_tile+1;
        auto gap=owner;gap.begin_tile=owner.end_tile;gap.end_tile=gap.begin_tile+1;gap.key=0;
        const std::vector<PackedEdramOwner> coverage{owner,gap};
        if(!VerifyPackedEdramSnapshots(cp,a,b,coverage)){REXLOG_ERROR("Packed audit identical snapshots rejected");return false;}
        Microsoft::WRL::ComPtr<ID3D12Resource> first_word;
        if(!pixel_test::Buffer(cp.GetD3D12Provider().GetDevice(),4,D3D12_HEAP_TYPE_READBACK,&first_word) || !(cp.*MemberOf(BeginTag{}))(false))return false;
        cp.PushTransitionBarrier(a.buffer.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);cp.SubmitBarriers();
        cp.GetDeferredCommandList().D3DCopyBufferRegion(first_word.Get(),0,a.buffer.Get(),uint64_t(owner.begin_tile)*5120*a.scale_x*a.scale_y,4);
        cp.PushTransitionBarrier(a.buffer.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);cp.SubmitBarriers();
        if(!cp.DrainForResourceChange()){first_word.Detach();a.buffer.Detach();return false;}
        void* first_data=nullptr;D3D12_RANGE first_read{0,4},empty{0,0};if(FAILED(first_word->Map(0,&first_read,&first_data)))return false;
        const uint32_t changed=*static_cast<const uint32_t*>(first_data)^1u;first_word->Unmap(0,&empty);
        Microsoft::WRL::ComPtr<ID3D12Resource> zero;
        if(!pixel_test::Buffer(cp.GetD3D12Provider().GetDevice(),4,D3D12_HEAP_TYPE_UPLOAD,&zero))return false;
        void* data=nullptr;D3D12_RANGE none{0,0};if(FAILED(zero->Map(0,&none,&data)))return false;
        *static_cast<uint32_t*>(data)=changed;zero->Unmap(0,nullptr);
        auto corrupt=[&](uint32_t tile) {
            if(!(cp.*MemberOf(BeginTag{}))(false))return false;
            cp.GetDeferredCommandList().D3DCopyBufferRegion(b.buffer.Get(),uint64_t(tile)*5120*a.scale_x*a.scale_y,zero.Get(),0,4);
            if(!cp.DrainForResourceChange()){zero.Detach();b.buffer.Detach();return false;}return true;
        };
        if(!corrupt(owner.end_tile) || !VerifyPackedEdramSnapshots(cp,a,b,coverage)){REXLOG_ERROR("Packed audit rejected out-of-coverage mutation");return false;}
        if(!corrupt(owner.begin_tile) || VerifyPackedEdramSnapshots(cp,a,b,coverage)){REXLOG_ERROR("Packed audit missed owned one-bit corruption");return false;}
        auto malformed=coverage;malformed.front().end_tile=2049;
        if(VerifyPackedEdramSnapshots(cp,a,b,malformed))return false;
        REXLOG_INFO("Packed rehydration GPU audit passed: equal owned data accepts, isolated out-of-coverage corruption is ignored, one wrong owned word rejects, malformed tile bounds reject; private snapshots only, source cache unchanged");
    }
    {
        auto* graphics=cp.*MemberOf(GraphicsTag{});
        PipelineShaderSnapshot before;if(!CapturePipelineShaders(cp,before))return false;
        auto* original_target=(cp.*MemberOf(RenderTag{})).get();auto* original_presenter=graphics->presenter();unsigned exchanges=0;
        auto exchange=[&](std::unique_ptr<rex::ui::Presenter>& replacement){++exchanges;ExchangeGraphicsPresenter(*graphics,replacement);return true;};
        auto declined=[&](std::unique_ptr<rex::ui::Presenter>&){return false;};
        if(ApplyRendererColourMode(cp,!before.hdr_scene,declined)!=LiveColourModeStatus::PresenterFailed ||
           (cp.*MemberOf(RenderTag{})).get()!=original_target || graphics->presenter()!=original_presenter)return false;
        // Make publication reject an otherwise prepared transaction after the
        // presenter exchange. A real shader outside the current registry gives
        // the native publication guard a mismatched active-shader snapshot.
        d::D3D12Shader unrelated(g::xenos::ShaderType::kVertex,0,vertex.data(),vertex.size(),std::endian::native);
        auto* original_vertex=cp.active_vertex_shader_;unsigned rollback_calls=0;
        auto rollback=[&](std::unique_ptr<rex::ui::Presenter>& replacement){
            ExchangeGraphicsPresenter(*graphics,replacement);++rollback_calls;
            cp.active_vertex_shader_=rollback_calls==1?&unrelated:original_vertex;return true;
        };
        const auto rollback_status=ApplyRendererColourMode(cp,!before.hdr_scene,rollback);
        cp.active_vertex_shader_=original_vertex;
        if(rollback_status!=LiveColourModeStatus::RestoreFailed || rollback_calls!=2 ||
           (cp.*MemberOf(RenderTag{})).get()!=original_target || graphics->presenter()!=original_presenter)return false;
        for(bool hdr:{!before.hdr_scene,before.hdr_scene}){
            // Retiring a populated graph must tolerate native failed PSOs just
            // as it tolerates pending creations cancelled by worker shutdown.
            InjectFailedPipeline(*(cp.*MemberOf(PipelineTag{})));
            if(ApplyRendererColourMode(cp,hdr,exchange)!=LiveColourModeStatus::Applied)return false;
            PipelineShaderSnapshot after;if(!CapturePipelineShaders(cp,after) || after.hdr_scene!=hdr ||
               IsHdrPresenter(static_cast<rex::ui::d3d12::D3D12Presenter*>(graphics->presenter()))!=hdr ||
               after.active_vertex!=before.active_vertex || after.active_pixel!=before.active_pixel)return false;
        }
        const auto count=exchanges;if(ApplyRendererColourMode(cp,before.hdr_scene,exchange)!=LiveColourModeStatus::Applied || exchanges!=count || exchanges!=2)return false;
        REXLOG_INFO("Failed-pipeline retirement regression passed: ordinary cache, staged HDR graph and live HDR on/off graph retirement with null native PSOs; successful native PSOs retained their normal cleanup");
        REXLOG_INFO("Live colour mode transaction passed: real graph/presenter publication both directions, shader remapping, no-op, declined-host rejection and forced publication failure restore current pointers; headless host exchange, no window/UI/Present/title");
    }
    {
        using Status=SceneLightingSettingStatus;
        auto* graphics=cp.*MemberOf(GraphicsTag{});auto* original=(cp.*MemberOf(RenderTag{})).get();
        auto* display=graphics->presenter();
        if(SceneLightingEnabled() || IsHdrPresenter(static_cast<rex::ui::d3d12::D3D12Presenter*>(display)))return false;
        PipelineShaderSnapshot before;if(!CapturePipelineShaders(cp,before))return false;
        PackedEdramSnapshot packed_before,packed_after;
        if(!CapturePackedEdram(cp,*original,packed_before))return false;
        {GraphicsResourceCheckpoint pinned(cp);if(!pinned.valid() || ApplyRendererSceneLighting(cp,true)!=Status::Busy || SceneLightingEnabled() || (cp.*MemberOf(RenderTag{})).get()!=original)return false;}
        if(ApplyRendererSceneLighting(cp,true)!=Status::Applied || !SceneLightingEnabled() || graphics->presenter()!=display)return false;
        auto matches=[&](bool enabled){PipelineShaderSnapshot after;if(!CapturePipelineShaders(cp,after) || after.hdr_scene!=enabled || after.material_capture!=enabled || after.material_surfaces!=SceneIndirectEnabled() || after.material_surface_linear!=SceneIndirectEnabled() || after.hdr_encoding!=(enabled?HdrSceneEncoding::kNativeLinear:HdrSceneEncoding::kCompatibility) || after.active_vertex!=before.active_vertex || after.active_pixel!=before.active_pixel || after.shaders.size()!=before.shaders.size())return false;for(unsigned i=0;i<before.shaders.size();++i)if(after.shaders[i].type!=before.shaders[i].type || after.shaders[i].hash!=before.shaders[i].hash || after.shaders[i].code_big_endian!=before.shaders[i].code_big_endian)return false;return true;};
        if(!matches(true))return false;
        auto* enabled_cache=(cp.*MemberOf(RenderTag{})).get();
        if(ApplyRendererSceneLighting(cp,true)!=Status::Applied || (cp.*MemberOf(RenderTag{})).get()!=enabled_cache || graphics->presenter()!=display)return false;
        if(ApplyRendererSceneLighting(cp,-1)!=Status::Failed || ApplyRendererSceneLighting(cp,3)!=Status::Failed || SceneIndirectEnabled())return false;
        if(ApplyRendererSceneLighting(cp,2)!=Status::Applied || !SceneIndirectEnabled() || !matches(true) || !HasLinearMaterialSurfaceCapture((cp.*MemberOf(RenderTag{})).get()))return false;
        auto* indirect_cache=(cp.*MemberOf(RenderTag{})).get();if(ApplyRendererSceneLighting(cp,2)!=Status::Applied || (cp.*MemberOf(RenderTag{})).get()!=indirect_cache || graphics->presenter()!=display)return false;
        for(unsigned scale:{2u,1u})if(ApplyRendererScale(cp,scale)!=LiveRenderScaleStatus::Applied || !SceneLightingEnabled() || !SceneIndirectEnabled() || !HasLinearMaterialSurfaceCapture((cp.*MemberOf(RenderTag{})).get()) || !matches(true) || graphics->presenter()!=display)return false;
        unsigned exchanges=0;auto exchange=[&](std::unique_ptr<rex::ui::Presenter>& candidate){++exchanges;ExchangeGraphicsPresenter(*graphics,candidate);return true;};
        for(bool hdr:{true,false}){
            if(ApplyRendererColourMode(cp,hdr,exchange)!=LiveColourModeStatus::Applied || !SceneLightingEnabled() || !matches(true) || IsHdrPresenter(static_cast<rex::ui::d3d12::D3D12Presenter*>(graphics->presenter()))!=hdr)return false;
        }
        display=graphics->presenter();
        if(!SceneIndirectEnabled() || !HasLinearMaterialSurfaceCapture((cp.*MemberOf(RenderTag{})).get()) || ApplyRendererSceneLighting(cp,1)!=Status::Applied || SceneIndirectEnabled() || HasMaterialSurfaceCapture((cp.*MemberOf(RenderTag{})).get()) || !matches(true))return false;
        if(exchanges!=2 || ApplyRendererSceneLighting(cp,false)!=Status::Applied || SceneLightingEnabled() || graphics->presenter()!=display || !matches(false))return false;
        auto* disabled_cache=(cp.*MemberOf(RenderTag{})).get();
        if(ApplyRendererSceneLighting(cp,false)!=Status::Applied || (cp.*MemberOf(RenderTag{})).get()!=disabled_cache || !CapturePackedEdram(cp,*disabled_cache,packed_after) || !VerifyPackedEdramSnapshots(cp,packed_before,packed_after,packed_before.owners))return false;
        std::filesystem::create_directories(directory);std::ofstream report(directory/"scene-lighting-mode-results.json");report<<"{\"lighting_publications\":4,\"invalid_modes\":2,\"hdr_publications\":2,\"scale_publications\":2,\"no_ops\":3,\"busy_rejections\":1,\"owned_edram_equal\":true,\"shader_registry_equal\":true,\"scope\":\"Actual native graph capture/rebuild/restore/publication, material registration and native-linear encoding, original shader registry/active hashes, unchanged presenter for AO toggles, HDR on/off and 1x/2x/1x scale cycle retain verified linear GI surface graph; AO/GI/AO/Original cycle, native owned packed EDRAM readback equal after cycle. Headless host exchange; no game draw, menu interaction or displayed AO proof.\"}\n";if(!report.good())return false;
        REXLOG_INFO("Live scene lighting modes passed: AO/GI/AO/Original, three no-ops, two invalid modes, busy rejection, HDR/scale coexistence, exact shader registry and owned EDRAM, display retained for AO toggles; headless diagnostic");
    }
    REXLOG_INFO("Live renderer scale request path passed:queued/applied status, full1x->2x->3x->1x capture/build/restore/publication/retirement, actual cache recapture at each scale and every owned packed word compared before/after complete cycle, busy checkpoint and invalid scale rejection, same-scale no-op; no title or guest draw");
    REXLOG_INFO("Renderer graph staging passed: {} scale pairs1/2/3x and2x3, independent RT/texture/pipeline caches, exact five-shader microcode registry with active remapping, source-cache eviction before translation, preserved selected bloom/shadow code despite changed settings, no staged persistence writers, synthetic and native eight-tap shadow PSOs with two creation workers under checkpoint, malformed input/output retention; no renderer publication or pixels drawn",tested);
    return true;
}

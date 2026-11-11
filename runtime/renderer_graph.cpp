#include "renderer_graph.h"
#include "gpu_sync.h"
#include "renderer_storage.h"
#include "renderer_bindings.h"
#include "gpu_constants_internal.h"
#include "hdr_scene_cache.h"
#include "material_capture.h"
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <algorithm>
#include <bit>
#include <immintrin.h>
#include <xxhash.h>
#include <utility>
#include <atomic>
#include <mutex>
#include <type_traits>
#include <rex/cvar.h>
#include <rex/logging.h>
REXCVAR_DECLARE(int32_t,draw_resolution_scale_x);
REXCVAR_DECLARE(int32_t,draw_resolution_scale_y);
extern "C" rex::graphics::d3d12::D3D12Shader* EM2OriginalLoadShader(
    rex::graphics::d3d12::PipelineCache*,rex::graphics::xenos::ShaderType,const uint32_t*,uint32_t);
namespace {
using CP=rex::graphics::d3d12::D3D12CommandProcessor;
using Base=rex::graphics::CommandProcessor;
using PC=rex::graphics::d3d12::PipelineCache;
using RT=rex::graphics::d3d12::D3D12RenderTargetCache;
using TC=rex::graphics::d3d12::D3D12TextureCache;
template<class Tag,auto Member>struct Access {friend auto MemberOf(Tag){return Member;}};
struct PipelineTag{friend auto MemberOf(PipelineTag);};template struct Access<PipelineTag,&CP::pipeline_cache_>;
struct RegistryTag{friend auto MemberOf(RegistryTag);};template struct Access<RegistryTag,&PC::shaders_>;
struct PipelinesTag{friend auto MemberOf(PipelinesTag);};template struct Access<PipelinesTag,&PC::pipelines_>;
struct RootTag{friend auto MemberOf(RootTag);};template struct Access<RootTag,&PC::shader_storage_cache_root_>;
struct TitleTag{friend auto MemberOf(TitleTag);};template struct Access<TitleTag,&PC::shader_storage_title_id_>;
struct RenderTag{friend auto MemberOf(RenderTag);};template struct Access<RenderTag,&CP::render_target_cache_>;
struct GraphicsTag{friend auto MemberOf(GraphicsTag);};template struct Access<GraphicsTag,&Base::graphics_system_>;
struct RemovedTag{friend auto MemberOf(RemovedTag);};template struct Access<RemovedTag,&CP::device_removed_>;
struct SharedTag{friend auto MemberOf(SharedTag);};template struct Access<SharedTag,&CP::shared_memory_>;
struct ModeTag{friend auto MemberOf(ModeTag);};template struct Access<ModeTag,&CP::bindless_resources_used_>;
struct RegsTag{friend auto MemberOf(RegsTag);};template struct Access<RegsTag,&Base::register_file_>;
struct MemoryTag{friend auto MemberOf(MemoryTag);};template struct Access<MemoryTag,&Base::memory_>;
struct TraceTag{friend auto MemberOf(TraceTag);};template struct Access<TraceTag,&Base::trace_writer_>;
struct Field0{friend auto MemberOf(Field0);};template struct Access<Field0,&CP::texture_cache_>;
struct Field1{friend auto MemberOf(Field1);};template struct Access<Field1,&CP::current_guest_pipeline_>;
struct Field2{friend auto MemberOf(Field2);};template struct Access<Field2,&CP::current_external_pipeline_>;
struct Field3{friend auto MemberOf(Field3);};template struct Access<Field3,&CP::current_graphics_root_signature_>;
struct Field4{friend auto MemberOf(Field4);};template struct Access<Field4,&CP::current_graphics_root_up_to_date_>;
struct Field5{friend auto MemberOf(Field5);};template struct Access<Field5,&CP::ff_viewport_update_needed_>;
struct Field6{friend auto MemberOf(Field6);};template struct Access<Field6,&CP::ff_scissor_update_needed_>;
struct Field7{friend auto MemberOf(Field7);};template struct Access<Field7,&CP::ff_blend_factor_update_needed_>;
struct Field8{friend auto MemberOf(Field8);};template struct Access<Field8,&CP::ff_stencil_ref_update_needed_>;
struct Field9{friend auto MemberOf(Field9);};template struct Access<Field9,&CP::cbuffer_binding_system_>;
struct Field10{friend auto MemberOf(Field10);};template struct Access<Field10,&CP::cbuffer_binding_float_vertex_>;
struct Field11{friend auto MemberOf(Field11);};template struct Access<Field11,&CP::cbuffer_binding_float_pixel_>;
struct Field12{friend auto MemberOf(Field12);};template struct Access<Field12,&CP::cbuffer_binding_bool_loop_>;
struct Field13{friend auto MemberOf(Field13);};template struct Access<Field13,&CP::cbuffer_binding_fetch_>;
struct Field14{friend auto MemberOf(Field14);};template struct Access<Field14,&CP::cbuffer_binding_descriptor_indices_vertex_>;
struct Field15{friend auto MemberOf(Field15);};template struct Access<Field15,&CP::cbuffer_binding_descriptor_indices_pixel_>;
struct Field16{friend auto MemberOf(Field16);};template struct Access<Field16,&CP::current_shared_memory_binding_is_uav_>;
struct Field17{friend auto MemberOf(Field17);};template struct Access<Field17,&CP::bindful_textures_written_vertex_>;
struct Field18{friend auto MemberOf(Field18);};template struct Access<Field18,&CP::bindful_textures_written_pixel_>;
struct Field19{friend auto MemberOf(Field19);};template struct Access<Field19,&CP::bindful_samplers_written_vertex_>;
struct Field20{friend auto MemberOf(Field20);};template struct Access<Field20,&CP::bindful_samplers_written_pixel_>;
struct Field21{friend auto MemberOf(Field21);};template struct Access<Field21,&CP::current_texture_layout_uid_vertex_>;
struct Field22{friend auto MemberOf(Field22);};template struct Access<Field22,&CP::current_texture_layout_uid_pixel_>;
struct Field23{friend auto MemberOf(Field23);};template struct Access<Field23,&CP::current_sampler_layout_uid_vertex_>;
struct Field24{friend auto MemberOf(Field24);};template struct Access<Field24,&CP::current_sampler_layout_uid_pixel_>;
struct Fp {unsigned value=_mm_getcsr();~Fp(){_mm_setcsr(value);}};
bool ValidType(rex::graphics::xenos::ShaderType type) {
    return type==rex::graphics::xenos::ShaderType::kVertex || type==rex::graphics::xenos::ShaderType::kPixel;
}
}
// Called by native PipelineCache::Shutdown only after its original worker joins
// and storage shutdown. A failed native compilation explicitly leaves state=null.
// Keep deletion of the pipeline record even when it owns no COM object.
extern "C" void EM2DestroyNativePipeline(void* opaque) noexcept {
    using Registry=std::remove_reference_t<decltype(std::declval<PC&>().*MemberOf(PipelinesTag{}))>;
    using Pipeline=std::remove_pointer_t<Registry::mapped_type>;
    static_assert(sizeof(Pipeline)==0x68 && std::is_trivially_destructible_v<Pipeline>);
    auto* pipeline=static_cast<Pipeline*>(opaque);
    if(pipeline->state)pipeline->state->Release();
    delete pipeline;
}
em2::RendererGraph::RendererGraph(RendererGraph&& source) noexcept {*this=std::move(source);}
em2::RendererGraph& em2::RendererGraph::operator=(RendererGraph&& source) noexcept {
    if(this==&source)return *this;
    // Pipeline shutdown may still consult its render-target reference.
    pipelines.reset();textures.reset();targets.reset();
    targets=std::move(source.targets);textures=std::move(source.textures);pipelines=std::move(source.pipelines);
    active_vertex=std::exchange(source.active_vertex,nullptr);active_pixel=std::exchange(source.active_pixel,nullptr);
    storage_root=std::move(source.storage_root);storage_title=std::exchange(source.storage_title,0);return *this;
}
bool em2::CapturePipelineShaders(Em2CommandProcessor& cp,PipelineShaderSnapshot& output) {
    if(!cp.IsGraphicsWorker())return false;auto* source=(cp.*MemberOf(PipelineTag{})).get();
    auto* owner=(cp.*MemberOf(RenderTag{})).get();return source && owner && CapturePipelineShaders(cp,*source,*owner,cp.active_vertex_shader(),cp.active_pixel_shader(),output);
}
bool em2::CapturePipelineShaders(Em2CommandProcessor& cp,PC& source,const rex::graphics::d3d12::D3D12RenderTargetCache& owner,rex::graphics::Shader* vertex,
    rex::graphics::Shader* pixel,PipelineShaderSnapshot& output) {
    Fp fp;if(!cp.IsGraphicsWorker())return false;
    PipelineShaderSnapshot snapshot;bool vertex_found=!vertex,pixel_found=!pixel;
    snapshot.hdr_scene=HasHdrSceneCache(&owner);snapshot.material_capture=HasMaterialCapture(&owner);snapshot.material_surfaces=HasMaterialSurfaceCapture(&owner);snapshot.material_surface_linear=HasLinearMaterialSurfaceCapture(&owner);snapshot.hdr_encoding=GetHdrSceneEncoding(&owner);
    snapshot.storage_root=source.*MemberOf(RootTag{});snapshot.storage_title=source.*MemberOf(TitleTag{});
    for(const auto& [hash,shader]:source.*MemberOf(RegistryTag{})) {
        if(!shader || shader->ucode_data_hash()!=hash || !ValidType(shader->type()) ||
           !shader->ucode_dword_count() || shader->ucode_dword_count()>UINT32_MAX)return false;
        PipelineShaderRecord record{shader->type(),hash,{},shader->is_ucode_analyzed()};
        record.code_big_endian.assign(shader->ucode_dwords(),shader->ucode_dwords()+shader->ucode_dword_count());
        if constexpr(std::endian::native!=std::endian::big)for(auto& word:record.code_big_endian)word=std::byteswap(word);
        if(XXH3_64bits(record.code_big_endian.data(),record.code_big_endian.size()*4)!=hash)return false;
        if(shader==vertex){if(shader->type()!=rex::graphics::xenos::ShaderType::kVertex)return false;snapshot.active_vertex=hash;vertex_found=true;}
        if(shader==pixel){if(shader->type()!=rex::graphics::xenos::ShaderType::kPixel)return false;snapshot.active_pixel=hash;pixel_found=true;}
        snapshot.shaders.push_back(std::move(record));
    }
    if(!vertex_found || !pixel_found)return false;
    std::sort(snapshot.shaders.begin(),snapshot.shaders.end(),[](const auto& a,const auto& b){return a.hash<b.hash;});
    output=std::move(snapshot);return true;
}
bool em2::BuildRendererGraph(Em2CommandProcessor& cp,const GraphicsResourceCheckpoint& checkpoint,
    const PipelineShaderSnapshot& source,uint32_t sx,uint32_t sy,RendererGraph& output) {
    Fp fp;if((source.material_surfaces && !source.material_capture) || (source.material_surface_linear && !source.material_surfaces) || !cp.IsGraphicsWorker() || !checkpoint.owns(cp) || !sx || sx>7 || !sy || sy>7 ||
        (sx*sy!=1 && sx*sy<4))return false;
    std::optional<uint64_t> previous;bool vertex_found=!source.active_vertex,pixel_found=!source.active_pixel;
    for(const auto& record:source.shaders) {
        if(!ValidType(record.type) || record.code_big_endian.empty() || record.code_big_endian.size()>UINT32_MAX ||
           (previous && record.hash<=*previous) || XXH3_64bits(record.code_big_endian.data(),record.code_big_endian.size()*4)!=record.hash)return false;
        if(source.active_vertex==record.hash){if(record.type!=rex::graphics::xenos::ShaderType::kVertex)return false;vertex_found=true;}
        if(source.active_pixel==record.hash){if(record.type!=rex::graphics::xenos::ShaderType::kPixel)return false;pixel_found=true;}
        previous=record.hash;
    }
    if(!vertex_found || !pixel_found)return false;
    auto* regs=cp.*MemberOf(RegsTag{});auto* memory=cp.*MemberOf(MemoryTag{});auto* shared=(cp.*MemberOf(SharedTag{})).get();
    auto* current=(cp.*MemberOf(RenderTag{})).get();const bool mode=cp.*MemberOf(ModeTag{});
    if(!regs || !memory || !shared || !current)return false;
    uint32_t dx=sx,dy=sy;
    if(!TC::ClampDrawResolutionScaleToMaxSupported(dx,dy,cp.GetD3D12Provider()) || dx!=sx || dy!=sy)return false;
    RendererGraph graph;graph.storage_root=source.storage_root;graph.storage_title=source.storage_title;
    graph.targets=std::make_unique<RT>(*regs,*memory,cp.*MemberOf(TraceTag{}),sx,sy,cp,mode);
    if(!graph.targets->Initialize() || graph.targets->GetPath()!=current->GetPath())return false;
    // Mode must be registered before creating this graph's pipeline cache and
    // its background workers. Registration persists until native cache shutdown.
    if(source.material_capture && !RegisterMaterialCapture(graph.targets.get(),cp.GetD3D12Provider().GetDevice(),128ull*1024*1024,source.material_surfaces,source.material_surface_linear))return false;
    auto hdr_registration=source.hdr_scene?HdrSceneCacheRegistration::Create(*graph.targets,cp.GetD3D12Provider().GetDevice(),256ull*1024*1024,source.hdr_encoding):nullptr;
    if(source.hdr_scene && !hdr_registration)return false;
    graph.textures=TC::Create(*regs,*shared,sx,sy,cp,mode);if(!graph.textures)return false;
    graph.pipelines=std::make_unique<PC>(cp,*regs,*graph.targets,mode);if(!graph.pipelines->Initialize())return false;
    for(const auto& record:source.shaders) {
        // Preserve the exact already-selected program. Do not re-run live
        // shadow/bloom substitutions over serialized modified microcode.
        auto* shader=EM2OriginalLoadShader(graph.pipelines.get(),record.type,record.code_big_endian.data(),uint32_t(record.code_big_endian.size()));
        if(!shader || shader->type()!=record.type || shader->ucode_data_hash()!=record.hash ||
           shader->ucode_dword_count()!=record.code_big_endian.size())return false;
        if(record.analyzed || source.active_vertex==record.hash || source.active_pixel==record.hash)graph.pipelines->AnalyzeShaderUcode(*shader);
        if(source.active_vertex==record.hash)graph.active_vertex=shader;
        if(source.active_pixel==record.hash)graph.active_pixel=shader;
    }
    if(hdr_registration && !hdr_registration->KeepUntilCacheShutdown())return false;
    output=std::move(graph);return true;
}

namespace em2 {
namespace {
bool RestoreGraphStorage(Em2CommandProcessor& cp,const GraphicsResourceCheckpoint& checkpoint,
    const RendererStorageSnapshot& source,RendererGraph& graph,ResolveRangeLedger& output,RendererStorageSnapshot* consume) {
    Fp fp;if(!cp.IsGraphicsWorker() || !checkpoint.owns(cp) || !graph.targets || !graph.textures || !graph.pipelines)return false;
    const auto sx=source.packed.scale_x,sy=source.packed.scale_y;
    if(!sx || sx>7 || !sy || sy>7 || source.auxiliary.scale_x!=sx || source.auxiliary.scale_y!=sy ||
       source.resolved.scale_x!=sx || source.resolved.scale_y!=sy || source.packed.owners.size()!=source.auxiliary.owners.size())return false;
    for(size_t i=0;i<source.packed.owners.size();++i) {
        const auto& a=source.packed.owners[i];const auto& b=source.auxiliary.owners[i];
        if(a.begin_tile!=b.begin_tile || a.end_tile!=b.end_tile || a.key!=b.key || a.depth_unorm_key!=b.depth_unorm_key ||
           a.depth_float_key!=b.depth_float_key || a.pixels_are_64_bit!=b.pixels_are_64_bit || a.is_depth!=b.is_depth)return false;
    }
    const auto dx=graph.targets->draw_resolution_scale_x(),dy=graph.targets->draw_resolution_scale_y();
    if(dx!=graph.textures->draw_resolution_scale_x() || dy!=graph.textures->draw_resolution_scale_y())return false;
    EdramSystemBindings bindings(cp,checkpoint,*graph.targets);if(!bindings.valid())return false;
    ResolveRangeLedger staged;
    {
        PackedEdramSnapshot canonical,rehydrated;
        if(!RestorePackedEdram(cp,source.packed,*graph.targets))return false;
        // RestorePackedEdram has fenced its reads. Only ownership metadata is
        // needed below, so a consuming caller can release the source allocation.
        if(consume)consume->packed.buffer.Reset();
        if(!CapturePackedEdram(cp,*graph.targets,canonical) || !RestoreAuxiliaryDepth(cp,source.auxiliary,*graph.targets))return false;
        if(consume)consume->auxiliary.targets.clear();
        if(!RehydratePackedEdramOwners(cp,source.packed,*graph.targets) || !CapturePackedEdram(cp,*graph.targets,rehydrated) ||
           !VerifyPackedEdramSnapshots(cp,canonical,rehydrated,source.packed.owners))return false;
        // The GPU audit completed. Release both full EDRAM copies before
        // allocating converted resolved storage or committing texture residency.
    }
    ResolvedStorageSnapshot converted;
    if(!ConvertResolvedStorage(cp,source.resolved,dx,dy,converted))return false;
    if(consume)consume->resolved.buffers.clear();
    if(!RestoreResolvedStorage(cp,converted,*graph.textures,staged) || !bindings.Restore())return false;
    output=std::move(staged);return true;
}
}
bool RestoreRendererGraph(Em2CommandProcessor& cp,const GraphicsResourceCheckpoint& checkpoint,
    const RendererStorageSnapshot& source,RendererGraph& graph,ResolveRangeLedger& output) {
    return RestoreGraphStorage(cp,checkpoint,source,graph,output,nullptr);
}
bool RestoreRendererGraph(Em2CommandProcessor& cp,const GraphicsResourceCheckpoint& checkpoint,
    RendererStorageSnapshot&& source,RendererGraph& graph,ResolveRangeLedger& output) {
    return RestoreGraphStorage(cp,checkpoint,source,graph,output,&source);
}
} // namespace em2

bool em2::PublishRendererGraph(Em2CommandProcessor& cp,const GraphicsResourceCheckpoint& checkpoint,
    RendererGraph& stage,ResolveRangeLedger& ledger,RendererGraph& retired) {
    Fp fp;
    if(!cp.IsGraphicsWorker() || !checkpoint.owns(cp) || !stage.targets || !stage.textures || !stage.pipelines ||
       retired.targets || retired.textures || retired.pipelines || !cp.resolve_ledger_ || !cp.resolve_watch_ ||
       stage.pipelines->IsCreatingPipelines())return false;
    const auto sx=stage.targets->draw_resolution_scale_x(),sy=stage.targets->draw_resolution_scale_y();
    if(stage.textures->draw_resolution_scale_x()!=sx || stage.textures->draw_resolution_scale_y()!=sy ||
       ledger.scale_x()!=sx || ledger.scale_y()!=sy)return false;
    PipelineShaderSnapshot incoming,previous;
    std::vector<ResolvedStorageRange> ranges;
    if(!ledger.Snapshot(ranges) || !CapturePipelineShaders(cp,*stage.pipelines,*stage.targets,stage.active_vertex,stage.active_pixel,incoming) ||
       !CapturePipelineShaders(cp,previous) || incoming.active_vertex!=previous.active_vertex ||
       incoming.active_pixel!=previous.active_pixel || stage.storage_root!=previous.storage_root ||
       stage.storage_title!=previous.storage_title || !cp.DrainForResourceChange())return false;
    // Allocate/copy metadata before changing either descriptors or ownership.
    RendererGraph old;old.storage_root=previous.storage_root;old.storage_title=previous.storage_title;
    old.active_vertex=static_cast<rex::graphics::d3d12::D3D12Shader*>(cp.active_vertex_shader_);
    old.active_pixel=static_cast<rex::graphics::d3d12::D3D12Shader*>(cp.active_pixel_shader_);
    auto* old_expected=cp.live_pixel_expected_;
    const bool remap_live=old_expected && old_expected==cp.active_pixel_shader_;
    EdramSystemBindings bindings(cp,checkpoint,*stage.targets);if(!bindings.valid())return false;
    // The old writer must finish before the replacement is allowed to open the
    // same native files. PSO storage remains owned by the existing outer lease.
    (cp.*MemberOf(PipelineTag{}))->ShutdownShaderStorage();
    old.targets=std::move(cp.*MemberOf(RenderTag{}));
    cp.scene_draw_inputs_.reset();
    cp.scene_replay_.reset();
    cp.scene_lighting_.reset(); // Publish already drained the native GPU queue.
    cp.scene_lighting_reported_=false;cp.scene_lighting_failed_=false;
    cp.scene_depth_view_.reset();cp.scene_depth_view_cache_.reset();
    old.textures=std::move(cp.*MemberOf(Field0{}));
    old.pipelines=std::move(cp.*MemberOf(PipelineTag{}));
    cp.*MemberOf(RenderTag{})=std::move(stage.targets);
    cp.*MemberOf(Field0{})=std::move(stage.textures);
    cp.*MemberOf(PipelineTag{})=std::move(stage.pipelines);
    cp.active_vertex_shader_=stage.active_vertex;cp.active_pixel_shader_=stage.active_pixel;
    cp.live_pixel_expected_=remap_live?cp.active_pixel_shader_:nullptr;
    std::swap(*cp.resolve_ledger_,ledger); // Preserve the watched ledger object's address.
    if(!bindings.Commit()) {
        std::swap(cp.*MemberOf(RenderTag{}),old.targets);
        std::swap(cp.*MemberOf(Field0{}),old.textures);
        std::swap(cp.*MemberOf(PipelineTag{}),old.pipelines);
        cp.active_vertex_shader_=old.active_vertex;cp.active_pixel_shader_=old.active_pixel;
        cp.live_pixel_expected_=old_expected;std::swap(*cp.resolve_ledger_,ledger);
        stage.targets=std::move(old.targets);stage.textures=std::move(old.textures);stage.pipelines=std::move(old.pipelines);
        // A failed drain may indicate device loss. Keep both graphs; caller must
        // not resume rendering or destroy caches until completion is established.
        return false;
    }
    cp.*MemberOf(Field1{})=nullptr;
    cp.*MemberOf(Field2{})=nullptr;
    cp.*MemberOf(Field3{})=nullptr;
    cp.*MemberOf(Field4{})=0;
    cp.*MemberOf(Field5{})=true;
    cp.*MemberOf(Field6{})=true;
    cp.*MemberOf(Field7{})=true;
    cp.*MemberOf(Field8{})=true;
    (cp.*MemberOf(Field9{})).up_to_date=false;
    (cp.*MemberOf(Field10{})).up_to_date=false;
    (cp.*MemberOf(Field11{})).up_to_date=false;
    (cp.*MemberOf(Field12{})).up_to_date=false;
    (cp.*MemberOf(Field13{})).up_to_date=false;
    (cp.*MemberOf(Field14{})).up_to_date=false;
    (cp.*MemberOf(Field15{})).up_to_date=false;
    (cp.*MemberOf(Field16{})).reset();
    cp.*MemberOf(Field17{})=false;
    cp.*MemberOf(Field18{})=false;
    cp.*MemberOf(Field19{})=false;
    cp.*MemberOf(Field20{})=false;
    cp.*MemberOf(Field21{})=PC::kLayoutUIDEmpty;
    cp.*MemberOf(Field22{})=PC::kLayoutUIDEmpty;
    cp.*MemberOf(Field23{})=PC::kLayoutUIDEmpty;
    cp.*MemberOf(Field24{})=PC::kLayoutUIDEmpty;
    retired=std::move(old);
    // Native storage failure is a cache-performance issue, not a failed graph
    // publication. The SDK's initializer has no success result.
    if(!stage.storage_root.empty()) {
        try {(cp.*MemberOf(PipelineTag{}))->InitializeShaderStorage(stage.storage_root,stage.storage_title,true);}
        catch(...) {(cp.*MemberOf(PipelineTag{}))->ShutdownShaderStorage();}
    }
    stage.active_vertex=stage.active_pixel=nullptr;stage.storage_root.clear();stage.storage_title=0;
    return true;
}

namespace {
struct ScaleRequestStatus {
    using Status=em2::LiveRenderScaleStatus;
    std::mutex mutex;uint64_t revision=0;Status value=Status::Idle;
    void Queue(uint64_t request) {
        std::lock_guard lock(mutex);
        if(!request || request>revision){if(request)revision=request;value=Status::Queued;}
    }
    Status Finish(uint64_t request,Status result) {
        std::lock_guard lock(mutex);
        if(!request || request==revision)value=result;
        return result;
    }
    Status Get(){std::lock_guard lock(mutex);return value;}
}scale_status;
}
void em2::QueueRendererScaleChange(uint64_t revision){scale_status.Queue(revision);}
em2::LiveRenderScaleStatus em2::RendererScaleStatus(){return scale_status.Get();}
bool em2::TestRendererScaleRequestOrdering() {
    using Status=LiveRenderScaleStatus;ScaleRequestStatus state;
    state.Queue(1);state.Queue(2);state.Finish(1,Status::MemoryLimit);
    if(state.Get()!=Status::Queued)return false;
    state.Finish(2,Status::Applied);state.Queue(1);state.Queue(2);
    if(state.Get()!=Status::Applied)return false;
    state.Queue(3);state.Finish(2,Status::Applied);
    if(state.Get()!=Status::Queued)return false;
    state.Finish(3,Status::RestoreFailed);
    if(state.Get()!=Status::RestoreFailed)return false;
    state.Queue(4);state.Finish(3,Status::MemoryLimit);state.Finish(4,Status::Applied);
    if(state.Get()!=Status::Applied)return false;
    state.Queue(0);state.Finish(0,Status::Busy);
    if(state.Get()!=Status::Busy)return false;
    REXLOG_INFO("Live scale request status passed: stale failures/completions and late/duplicate queue events preserve the latest request; unversioned diagnostics remain supported");
    return true;
}
em2::LiveRenderScaleStatus em2::ApplyRendererScale(Em2CommandProcessor& cp,uint32_t scale,uint64_t revision) {
    Fp fp;using Status=LiveRenderScaleStatus;
    auto finish=[revision](Status result){return scale_status.Finish(revision,result);};
    if(!cp.IsGraphicsWorker() || scale<1 || scale>3)return finish(Status::RestoreFailed);
    auto* current=(cp.*MemberOf(RenderTag{})).get();
    if(!current)return finish(Status::RestoreFailed);
    if(current->draw_resolution_scale_x()==scale && current->draw_resolution_scale_y()==scale)return finish(Status::Applied);
    GraphicsResourceCheckpoint checkpoint(cp);if(!checkpoint.valid())return finish(Status::Busy);
    struct Work {RendererGraph stage,retired;RendererStorageSnapshot storage;PipelineShaderSnapshot shaders;ResolveRangeLedger ledger;};
    std::unique_ptr<Work> work;Status result=Status::CaptureFailed;
    try {
        work=std::make_unique<Work>();
        if(CaptureRendererStorage(cp,checkpoint,work->storage) && CapturePipelineShaders(cp,work->shaders)) {
            result=Status::MemoryLimit;
            if(BuildRendererGraph(cp,checkpoint,work->shaders,scale,scale,work->stage)) {
                result=Status::RestoreFailed;
                if(RestoreRendererGraph(cp,checkpoint,std::move(work->storage),work->stage,work->ledger) &&
                   PublishRendererGraph(cp,checkpoint,work->stage,work->ledger,work->retired)) {
                    REXCVAR_SET(draw_resolution_scale_x,int32_t(scale));REXCVAR_SET(draw_resolution_scale_y,int32_t(scale));
                    result=Status::Applied;
                }
            }
        }
    }catch(const std::bad_alloc&){result=Status::MemoryLimit;}
    catch(...){result=Status::RestoreFailed;}
    if(!cp.DrainForResourceChange()) {
        // Never free a cache/snapshot that an unfinished GPU command may read.
        work.release();result=Status::GpuFailure;
        cp.*MemberOf(RemovedTag{})=true;
        if(auto* graphics=cp.*MemberOf(GraphicsTag{}))graphics->OnHostGpuLossFromAnyThread(false);
    }
    REXLOG_INFO("Live internal resolution request {}x completed with status {}",scale,int(result));
    return finish(result);
}

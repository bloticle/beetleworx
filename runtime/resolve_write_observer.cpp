#include "resolve_write_observer.h"
#include "resolve_coverage.h"
#include "resolved_storage_capture.h"
#include "renderer_storage.h"
#include "gpu_sync.h"
#include "gpu_constants_internal.h"
#include <rex/graphics/d3d12/texture_cache.h>
#include <rex/graphics/d3d12/render_target_cache.h>
#include <rex/graphics/trace_writer.h>
#include <rex/logging.h>
#include <rex/cvar.h>
#include <atomic>
#include <string>
#include "native_pixel_fixture.h"
#include <rex/graphics/pipeline/texture/cache.h>
#include <immintrin.h>
#include <algorithm>
extern "C" bool EM2OriginalGetResolveInfo(const rex::graphics::RegisterFile&,const rex::memory::Memory&,
    rex::graphics::TraceWriter&,uint32_t,uint32_t,bool,bool,rex::graphics::draw_util::ResolveInfo&);
extern "C" void EM2OriginalMarkRangeAsResolved(rex::graphics::TextureCache*,uint32_t,uint32_t);
REXCVAR_DECLARE(bool,em2_trace_shadow_gpu_state);
REXCVAR_DECLARE(bool,em2_private_profile);
REXCVAR_DECLARE(std::string,em2_user_data_directory);
namespace {
thread_local em2::ResolveWriteScope* active=nullptr;
template<class Tag,auto Member>struct Access {friend auto MemberOf(Tag){return Member;}};
struct TextureTag{friend auto MemberOf(TextureTag);};
template struct Access<TextureTag,&rex::graphics::d3d12::D3D12CommandProcessor::texture_cache_>;
using CP=rex::graphics::d3d12::D3D12CommandProcessor;
using RT=rex::graphics::d3d12::D3D12RenderTargetCache;
using Base=rex::graphics::RenderTargetCache;
struct MemoryTag{friend auto MemberOf(MemoryTag);};template struct Access<MemoryTag,&rex::graphics::CommandProcessor::memory_>;
struct RenderCacheTag{friend auto MemberOf(RenderCacheTag);};template struct Access<RenderCacheTag,&CP::render_target_cache_>;
struct SharedTag{friend auto MemberOf(SharedTag);};template struct Access<SharedTag,&CP::shared_memory_>;
struct BeginTag{friend auto MemberOf(BeginTag);};template struct Access<BeginTag,&CP::BeginSubmission>;
struct CreateTag{friend auto MemberOf(CreateTag);};template struct Access<CreateTag,&Base::GetOrCreateRenderTarget>;
struct OwnTag{friend auto MemberOf(OwnTag);};template struct Access<OwnTag,&Base::ChangeOwnership>;
template<class Tag,class T>struct TypeAccess {friend auto TypeOf(Tag){return std::type_identity<T>{};}};
struct KeyTag{friend auto TypeOf(KeyTag);};template struct TypeAccess<KeyTag,Base::RenderTargetKey>;
struct TargetTag{friend auto TypeOf(TargetTag);};template struct TypeAccess<TargetTag,RT::D3D12RenderTarget>;
}
bool em2::Em2CommandProcessor::SetResolveWriteObserver(ResolveWriteObserver* observer) {
    if(!IsGraphicsWorker())return false;resolve_write_observer_=observer;return true;
}
em2::Em2CommandProcessor::~Em2CommandProcessor()=default;
bool em2::Em2CommandProcessor::SetupContext() {
    if(!D3D12CommandProcessor::SetupContext())return false;
    try {
        auto* texture=(this->*MemberOf(TextureTag{})).get();auto* shared=(this->*MemberOf(SharedTag{})).get();
        auto ledger=std::make_unique<ResolveRangeLedger>();
        if(!ledger->SeedRanges({},texture->draw_resolution_scale_x(),texture->draw_resolution_scale_y()))return true;
        auto watch=std::make_unique<ResolveLedgerWatch>(*shared,*ledger);
        if(!watch->valid())return true;
        resolve_ledger_=std::move(ledger);resolve_watch_=std::move(watch);
        REXLOG_INFO("Resolved-storage tracking attached to native renderer; geometry preparation before write, exact committed ranges and CPU invalidation; live renderer swap not installed");
    }catch(...){REXLOG_WARN("Resolved-storage tracking unavailable; renderer remains usable, live migration cannot capture metadata");}
    return true;
}
void em2::Em2CommandProcessor::ShutdownContext() {
    if(scene_lighting_ && DrainForResourceChange())scene_lighting_.reset();
    resolve_write_observer_=nullptr;resolve_watch_.reset();resolve_ledger_.reset();edram_bindings_overridden_=false;
    D3D12CommandProcessor::ShutdownContext();
    scene_lighting_.reset();
    scene_lighting_reported_=false;scene_lighting_failed_=false;
    hdr_swap_.reset();
}
bool em2::Em2CommandProcessor::CopyTrackedResolveLedger(ResolveRangeLedger& output) {
    if(!IsGraphicsWorker() || !resolve_ledger_ || !resolve_watch_)return false;
    rex::thread::global_critical_region region;auto lock=region.Acquire();
    std::vector<ResolvedStorageRange> ranges;if(!resolve_ledger_->Snapshot(ranges))return false;
    return output.SeedRanges(ranges,resolve_ledger_->scale_x(),resolve_ledger_->scale_y());
}
bool em2::Em2CommandProcessor::IssueCopy() {
    scene_draw_inputs_.reset();
    scene_replay_.reset();
    scene_depth_view_.reset();
    if(!resolve_ledger_ && !resolve_write_observer_)return D3D12CommandProcessor::IssueCopy();
    struct CopyContext {Em2CommandProcessor& cp;bool baseline_ready=false;} context{*this};
    ResolveWriteObserver combined{&context,[](void* opaque,const ResolveWriteObservation& event) noexcept {
        auto& context=*static_cast<CopyContext*>(opaque);auto& cp=context.cp;
        const auto csr=_mm_getcsr();
        if(cp.resolve_ledger_) {
            if((event.scale_x!=1 || event.scale_y!=1) && (!context.baseline_ready || !event.info)) {
                // Native scaled resolves mark entire shared-memory pages GPU
                // valid although only typed groups exist in scaled storage.
                // If their CPU baseline could not be uploaded first, holes
                // cannot safely survive a later return to1x. Block migration
                // until invalidation or complete typed overwrite repairs them.
                auto unknown=event;unknown.info=nullptr;unknown.address&=~4095u;
                const uint64_t end=std::min<uint64_t>(0x20000000,(uint64_t(event.address)+event.length+4095)&~uint64_t(4095));
                unknown.length=uint32_t(end-unknown.address);cp.resolve_ledger_->Record(unknown);
            }
            cp.resolve_ledger_->Record(event);
        }
        _mm_setcsr(csr);auto* observer=cp.resolve_write_observer_;
        if(observer && observer->written)observer->written(observer->context,event);
    },[](void* opaque,const ResolveWriteObservation& event) noexcept {
        auto& context=*static_cast<CopyContext*>(opaque);auto& cp=context.cp;
        const auto csr=_mm_getcsr();context.baseline_ready=event.scale_x==1 && event.scale_y==1;
        if(!context.baseline_ready) {
            // Before native MarkRangeAsResolved makes the whole destination
            // page GPU-valid, preserve CPU bytes in its untouched groups.
            try {auto* shared=(cp.*MemberOf(SharedTag{})).get();
                context.baseline_ready=shared && shared->RequestRange(event.address,event.length);
            }catch(...){context.baseline_ready=false;}
        }
        if(cp.resolve_ledger_)cp.resolve_ledger_->Prepare(event);_mm_setcsr(csr);
        auto* observer=cp.resolve_write_observer_;
        if(observer && observer->prepared)observer->prepared(observer->context,event);
    }};
    ResolveWriteScope scope((this->*MemberOf(TextureTag{})).get(),&combined);return D3D12CommandProcessor::IssueCopy();
}
em2::ResolveWriteScope::ResolveWriteScope(rex::graphics::TextureCache* cache,ResolveWriteObserver* observer) noexcept:
    cache_(cache),observer_(observer),previous_(active){active=this;}
em2::ResolveWriteScope::~ResolveWriteScope(){active=previous_;}
void em2::ObserveNativeResolveInfo(bool success,const rex::graphics::draw_util::ResolveInfo& info,uint32_t sx,uint32_t sy) noexcept {
    if(!active)return;
    active->valid_=success && info.coordinate_info.width_div_8 && info.height_div_8 && info.copy_dest_extent_length;
    if(active->valid_) {
        std::memcpy(&active->info_,&info,sizeof(info));active->sx_=sx;active->sy_=sy;
        if(active->observer_ && active->observer_->prepared) {
            const auto csr=_mm_getcsr();
            active->observer_->prepared(active->observer_->context,
                {active->cache_,info.copy_dest_extent_start,info.copy_dest_extent_length,sx,sy,&active->info_});
            _mm_setcsr(csr);
        }
    }
}
void em2::ObserveNativeResolveWrite(rex::graphics::TextureCache* cache,uint32_t start,uint32_t length) noexcept {
    if(!length || !active || active->cache_!=cache || !active->observer_ || !active->observer_->written)return;
    const auto csr=_mm_getcsr();
    const bool matching=active->valid_ && active->info_.copy_dest_extent_start==start && active->info_.copy_dest_extent_length==length;
    const ResolveWriteObservation event{cache,start,length,cache->draw_resolution_scale_x(),cache->draw_resolution_scale_y(),
        matching && active->sx_==cache->draw_resolution_scale_x() && active->sy_==cache->draw_resolution_scale_y()?&active->info_:nullptr};
    if(event.info && REXCVAR_GET(em2_trace_shadow_gpu_state) && REXCVAR_GET(em2_private_profile)){
        try{
            const auto& info=*event.info;
            if(!REXCVAR_GET(em2_user_data_directory).empty() && (info.IsCopyingDepth() || uint32_t(info.copy_dest_info.copy_dest_format)==36) && info.coordinate_info.width_div_8==128 && info.height_div_8==128){
                static std::atomic<unsigned> reports{0};
                if(reports.fetch_add(1)<16)REXLOG_INFO("Private shadow GPU resolve: physical={:08X} extent={:08X}+{} format={} copy_src_select={} is_depth={} depth_base={} color_base={} depth_clear={:08X} color_clear={:08X} scale={}x{}; original committed resolve retained",info.copy_dest_base,start,length,uint32_t(info.copy_dest_info.copy_dest_format),uint32_t(info.rb_copy_control.copy_src_select),info.IsCopyingDepth(),info.depth_original_base,info.color_original_base,info.rb_depth_clear,info.rb_color_clear,event.scale_x,event.scale_y);
            }
        }catch(...){}
    }
    active->valid_=false;active->observer_->written(active->observer_->context,event);_mm_setcsr(csr);
}
bool rex::graphics::draw_util::GetResolveInfo(const RegisterFile& regs,const memory::Memory& memory,TraceWriter& trace,
    uint32_t sx,uint32_t sy,bool rg16,bool rgba16,ResolveInfo& info) {
    const bool success=EM2OriginalGetResolveInfo(regs,memory,trace,sx,sy,rg16,rgba16,info);
    em2::ObserveNativeResolveInfo(success,info,sx,sy);return success;
}
void rex::graphics::TextureCache::MarkRangeAsResolved(uint32_t start,uint32_t length) {
    if(!active){EM2OriginalMarkRangeAsResolved(this,start,length);return;}
    // SharedMemory CPU invalidation uses this same recursive global lock.
    // Keep the native page marking and the observer commit indivisible.
    auto lock=global_critical_region_.Acquire();
    EM2OriginalMarkRangeAsResolved(this,start,length);
    start&=0x1FFFFFFF;length=std::min(length,0x20000000-start);
    em2::ObserveNativeResolveWrite(this,start,length);
}

bool em2::TestResolveWriteObserver(Em2CommandProcessor& cp) {
    namespace g=rex::graphics;namespace x=g::xenos;
    struct Fp {unsigned value=_mm_getcsr();~Fp(){_mm_setcsr(value);}}fp;
    if(!cp.IsGraphicsWorker() || !cp.DrainForResourceChange() || !TestResolveCoverage())return false;
    auto* memory=cp.*MemberOf(MemoryTag{});auto* heap=memory->LookupHeapByType(true,65536);if(!heap)return false;
    std::array<uint32_t,3> allocations{};
    struct Free {rex::memory::BaseHeap* heap;std::array<uint32_t,3>& addresses;~Free(){for(auto address:addresses)if(address)heap->Release(address);}}free{heap,allocations};
    for(auto& allocation:allocations)if(!heap->Alloc(65536,65536,rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,
        rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite,false,&allocation))return false;
    const auto vertices_address=allocations[0]&0x1FFFFFFF,destination_address=allocations[1]&0x1FFFFFFF;
    const auto partial_address=allocations[2]&0x1FFFFFFF;std::memset(memory->TranslatePhysical(partial_address),0xCD,65536);
    const float vertices[]={-.5f,-.5f,31.5f,-.5f,-.5f,31.5f};std::memcpy(memory->TranslatePhysical(vertices_address),vertices,sizeof(vertices));
    auto regs=std::make_unique<g::RegisterFile>();std::fill(std::begin(regs->values),std::end(regs->values),0u);
    auto set=[&]<class T>(T value){regs->values[T::register_index]=value.value;};
    x::xe_gpu_vertex_fetch_t fetch{};fetch.type=x::FetchConstantType::kVertex;fetch.size=6;fetch.address=vertices_address/4;
    std::memcpy(&regs->values[g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],&fetch,sizeof(fetch));
    g::reg::PA_SC_SCREEN_SCISSOR_BR screen{};screen.br_x=screen.br_y=32;set(screen);
    g::reg::PA_SC_WINDOW_SCISSOR_TL top{};top.window_offset_disable=1;set(top);
    g::reg::PA_SC_WINDOW_SCISSOR_BR window{};window.br_x=window.br_y=32;set(window);
    g::reg::RB_SURFACE_INFO surface{};surface.surface_pitch=32;set(surface);
    g::reg::RB_COLOR_INFO color{};color.color_format=x::ColorRenderTargetFormat::k_8_8_8_8;regs->values[g::XE_GPU_REG_RB_COLOR_INFO]=color.value;
    g::reg::RB_COPY_CONTROL control{};control.copy_command=x::CopyCommand::kRaw;set(control);
    g::reg::RB_COPY_DEST_INFO dest{};dest.copy_dest_format=x::ColorFormat(x::TextureFormat::k_8_8_8_8);set(dest);
    g::reg::RB_COPY_DEST_PITCH pitch{};pitch.copy_dest_pitch=pitch.copy_dest_height=32;set(pitch);
    regs->values[g::XE_GPU_REG_RB_COPY_DEST_BASE]=destination_address;
    auto* texture=(cp.*MemberOf(TextureTag{})).get();auto* shared=(cp.*MemberOf(SharedTag{})).get();
    if(texture->IsDrawResolutionScaled())return false; // This fixture independently reads native shared memory.
    auto trace=std::make_unique<g::TraceWriter>(memory->physical_membase());auto cache=std::make_unique<RT>(*regs,*memory,*trace,1,1,cp,false);
    struct Finish {Em2CommandProcessor& cp;std::unique_ptr<RT>& cache;std::unique_ptr<g::TraceWriter>& trace;std::unique_ptr<g::RegisterFile>& regs;
        ~Finish(){if(!cp.DrainForResourceChange()){cache.release();trace.release();regs.release();}}}finish{cp,cache,trace,regs};
    if(!cache->Initialize())return false;
    using Key=typename decltype(TypeOf(KeyTag{}))::type;using Target=typename decltype(TypeOf(TargetTag{}))::type;
    Key key;key.pitch_tiles_at_32bpp=1;key.resource_format=uint32_t(x::ColorRenderTargetFormat::k_8_8_8_8);
    auto* target=static_cast<Target*>((cache.get()->*MemberOf(CreateTag{}))(key));if(!target || !(cp.*MemberOf(BeginTag{}))(false))return false;
    (cache.get()->*MemberOf(OwnTag{}))(key,0,2,nullptr,nullptr);
    cp.PushTransitionBarrier(target->resource(),target->SetResourceState(D3D12_RESOURCE_STATE_RENDER_TARGET),D3D12_RESOURCE_STATE_RENDER_TARGET);cp.SubmitBarriers();
    const float red[]={1,0,0,1};cp.GetDeferredCommandList().D3DClearRenderTargetView(target->descriptor_draw().GetHandle(),red,0,nullptr);
    struct Events {unsigned count=0,typed=0,csr=0;uint32_t address=0,length=0;g::draw_util::ResolveInfo info{};ResolveRangeLedger ledger;bool ledger_ok=true;}events;
    ResolveLedgerWatch watch(*shared,events.ledger);if(!watch.valid())return false;
    ResolveWriteObserver observer{&events,[](void* opaque,const ResolveWriteObservation& event) noexcept {
        auto& state=*static_cast<Events*>(opaque);++state.count;state.address=event.address;state.length=event.length;state.csr=_mm_getcsr();
        state.ledger_ok&=state.ledger.Record(event);
        if(event.info){++state.typed;std::memcpy(&state.info,event.info,sizeof(state.info));}_mm_setcsr((_mm_getcsr()&~_MM_ROUND_MASK)|_MM_ROUND_DOWN);
    }};
    uint32_t written=0,length=0;
    auto tracked_copy=[&]() {
        // Exercise the actual virtual IssueCopy path with the fixture cache;
        // retain the native primary cache and restore it after the GPU drain.
        auto& primary=cp.*MemberOf(RenderCacheTag{});primary.swap(cache);
        auto* saved_observer=cp.resolve_write_observer_;cp.SetResolveWriteObserver(&observer);
        const bool result=cp.IssueCopy();const bool completed=cp.DrainForResourceChange();
        cp.SetResolveWriteObserver(saved_observer);primary.swap(cache);
        written=events.address;length=events.length;return result && completed;
    };
    {
        if(!tracked_copy())return false;
        if(events.count!=1 || events.typed!=1 || written!=events.address || length!=events.length || !length || _mm_getcsr()!=events.csr ||
           events.info.coordinate_info.width_div_8!=4 || events.info.height_div_8!=4 ||
           events.info.copy_dest_info.copy_dest_format!=x::ColorFormat(x::TextureFormat::k_8_8_8_8))return false;
    }
    if(!cp.DrainForResourceChange())return false;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    if(!pixel_test::Buffer(cp.GetD3D12Provider().GetDevice(),length,D3D12_HEAP_TYPE_READBACK,&readback) || !(cp.*MemberOf(BeginTag{}))(false))return false;
    shared->UseAsCopySource();cp.SubmitBarriers();cp.GetDeferredCommandList().D3DCopyBufferRegion(readback.Get(),0,shared->GetBuffer(),written,length);
    if(!cp.DrainForResourceChange()){readback.Detach();return false;}
    void* mapped=nullptr;D3D12_RANGE range{0,length};if(FAILED(readback->Map(0,&range,&mapped)))return false;
    bool correct=true;for(unsigned i=0;i<length/4;++i)correct&=static_cast<const uint32_t*>(mapped)[i]==0xFF0000FF;
    D3D12_RANGE none{0,0};readback->Unmap(0,&none);if(!correct)return false;
    const float partial_vertices[]={7.5f,7.5f,15.5f,7.5f,7.5f,15.5f};std::memcpy(memory->TranslatePhysical(vertices_address),partial_vertices,sizeof(partial_vertices));
    regs->values[g::XE_GPU_REG_RB_COPY_DEST_BASE]=partial_address;
    if(!(cp.*MemberOf(BeginTag{}))(false))return false;cache->BeginSubmission();
    {
        if(!tracked_copy() || events.count!=2 || events.typed!=2)return false;
    }
    std::vector<ResolvedStorageRange> coverage;
    const ResolveWriteObservation partial_event{texture,written,length,1,1,&events.info};
    if(!GetResolveCoverage(partial_event,coverage))return false;
    std::vector<uint8_t> expected(length,0xCD);size_t written_bytes=0;
    const uint8_t red_bytes[]={0xFF,0,0,0xFF};
    for(const auto& range:coverage)for(unsigned offset=0;offset<range.length;++offset) {
        const auto address=range.address+offset;expected[address-written]=red_bytes[address&3];++written_bytes;
    }
    if(written_bytes!=8*8*4 || written_bytes>=length || !cp.DrainForResourceChange())return false;
    readback.Reset();if(!pixel_test::Buffer(cp.GetD3D12Provider().GetDevice(),length,D3D12_HEAP_TYPE_READBACK,&readback) || !(cp.*MemberOf(BeginTag{}))(false))return false;
    shared->UseAsCopySource();cp.SubmitBarriers();cp.GetDeferredCommandList().D3DCopyBufferRegion(readback.Get(),0,shared->GetBuffer(),written,length);
    if(!cp.DrainForResourceChange()){readback.Detach();return false;}
    range={0,length};if(FAILED(readback->Map(0,&range,&mapped)))return false;
    correct=!std::memcmp(mapped,expected.data(),length);readback->Unmap(0,&none);
    if(!correct){REXLOG_ERROR("Partial native resolve exact coverage disagrees with GPU writes/sentinel holes");return false;}
    ResolveRangeLedger ledger;if(!ledger.Record(partial_event))return false;
    std::vector<ResolvedStorageRange> tracked;if(!ledger.Snapshot(tracked) || tracked.size()!=coverage.size())return false;
    std::vector<ResolvedStorageRange> current;
    if(!events.ledger_ok || !events.ledger.Snapshot(current))return false;
    ResolveRangeLedger owned;std::vector<ResolvedStorageRange> owned_ranges;
    if(!cp.CopyTrackedResolveLedger(owned) || !owned.Snapshot(owned_ranges) || owned_ranges.size()!=current.size() ||
       !std::equal(owned_ranges.begin(),owned_ranges.end(),current.begin(),[](const auto& a,const auto& b){
           return a.address==b.address && a.length==b.length && a.bytes_per_texel==b.bytes_per_texel;}))return false;
    RendererStorageSnapshot renderer_snapshot;
    auto& primary=cp.*MemberOf(RenderCacheTag{});primary.swap(cache);
    const bool capture_ok=CaptureRendererStorage(cp,renderer_snapshot);primary.swap(cache);
    {
        GraphicsResourceCheckpoint owner(cp);if(!owner.valid())return false;
        GraphicsResourceCheckpoint invalid(cp);auto* retained=renderer_snapshot.packed.buffer.Get();
        if(CaptureRendererStorage(cp,invalid,renderer_snapshot) || renderer_snapshot.packed.buffer.Get()!=retained)return false;
    }
    auto captured=std::move(renderer_snapshot.resolved);
    if(!capture_ok || !renderer_snapshot.packed.buffer || renderer_snapshot.packed.owners.empty() ||
       renderer_snapshot.packed.owners.front().key!=key.key || renderer_snapshot.packed.owners.front().begin_tile!=0 ||
       renderer_snapshot.packed.owners.front().end_tile!=2 || !renderer_snapshot.auxiliary.targets.empty() ||
       captured.spans.empty() || captured.scale_x!=1 || captured.scale_y!=1)return false;
    Microsoft::WRL::ComPtr<ID3D12Resource> packed_readback;constexpr uint32_t packed_bytes=2*5120;
    if(!pixel_test::Buffer(cp.GetD3D12Provider().GetDevice(),packed_bytes,D3D12_HEAP_TYPE_READBACK,&packed_readback) ||
       !(cp.*MemberOf(BeginTag{}))(false))return false;
    auto* packed_buffer=renderer_snapshot.packed.buffer.Get();
    cp.PushTransitionBarrier(packed_buffer,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);cp.SubmitBarriers();
    cp.GetDeferredCommandList().D3DCopyBufferRegion(packed_readback.Get(),0,packed_buffer,0,packed_bytes);
    cp.PushTransitionBarrier(packed_buffer,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);cp.SubmitBarriers();
    if(!cp.DrainForResourceChange()){packed_readback.Detach();renderer_snapshot.packed.buffer.Detach();return false;}
    void* packed_mapping=nullptr;D3D12_RANGE packed_range{0,packed_bytes};
    if(FAILED(packed_readback->Map(0,&packed_range,&packed_mapping)))return false;
    bool packed_correct=true;for(unsigned i=0;i<packed_bytes/4;++i)packed_correct&=static_cast<const uint32_t*>(packed_mapping)[i]==0xFF0000FF;
    packed_readback->Unmap(0,&none);if(!packed_correct)return false;
    REXLOG_INFO("Renderer storage capture passed: one pinned PM4 prefix and native memory lock across actual primary-cache packed EDRAM, auxiliary metadata and owned-ledger resolved capture; full/partial GPU data verified, no settings change or renderer publication");
    for(const auto& span:captured.spans) {
        Microsoft::WRL::ComPtr<ID3D12Resource> snapshot_readback;
        if(!pixel_test::Buffer(cp.GetD3D12Provider().GetDevice(),span.native.length,D3D12_HEAP_TYPE_READBACK,&snapshot_readback) || !(cp.*MemberOf(BeginTag{}))(false))return false;
        auto* buffer=captured.buffers[span.buffer_index].Get();cp.PushTransitionBarrier(buffer,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);cp.SubmitBarriers();
        cp.GetDeferredCommandList().D3DCopyBufferRegion(snapshot_readback.Get(),0,buffer,span.offset,span.native.length);
        cp.PushTransitionBarrier(buffer,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);cp.SubmitBarriers();
        if(!cp.DrainForResourceChange()){snapshot_readback.Detach();for(auto& entry:captured.buffers)entry.Detach();return false;}
        void* snapshot_mapping=nullptr;D3D12_RANGE snapshot_range{0,span.native.length};
        if(FAILED(snapshot_readback->Map(0,&snapshot_range,&snapshot_mapping)))return false;bool snapshot_correct=true;
        for(unsigned i=0;i<span.native.length/4;++i)snapshot_correct&=static_cast<const uint32_t*>(snapshot_mapping)[i]==0xFF0000FF;
        snapshot_readback->Unmap(0,&none);if(!snapshot_correct)return false;
    }
    // Change resident GPU data before restoring red snapshots: checking red
    // over already-red storage would not prove that restoration copied data.
    ResolvedStorageSnapshot blue;
    if(!ConvertResolvedStorage(cp,captured,1,1,blue) || blue.buffers.size()!=1)return false;
    Microsoft::WRL::ComPtr<ID3D12Resource> blue_upload;
    const auto blue_bytes=blue.buffers.front()->GetDesc().Width;
    if(!pixel_test::Buffer(cp.GetD3D12Provider().GetDevice(),blue_bytes,D3D12_HEAP_TYPE_UPLOAD,&blue_upload))return false;
    void* blue_mapping=nullptr;if(FAILED(blue_upload->Map(0,&none,&blue_mapping)))return false;
    std::fill_n(static_cast<uint32_t*>(blue_mapping),size_t(blue_bytes/4),0xFFFF0000u);blue_upload->Unmap(0,nullptr);
    if(!(cp.*MemberOf(BeginTag{}))(false))return false;
    cp.GetDeferredCommandList().D3DCopyBufferRegion(blue.buffers.front().Get(),0,blue_upload.Get(),0,blue_bytes);
    if(!cp.DrainForResourceChange()){blue_upload.Detach();for(auto& buffer:blue.buffers)buffer.Detach();return false;}
    auto verify_shared=[&](uint32_t word) {
        for(unsigned region=0;region<2;++region) {
            const auto address=region?written:destination_address,bytes=region?length:32u*32*4;
            Microsoft::WRL::ComPtr<ID3D12Resource> result;
            if(!pixel_test::Buffer(cp.GetD3D12Provider().GetDevice(),bytes,D3D12_HEAP_TYPE_READBACK,&result) || !(cp.*MemberOf(BeginTag{}))(false))return false;
            shared->UseAsCopySource();cp.SubmitBarriers();
            cp.GetDeferredCommandList().D3DCopyBufferRegion(result.Get(),0,shared->GetBuffer(),address,bytes);
            if(!cp.DrainForResourceChange()){result.Detach();return false;}
            void* data=nullptr;D3D12_RANGE read{0,bytes};if(FAILED(result->Map(0,&read,&data)))return false;
            bool matched=true;
            for(uint32_t offset=0;offset<bytes;++offset) {
                bool covered=!region;
                if(region)for(const auto& part:coverage)covered|=address+offset>=part.address && uint64_t(address)+offset<uint64_t(part.address)+part.length;
                const auto wanted=covered?uint8_t(word>>((offset&3)*8)):uint8_t(0xCD);
                matched&=static_cast<const uint8_t*>(data)[offset]==wanted;
            }
            result->Unmap(0,&none);if(!matched)return false;
        }
        return true;
    };
    ResolveRangeLedger restored_ledger;
    if(!RestoreResolvedStorage(cp,blue,*texture,restored_ledger) || !verify_shared(0xFFFF0000))return false;
    for(const auto scale:std::array<std::array<uint32_t,2>,3>{{{2,2},{3,3},{2,3}}}) {
        ResolvedStorageSnapshot scaled_snapshot,returned;
        if(!ConvertResolvedStorage(cp,captured,scale[0],scale[1],scaled_snapshot) ||
           !ConvertResolvedStorage(cp,scaled_snapshot,1,1,returned) ||
           !RestoreResolvedStorage(cp,returned,*texture,restored_ledger) || !verify_shared(0xFF0000FF))return false;
        // Repaint between attempts, so every red restore must perform a copy.
        if(!RestoreResolvedStorage(cp,blue,*texture,restored_ledger) || !verify_shared(0xFFFF0000))return false;
    }
    if(!RestoreResolvedStorage(cp,captured,*texture,restored_ledger) || !verify_shared(0xFF0000FF) || events.count!=2)return false;
    REXLOG_INFO("Native1x resolved restoration passed: eight commits with blue/red repaint, captured native full32x32 and partial8x8 RGBA8, return from2x2/3x3/2x3 snapshots, direct shared-memory readback, every byte including resident sentinel holes, native GPU notifications without spurious resolve observer callbacks; uninitialized scaled-only holes and renderer switch untested");
    const auto before_pages=events.ledger.page_count();
    // Use the actual SDK invalidation entry, which broadcasts the same global
    // watch callback used for guest CPU writes. One byte invalidates its page.
    shared->MemoryInvalidationCallback(written+8,1,true);
    if(!before_pages || events.ledger.page_count()>=before_pages || !events.ledger.Snapshot(current) ||
       !cp.CopyTrackedResolveLedger(owned) || !owned.Snapshot(owned_ranges) || owned_ranges.size()!=current.size() ||
       !std::equal(owned_ranges.begin(),owned_ranges.end(),current.begin(),[](const auto& a,const auto& b){
           return a.address==b.address && a.length==b.length && a.bytes_per_texel==b.bytes_per_texel;}))return false;
    size_t remaining_bytes=0;for(const auto& entry:current) {
        if(entry.address/4096==written/4096)return false;remaining_bytes+=entry.length;
    }
    if(remaining_bytes!=32*32*4)return false; // The neighboring full resolve survives.
    // Native markers without matching, fresh geometry must not be assigned a
    // guessed format; zero writes, nesting and observer lifetime are explicit.
    {
        ResolveWriteScope outer(texture,&observer);ObserveNativeResolveInfo(true,events.info,1,1);
        {ResolveWriteScope nested(texture,nullptr);texture->MarkRangeAsResolved(written,length);}
        texture->MarkRangeAsResolved(written,length); // Saved outer geometry survives nesting.
        texture->MarkRangeAsResolved(written,length); // It is consumed once.
        ObserveNativeResolveInfo(false,events.info,1,1);texture->MarkRangeAsResolved(written,length);
        ObserveNativeResolveInfo(true,events.info,2,2);texture->MarkRangeAsResolved(written,length);
        texture->MarkRangeAsResolved(written,0);
    }
    const auto count=events.count;texture->MarkRangeAsResolved(written,length);
    if(events.count!=count || events.count!=6 || events.typed!=3)return false;
    REXLOG_INFO("Renderer-owned resolve tracking passed: actual virtual IssueCopy for full/partial native resolves, external observer fanout, owned ledger snapshot and capture, real CPU invalidation with neighboring resolve retention; setup/shutdown watcher owned by processor, live renderer swap not installed");
    REXLOG_INFO("Native resolve-write observer passed: actual full 32x32 and partial 8x8 RGBA8 GPU resolve/readback, exact written groups and untouched sentinel holes, independent 1x resolved-storage snapshot, sparse ledger with real SDK CPU invalidation and neighboring resolve retention, native normalized geometry and committed range, MXCSR isolation, nested scope restoration, consumed/failed/wrong-scale metadata rejection, zero-write and disabled paths; gameplay tracking/migration not installed");
    return true;
}

#include "edram_capture.h"
#include "scaled_resolve_migration.h"
#include "gpu_constants_internal.h"
#include "gpu_sync.h"
#include "renderer_bindings.h"
#include "renderer_graph.h"
#include "renderer_storage.h"
#include "resolve_write_observer.h"
#include "resolved_storage_capture.h"
#include "native_pixel_fixture.h"
#include "auxiliary_depth_capture_test.h"
#include "auxiliary_depth_paint_test.h"
#include "auxiliary_depth_resample.h"
#include <rex/graphics/d3d12/render_target_cache.h>
#include <rex/graphics/d3d12/deferred_command_list.h>
#include <rex/graphics/pipeline/render_target/cache.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/trace_writer.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <memory>
#include <span>
#include <type_traits>
#include <immintrin.h>
REXCVAR_DECLARE(std::string,render_target_path_d3d12);
namespace {
using CP=rex::graphics::d3d12::D3D12CommandProcessor;
using RT=rex::graphics::d3d12::D3D12RenderTargetCache;
using Base=rex::graphics::RenderTargetCache;
using Deferred=rex::graphics::d3d12::DeferredCommandList;
using Microsoft::WRL::ComPtr;
// Use the pinned SDK's real members and types, without changing its headers or
// guessing object offsets. Access is checked again by compilation on SDK changes.
template<class Tag,auto Member>struct Access {friend auto MemberOf(Tag){return Member;}};
template<class Tag,class T>struct TypeAccess {friend auto TypeOf(Tag){return std::type_identity<T>{};}};
struct BeginTag{friend auto MemberOf(BeginTag);};template struct Access<BeginTag,&CP::BeginSubmission>;
struct MemoryTag{friend auto MemberOf(MemoryTag);};template struct Access<MemoryTag,&rex::graphics::CommandProcessor::memory_>;
struct OwnedLedgerTag{friend auto MemberOf(OwnedLedgerTag);};template struct Access<OwnedLedgerTag,&em2::Em2CommandProcessor::resolve_ledger_>;
struct IssueCopyTag{friend auto MemberOf(IssueCopyTag);};template struct Access<IssueCopyTag,&em2::Em2CommandProcessor::IssueCopy>;
struct RegistersTag{friend auto MemberOf(RegistersTag);};template struct Access<RegistersTag,&rex::graphics::CommandProcessor::register_file_>;
struct SharedTag{friend auto MemberOf(SharedTag);};template struct Access<SharedTag,&CP::shared_memory_>;
struct BindlessTag{friend auto MemberOf(BindlessTag);};template struct Access<BindlessTag,&CP::bindless_resources_used_>;
struct PrimaryTag{friend auto MemberOf(PrimaryTag);};template struct Access<PrimaryTag,&CP::render_target_cache_>;
struct DumpTag{friend auto MemberOf(DumpTag);};template struct Access<DumpTag,&RT::DumpRenderTargets>;
struct BufferTag{friend auto MemberOf(BufferTag);};template struct Access<BufferTag,&RT::edram_buffer_>;
struct TransitionTag{friend auto MemberOf(TransitionTag);};template struct Access<TransitionTag,&RT::TransitionEdramBuffer>;
struct OwnersTag{friend auto MemberOf(OwnersTag);};template struct Access<OwnersTag,&Base::ownership_ranges_>;
struct TargetsTag{friend auto MemberOf(TargetsTag);};template struct Access<TargetsTag,&Base::render_targets_>;
struct RectanglesTag{friend auto MemberOf(RectanglesTag);};template struct Access<RectanglesTag,&Base::GetResolveCopyRectanglesToDump>;
struct CreateTargetTag{friend auto MemberOf(CreateTargetTag);};template struct Access<CreateTargetTag,&Base::GetOrCreateRenderTarget>;
struct ChangeOwnerTag{friend auto MemberOf(ChangeOwnerTag);};template struct Access<ChangeOwnerTag,&Base::ChangeOwnership>;
struct TransfersTag{friend auto MemberOf(TransfersTag);};template struct Access<TransfersTag,&RT::PerformTransfersAndResolveClears>;
struct StreamTag{friend auto MemberOf(StreamTag);};template struct Access<StreamTag,&Deferred::command_stream_>;
struct DispatchTag{friend auto MemberOf(DispatchTag);};template struct Access<DispatchTag,Deferred::Command::kD3DDispatch>;
struct RootConstantsTag{friend auto MemberOf(RootConstantsTag);};template struct Access<RootConstantsTag,Deferred::Command::kD3DSetComputeRoot32BitConstants>;
struct OffsetsIndexTag{friend auto MemberOf(OffsetsIndexTag);};template struct Access<OffsetsIndexTag,RT::kDumpRootParameterOffsets>;
struct RootHeaderTypeTag{friend auto TypeOf(RootHeaderTypeTag);};template struct TypeAccess<RootHeaderTypeTag,Deferred::SetRoot32BitConstantsHeader>;
struct DumpOffsetsTypeTag{friend auto TypeOf(DumpOffsetsTypeTag);};template struct TypeAccess<DumpOffsetsTypeTag,RT::DumpOffsets>;
struct DrawTag{friend auto MemberOf(DrawTag);};template struct Access<DrawTag,Deferred::Command::kD3DDrawInstanced>;
struct HeaderTag{friend auto TypeOf(HeaderTag);};template struct TypeAccess<HeaderTag,Deferred::CommandHeader>;
struct ArgumentsTag{friend auto TypeOf(ArgumentsTag);};template struct TypeAccess<ArgumentsTag,Deferred::D3DDispatchArguments>;
struct RectTypeTag{friend auto TypeOf(RectTypeTag);};template struct TypeAccess<RectTypeTag,Base::ResolveCopyDumpRectangle>;
struct HostTypeTag{friend auto TypeOf(HostTypeTag);};template struct TypeAccess<HostTypeTag,RT::D3D12RenderTarget>;
struct KeyTypeTag{friend auto TypeOf(KeyTypeTag);};template struct TypeAccess<KeyTypeTag,Base::RenderTargetKey>;
struct TransferTypeTag{friend auto TypeOf(TransferTypeTag);};template struct TypeAccess<TransferTypeTag,Base::Transfer>;
struct BaseTargetTypeTag{friend auto TypeOf(BaseTargetTypeTag);};template struct TypeAccess<BaseTargetTypeTag,Base::RenderTarget>;
using Header=typename decltype(TypeOf(HeaderTag{}))::type;
using Arguments=typename decltype(TypeOf(ArgumentsTag{}))::type;
using Rectangle=typename decltype(TypeOf(RectTypeTag{}))::type;
using HostTarget=typename decltype(TypeOf(HostTypeTag{}))::type;
using TargetKey=typename decltype(TypeOf(KeyTypeTag{}))::type;
using NativeTransfer=typename decltype(TypeOf(TransferTypeTag{}))::type;
using BaseTarget=typename decltype(TypeOf(BaseTargetTypeTag{}))::type;
using Dispatch=std::array<uint32_t,3>;
constexpr uint32_t tile_count=2048,edram_bytes=tile_count*5120;

bool ReadDispatches(std::span<const uintmax_t> stream,std::vector<Dispatch>& output) {
    constexpr size_t header_words=(sizeof(Header)+sizeof(uintmax_t)-1)/sizeof(uintmax_t);
    constexpr size_t argument_words=(sizeof(Arguments)+sizeof(uintmax_t)-1)/sizeof(uintmax_t);
    size_t cursor=0;
    while(cursor<stream.size()) {
        if(stream.size()-cursor<header_words)return false;
        Header header;std::memcpy(&header,stream.data()+cursor,sizeof(header));cursor+=header_words;
        if(header.arguments_size_elements>stream.size()-cursor)return false;
        if(header.command==MemberOf(DispatchTag{})) {
            if(header.arguments_size_elements!=argument_words)return false;
            Arguments arguments;std::memcpy(&arguments,stream.data()+cursor,sizeof(arguments));
            if(!arguments.thread_group_count_x || !arguments.thread_group_count_y || !arguments.thread_group_count_z)return false;
            output.push_back({arguments.thread_group_count_x,arguments.thread_group_count_y,arguments.thread_group_count_z});
        }
        cursor+=header.arguments_size_elements;
    }
    return cursor==stream.size();
}
bool Capture(em2::Em2CommandProcessor& cp,RT& cache,em2::PackedEdramSnapshot& output,bool perform_dump) {
    struct Fp {unsigned csr=_mm_getcsr();~Fp(){_mm_setcsr(csr);}}fp;
    if(!cp.IsGraphicsWorker() || cache.GetPath()!=Base::Path::kHostRenderTargets || !cp.DrainForResourceChange())return false;
    em2::PackedEdramSnapshot snapshot;
    snapshot.scale_x=cache.draw_resolution_scale_x();snapshot.scale_y=cache.draw_resolution_scale_y();
    if(!snapshot.scale_x || snapshot.scale_x>7 || !snapshot.scale_y || snapshot.scale_y>7)return false;
    uint32_t next=0;
    for(const auto& [begin,owner]:cache.*MemberOf(OwnersTag{})) {
        if(begin!=next || owner.end_tiles<=begin || owner.end_tiles>tile_count)return false;
        snapshot.owners.push_back({begin,owner.end_tiles,owner.render_target.key,owner.host_depth_render_target_unorm24.key,
            owner.host_depth_render_target_float24.key,!owner.render_target.IsEmpty() && owner.render_target.Is64bpp(),
            !owner.render_target.IsEmpty() && bool(owner.render_target.is_depth)});
        next=owner.end_tiles;
    }
    if(next!=tile_count)return false;
    struct DumpArea {uint32_t base,length,source_base;bool wrapped;};std::vector<DumpArea> areas;
    // Dump offsets need the unwrapped EDRAM period, or tiles below an owner's
    // nonzero base underflow its source texture coordinates. Split physical
    // owner spans at that base before retaining the period in native constants.
    for(const auto& owner:snapshot.owners)if(owner.key) {
        TargetKey key;key.key=owner.key;uint32_t begin=owner.begin_tile;
        if(begin<key.base_tiles) {
            const uint32_t end=std::min(owner.end_tile,uint32_t(key.base_tiles));
            areas.push_back({begin,end-begin,uint32_t(key.base_tiles),true});begin=end;
        }
        if(begin<owner.end_tile)areas.push_back({begin,owner.end_tile-begin,uint32_t(key.base_tiles),false});
    }
    std::vector<Dispatch> expected;std::array<uint8_t,tile_count> coverage{};
    for(const auto& area:areas) {
        std::vector<Rectangle> rectangles;
        (cache.*MemberOf(RectanglesTag{}))(area.base,area.length,1,tile_count,rectangles);
        for(const auto& rectangle:rectangles) {
            std::array<Rectangle::Dispatch,Rectangle::kMaxDispatches> dispatches{};
            const auto count=rectangle.GetDispatches(tile_count,area.length,dispatches.data());
            for(unsigned i=0;i<count;++i) {
                const auto& d=dispatches[i];
                if(!d.width_tiles || !d.height_tiles || uint64_t(d.offset)+(uint64_t(d.height_tiles)-1)*tile_count+d.width_tiles>area.length)return false;
                for(unsigned row=0;row<d.height_tiles;++row)for(unsigned x=0;x<d.width_tiles;++x)
                    if(coverage[(area.base+d.offset+row*tile_count+x)&(tile_count-1)]++)return false;
                expected.push_back({(d.width_tiles*snapshot.scale_x)<<unsigned(!rectangle.render_target->key().Is64bpp()),
                    d.height_tiles*snapshot.scale_y,1});
            }
        }
    }
    for(const auto& owner:snapshot.owners)for(unsigned tile=owner.begin_tile;tile<owner.end_tile;++tile)
        if(bool(coverage[tile])!=(owner.key!=0))return false;
    if(!(cp.*MemberOf(BeginTag{}))(false))return false;
    if((cp.*MemberOf(PrimaryTag{})).get()!=&cache)cache.BeginSubmission();
    auto& list=cp.GetDeferredCommandList();auto& stream=list.*MemberOf(StreamTag{});
    const auto start=stream.size(),submission=cp.GetCurrentSubmission();
    if(perform_dump)for(const auto& area:areas) {
        const auto area_start=stream.size();
        (cache.*MemberOf(DumpTag{}))(area.base,area.length,1,tile_count);
        if(cp.GetCurrentSubmission()!=submission || stream.size()<area_start)return false;
        if(area.wrapped) {
            // Native source addressing expects an unwrapped dispatch tile.
            // Keep the requested physical extent (for ownership lookup), then
            // retain the correct period in its typed deferred root constant.
            using RootHeader=typename decltype(TypeOf(RootHeaderTypeTag{}))::type;
            using Offsets=typename decltype(TypeOf(DumpOffsetsTypeTag{}))::type;
            constexpr size_t header_words=(sizeof(Header)+sizeof(uintmax_t)-1)/sizeof(uintmax_t);
            size_t cursor=area_start;unsigned rewritten=0;
            while(cursor<stream.size()) {
                if(stream.size()-cursor<header_words)return false;
                Header header;std::memcpy(&header,stream.data()+cursor,sizeof(header));cursor+=header_words;
                if(header.arguments_size_elements>stream.size()-cursor)return false;
                if(header.command==MemberOf(RootConstantsTag{})) {
                    if(header.arguments_size_elements*sizeof(uintmax_t)<sizeof(RootHeader))return false;
                    RootHeader root;std::memcpy(&root,stream.data()+cursor,sizeof(root));
                    if(root.root_parameter_index==MemberOf(OffsetsIndexTag{})) {
                        if(root.num_32bit_values_to_set!=1 || root.dest_offset_in_32bit_values ||
                           header.arguments_size_elements*sizeof(uintmax_t)<sizeof(root)+sizeof(Offsets))return false;
                        auto* data=reinterpret_cast<uint8_t*>(stream.data()+cursor)+sizeof(root);
                        Offsets offsets;std::memcpy(&offsets,data,sizeof(offsets));
                        if(offsets.source_base_tiles!=area.source_base || offsets.dispatch_first_tile>=area.source_base)return false;
                        offsets.dispatch_first_tile=uint32_t(offsets.dispatch_first_tile)+tile_count;
                        std::memcpy(data,&offsets,sizeof(offsets));++rewritten;
                    }
                }
                cursor+=header.arguments_size_elements;
            }
            if(!rewritten)return false;
        }
    }
    if(submission!=cp.GetCurrentSubmission() || stream.size()<start)return false;
    std::vector<Dispatch> actual;
    if(!ReadDispatches(std::span<const uintmax_t>(stream).subspan(start),actual))return false;
    std::sort(actual.begin(),actual.end());std::sort(expected.begin(),expected.end());
    if(actual!=expected) {
        if(perform_dump)REXLOG_ERROR("Packed EDRAM capture rejected incomplete dump: expected {} dispatches, recorded {}",expected.size(),actual.size());
        else REXLOG_INFO("Packed EDRAM test intentionally rejected missing dump dispatches");
        return false;
    }
    const auto bytes=uint64_t(edram_bytes)*snapshot.scale_x*snapshot.scale_y;
    auto* source=cache.*MemberOf(BufferTag{});
    if(!source || source->GetDesc().Width<bytes)return false;
    auto desc=source->GetDesc();desc.Width=bytes;desc.Flags=D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    auto* device=cp.GetD3D12Provider().GetDevice();
    if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&snapshot.buffer))))return false;
    struct RetainPending {
        ComPtr<ID3D12Resource>& buffer;bool armed=false;
        ~RetainPending(){if(armed)buffer.Detach();}
    }retain{snapshot.buffer};
    (cache.*MemberOf(TransitionTag{}))(D3D12_RESOURCE_STATE_COPY_SOURCE);cp.SubmitBarriers();
    retain.armed=true;
    list.D3DCopyBufferRegion(snapshot.buffer.Get(),0,source,0,bytes);
    if(!cp.DrainForResourceChange()) {
        // The caller retains its old cache. Preserve this possible in-flight
        // destination too rather than freeing it after a failed device wait.
        return false;
    }
    retain.armed=false;
    snapshot.completed_submission=cp.GetCompletedSubmission();output=std::move(snapshot);return true;
}
}
bool em2::CapturePackedEdram(Em2CommandProcessor& cp,RT& cache,PackedEdramSnapshot& output) {
    return Capture(cp,cache,output,true);
}

bool em2::CaptureAuxiliaryDepth(Em2CommandProcessor& cp,RT& cache,AuxiliaryDepthSnapshot& output) {
    struct Fp {unsigned csr=_mm_getcsr();~Fp(){_mm_setcsr(csr);}}fp;
    if(!cp.IsGraphicsWorker() || cache.GetPath()!=Base::Path::kHostRenderTargets || !cp.DrainForResourceChange())return false;
    AuxiliaryDepthSnapshot snapshot;snapshot.scale_x=cache.draw_resolution_scale_x();snapshot.scale_y=cache.draw_resolution_scale_y();
    if(!snapshot.scale_x || snapshot.scale_x>7 || !snapshot.scale_y || snapshot.scale_y>7)return false;
    std::vector<uint32_t> keys;uint32_t next=0;
    for(const auto& [begin,owner]:cache.*MemberOf(OwnersTag{})) {
        if(begin!=next || owner.end_tiles<=begin || owner.end_tiles>tile_count)return false;
        snapshot.owners.push_back({begin,owner.end_tiles,owner.render_target.key,owner.host_depth_render_target_unorm24.key,
            owner.host_depth_render_target_float24.key,!owner.render_target.IsEmpty() && owner.render_target.Is64bpp(),
            !owner.render_target.IsEmpty() && bool(owner.render_target.is_depth)});
        for(const auto key:{owner.host_depth_render_target_unorm24,owner.host_depth_render_target_float24})
            if(!key.IsEmpty())keys.push_back(key.key);
        next=owner.end_tiles;
    }
    if(next!=tile_count)return false;
    std::sort(keys.begin(),keys.end());keys.erase(std::unique(keys.begin(),keys.end()),keys.end());
    std::vector<HostTarget*> sources;auto* device=cp.GetD3D12Provider().GetDevice();
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    for(const auto bits:keys) {
        TargetKey key;key.key=bits;if(!key.is_depth)return false;
        const auto& targets=cache.*MemberOf(TargetsTag{});const auto it=targets.find(key);
        if(it==targets.end() || !it->second)return false; // Never create a missing source.
        auto* source=static_cast<HostTarget*>(it->second);const auto desc=source->resource()->GetDesc();
        if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D)return false;
        AuxiliaryDepthTarget target;target.key=bits;
        if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,IID_PPV_ARGS(&target.texture))))return false;
        sources.push_back(source);snapshot.targets.push_back(std::move(target));
    }
    struct RetainPending {
        std::vector<AuxiliaryDepthTarget>& targets;bool armed=false;
        ~RetainPending(){if(armed)for(auto& target:targets)target.texture.Detach();}
    }retain{snapshot.targets};
    if(!sources.empty()) {
        if(!(cp.*MemberOf(BeginTag{}))(false))return false;
        for(auto* source:sources)cp.PushTransitionBarrier(source->resource(),source->SetResourceState(D3D12_RESOURCE_STATE_COPY_SOURCE),D3D12_RESOURCE_STATE_COPY_SOURCE);
        cp.SubmitBarriers();retain.armed=true;
        for(size_t i=0;i<sources.size();++i)
            cp.GetDeferredCommandList().D3DCopyResource(snapshot.targets[i].texture.Get(),sources[i]->resource());
        if(!cp.DrainForResourceChange())return false;
    }
    retain.armed=false;snapshot.completed_submission=cp.GetCompletedSubmission();output=std::move(snapshot);return true;
}

bool em2::RestoreAuxiliaryDepth(Em2CommandProcessor& cp,const AuxiliaryDepthSnapshot& snapshot,RT& cache) {
    struct Fp {unsigned csr=_mm_getcsr();~Fp(){_mm_setcsr(csr);}}fp;
    if(!cp.IsGraphicsWorker() || cache.GetPath()!=Base::Path::kHostRenderTargets || !cp.DrainForResourceChange())return false;
    const auto sx=snapshot.scale_x,sy=snapshot.scale_y,dx=cache.draw_resolution_scale_x(),dy=cache.draw_resolution_scale_y();
    if(!sx || sx>7 || !sy || sy>7 || !dx || dx>7 || !dy || dy>7)return false;
    std::vector<uint32_t> keys;
    for(const auto& target:snapshot.targets) {
        TargetKey key;key.key=target.key;
        if(!target.texture || !key.is_depth || key.GetDepthFormat()!=rex::graphics::xenos::DepthRenderTargetFormat::kD24FS8 ||
           target.texture->GetDesc().Format!=DXGI_FORMAT_R32G8X24_TYPELESS)return false;
        keys.push_back(target.key);
    }
    std::sort(keys.begin(),keys.end());if(std::adjacent_find(keys.begin(),keys.end())!=keys.end())return false;
    uint32_t next=0;
    for(const auto& owner:snapshot.owners) {
        if(owner.begin_tile!=next || owner.end_tile<=next || owner.end_tile>tile_count || owner.depth_unorm_key ||
           (owner.depth_float_key && (!owner.key || !std::binary_search(keys.begin(),keys.end(),owner.depth_float_key))))return false;
        next=owner.end_tile;
    }
    if(next!=tile_count || (!keys.empty() && cache.depth_float24_convert_in_pixel_shader()))return false;
    auto proposed=cache.*MemberOf(OwnersTag{});next=0;
    for(const auto& [begin,owner]:proposed) {
        if(begin!=next || owner.end_tiles<=begin || owner.end_tiles>tile_count || !owner.host_depth_render_target_unorm24.IsEmpty() ||
           !owner.host_depth_render_target_float24.IsEmpty())return false;
        next=owner.end_tiles;
    }
    if(next!=tile_count)return false;
    proposed.clear();const auto& current=cache.*MemberOf(OwnersTag{});auto stage=current.begin();size_t source_index=0;next=0;
    while(next<tile_count) {
        const auto& source=snapshot.owners[source_index];const auto& destination=stage->second;
        if(bool(source.key)==destination.render_target.IsEmpty() || (source.key && source.pixels_are_64_bit!=destination.render_target.Is64bpp()))return false;
        auto range=destination;range.end_tiles=std::min(source.end_tile,destination.end_tiles);
        range.host_depth_render_target_float24.key=source.depth_float_key;
        proposed.emplace(next,range);next=range.end_tiles;
        if(next==source.end_tile)++source_index;if(next==destination.end_tiles)++stage;
    }
    if(snapshot.targets.empty()){(cache.*MemberOf(OwnersTag{}))=std::move(proposed);return true;}
    struct Job {
        ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Fence> fence;
        ComPtr<ID3D12DescriptorHeap> views,dsvs;
        std::vector<ComPtr<ID3D12Resource>> sources,destinations;
        std::array<std::unique_ptr<AuxiliaryDepthResampler>,3> resamplers;
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);~Job(){if(event)CloseHandle(event);}
    };
    auto job=std::make_unique<Job>();auto* device=cp.GetD3D12Provider().GetDevice();
    const bool scaled=sx!=dx || sy!=dy;
    D3D12_DESCRIPTOR_HEAP_DESC view_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,UINT(snapshot.targets.size()*2),D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV,UINT(snapshot.targets.size()),D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    if(!job->event || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&job->allocator))) ||
       FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,job->allocator.Get(),nullptr,IID_PPV_ARGS(&job->list))) ||
       FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&job->fence))) ||
       (scaled && (FAILED(device->CreateDescriptorHeap(&view_desc,IID_PPV_ARGS(&job->views))) || FAILED(device->CreateDescriptorHeap(&dsv_desc,IID_PPV_ARGS(&job->dsvs))))))return false;
    const auto view_step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV),dsv_step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    if(scaled){ID3D12DescriptorHeap* heaps[]={job->views.Get()};job->list->SetDescriptorHeaps(1,heaps);}
    for(size_t i=0;i<snapshot.targets.size();++i) {
        const auto& source=snapshot.targets[i];TargetKey key;key.key=source.key;
        auto* destination=static_cast<HostTarget*>((cache.*MemberOf(CreateTargetTag{}))(key));if(!destination)return false;
        const auto a=source.texture->GetDesc(),b=destination->resource()->GetDesc();
        if(a.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || a.DepthOrArraySize!=1 || a.MipLevels!=1 || a.Format!=b.Format ||
           a.Width*dx!=b.Width*sx || uint64_t(a.Height)*dy!=uint64_t(b.Height)*sy || a.SampleDesc.Count!=b.SampleDesc.Count)return false;
        job->sources.push_back(source.texture);job->destinations.emplace_back(destination->resource());
        const auto destination_state=scaled?D3D12_RESOURCE_STATE_DEPTH_WRITE:D3D12_RESOURCE_STATE_COPY_DEST;
        const auto before=destination->SetResourceState(destination_state);
        if(before!=destination_state)pixel_test::Barrier(job->list.Get(),destination->resource(),before,destination_state);
        const auto source_state=scaled?D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:D3D12_RESOURCE_STATE_COPY_SOURCE;
        pixel_test::Barrier(job->list.Get(),source.texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,source_state);
        if(!scaled)job->list->CopyResource(destination->resource(),source.texture.Get());
        else {
            const auto samples=a.SampleDesc.Count;const unsigned slot=samples==1?0:samples==2?1:2;
            if(samples!=1 && samples!=2 && samples!=4)return false;
            if(!job->resamplers[slot]) {
                auto program=std::make_unique<AuxiliaryDepthResampler>();if(!program->Initialize(device,samples))return false;
                job->resamplers[slot]=std::move(program);
            }
            for(unsigned plane=0;plane<2;++plane) {
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=plane?DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
                srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.ViewDimension=samples>1?D3D12_SRV_DIMENSION_TEXTURE2DMS:D3D12_SRV_DIMENSION_TEXTURE2D;
                if(samples==1){srv.Texture2D.MipLevels=1;srv.Texture2D.PlaneSlice=plane;}
                auto view=job->views->GetCPUDescriptorHandleForHeapStart();view.ptr+=(i*2+plane)*view_step;device->CreateShaderResourceView(source.texture.Get(),&srv,view);
            }
            auto dsv=job->dsvs->GetCPUDescriptorHandleForHeapStart();dsv.ptr+=i*dsv_step;
            device->CopyDescriptorsSimple(1,dsv,destination->descriptor_draw().GetHandle(),D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
            auto view=job->views->GetGPUDescriptorHandleForHeapStart();view.ptr+=i*2*view_step;
            if(!job->resamplers[slot]->Record(job->list.Get(),destination->resource(),dsv,view,sx,sy,dx,dy))return false;
        }
        pixel_test::Barrier(job->list.Get(),source.texture.Get(),source_state,D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if(FAILED(job->list->Close()))return false;
    ID3D12CommandList* lists[]={job->list.Get()};auto* queue=cp.GetD3D12Provider().GetDirectQueue();queue->ExecuteCommandLists(1,lists);
    if(FAILED(queue->Signal(job->fence.Get(),1)) || FAILED(job->fence->SetEventOnCompletion(1,job->event)) ||
       WaitForSingleObject(job->event,30000)!=WAIT_OBJECT_0 || FAILED(device->GetDeviceRemovedReason())){job.release();return false;}
    (cache.*MemberOf(OwnersTag{}))=std::move(proposed);return true;
}

bool em2::RestorePackedEdram(Em2CommandProcessor& cp,const PackedEdramSnapshot& snapshot,RT& cache) {
    struct Fp {unsigned csr=_mm_getcsr();~Fp(){_mm_setcsr(csr);}}fp;
    if(!cp.IsGraphicsWorker() || cache.GetPath()!=Base::Path::kHostRenderTargets || !snapshot.buffer ||
       !cp.DrainForResourceChange())return false;
    const auto sx=snapshot.scale_x,sy=snapshot.scale_y,dx=cache.draw_resolution_scale_x(),dy=cache.draw_resolution_scale_y();
    if(!sx || sx>7 || !sy || sy>7 || !dx || dx>7 || !dy || dy>7 ||
       snapshot.buffer->GetDesc().Width<uint64_t(edram_bytes)*sx*sy)return false;
    for(const auto& [begin,owner]:cache.*MemberOf(OwnersTag{}))
        if(!owner.render_target.IsEmpty() || !owner.host_depth_render_target_unorm24.IsEmpty() ||
           !owner.host_depth_render_target_float24.IsEmpty())return false;
    uint32_t next=0;std::array<bool,2> needed{};
    for(const auto& owner:snapshot.owners) {
        if(owner.begin_tile!=next || owner.end_tile<=next || owner.end_tile>tile_count)return false;
        TargetKey original;original.key=owner.key;
        if(owner.key && (original.Is64bpp()!=owner.pixels_are_64_bit || bool(original.is_depth)!=owner.is_depth))return false;
        if(owner.key)needed[owner.pixels_are_64_bit]=true;
        next=owner.end_tile;
    }
    if(next!=tile_count)return false;
    if(!needed[0] && !needed[1])return true;
    struct Job {
        ScaledResolveMigrator migrator;
        ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Fence> fence;ComPtr<ID3D12Resource> source;
        std::array<ComPtr<ID3D12Resource>,2> linear,targets;
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        ~Job(){if(event)CloseHandle(event);}
    };
    auto job=std::make_unique<Job>();auto* device=cp.GetD3D12Provider().GetDevice();
    if(!job->event || !job->migrator.Initialize(device) ||
       FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&job->allocator))) ||
       FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,job->allocator.Get(),nullptr,IID_PPV_ARGS(&job->list))) ||
       FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&job->fence))))return false;
    std::array<HostTarget*,2> targets{};std::array<TargetKey,2> keys{};
    const uint64_t bytes=uint64_t(edram_bytes)*dx*dy;
    for(unsigned wide=0;wide<2;++wide)if(needed[wide]) {
        keys[wide].pitch_tiles_at_32bpp=16;
        keys[wide].resource_format=uint32_t(wide?rex::graphics::xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
            rex::graphics::xenos::ColorRenderTargetFormat::k_32_FLOAT);
        targets[wide]=static_cast<HostTarget*>((cache.*MemberOf(CreateTargetTag{}))(keys[wide]));
        if(!targets[wide])return false;
        job->targets[wide]=targets[wide]->resource();
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;
        desc.Height=desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
        desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,IID_PPV_ARGS(&job->linear[wide]))))return false;
    }
    job->source=snapshot.buffer;
    auto* list=job->list.Get();
    pixel_test::Barrier(list,snapshot.buffer.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for(const auto& owner:snapshot.owners)if(owner.key) {
        TargetKey original;original.key=owner.key;const auto msaa=unsigned(original.msaa_samples);
        const EdramScaleMigration p{sx,sy,dx,dy,owner.end_tile-owner.begin_tile,owner.pixels_are_64_bit,msaa==2?2u:1u,msaa?2u:1u};
        if(!job->migrator.RecordEdramLinear(list,snapshot.buffer.Get(),uint64_t(owner.begin_tile)*5120*sx*sy,
            job->linear[owner.pixels_are_64_bit].Get(),p,owner.begin_tile))return false;
    }
    pixel_test::Barrier(list,snapshot.buffer.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    for(unsigned wide=0;wide<2;++wide)if(needed[wide]) {
        pixel_test::Barrier(list,job->linear[wide].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        auto* target=targets[wide];
        pixel_test::Barrier(list,target->resource(),target->SetResourceState(D3D12_RESOURCE_STATE_COPY_DEST),D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=job->linear[wide].Get();source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint.Footprint={target->resource()->GetDesc().Format,1280*dx,(2048u>>wide)*dy,1,(5120u<<wide)*dx};
        D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=target->resource();destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
    }
    if(FAILED(list->Close()))return false;
    ID3D12CommandList* lists[]={list};auto* queue=cp.GetD3D12Provider().GetDirectQueue();
    queue->ExecuteCommandLists(1,lists);
    if(FAILED(queue->Signal(job->fence.Get(),1)) || FAILED(job->fence->SetEventOnCompletion(1,job->event)) ||
       WaitForSingleObject(job->event,30000)!=WAIT_OBJECT_0 || FAILED(device->GetDeviceRemovedReason())) {
        // Keep submitted objects alive even if the queue cannot prove completion.
        job.release();return false;
    }
    for(const auto& owner:snapshot.owners)if(owner.key)
        (cache.*MemberOf(ChangeOwnerTag{}))(keys[owner.pixels_are_64_bit],owner.begin_tile,owner.end_tile-owner.begin_tile,nullptr,nullptr);
    return true;
}

bool em2::RehydratePackedEdramOwners(Em2CommandProcessor& cp,const PackedEdramSnapshot& snapshot,RT& cache) {
    struct Fp {unsigned csr=_mm_getcsr();~Fp(){_mm_setcsr(csr);}}fp;
    if(!cp.IsGraphicsWorker() || cache.GetPath()!=Base::Path::kHostRenderTargets || !cp.DrainForResourceChange())return false;
    auto valid_key=[](TargetKey key) {
        if(key.IsEmpty())return true;
        if((key.key>>26) || !key.pitch_tiles_at_32bpp || key.pitch_tiles_at_32bpp>205 || unsigned(key.msaa_samples)>2)return false;
        if(key.is_depth)return key.resource_format<=1;
        using F=rex::graphics::xenos::ColorRenderTargetFormat;
        switch(key.GetColorFormat()) {
        case F::k_8_8_8_8:case F::k_8_8_8_8_GAMMA:case F::k_2_10_10_10:
        case F::k_2_10_10_10_AS_10_10_10_10:case F::k_2_10_10_10_FLOAT:
        case F::k_2_10_10_10_FLOAT_AS_16_16_16_16:case F::k_16_16:case F::k_16_16_16_16:
        case F::k_16_16_FLOAT:case F::k_16_16_16_16_FLOAT:case F::k_32_FLOAT:case F::k_32_32_FLOAT:return true;
        default:return false;
        }
    };
    std::array<uint32_t,tile_count> desired{},canonical{},depth_unorm{},depth_float{};std::vector<TargetKey> keys;
    uint32_t next=0;
    for(const auto& owner:snapshot.owners) {
        if(owner.begin_tile!=next || owner.end_tile<=next || owner.end_tile>tile_count)return false;
        TargetKey key;key.key=owner.key;
        if(!valid_key(key) || (owner.key && (key.Is64bpp()!=owner.pixels_are_64_bit || bool(key.is_depth)!=owner.is_depth)))return false;
        for(uint32_t auxiliary:{owner.depth_unorm_key,owner.depth_float_key})if(auxiliary) {
            TargetKey extra;extra.key=auxiliary;
            const auto& targets=cache.*MemberOf(TargetsTag{});const auto found=targets.find(extra);
            if(!valid_key(extra) || !extra.is_depth || found==targets.end() || !found->second)return false;
        }
        TargetKey source;if(owner.key) {
            source.pitch_tiles_at_32bpp=16;source.resource_format=uint32_t(owner.pixels_are_64_bit?
                rex::graphics::xenos::ColorRenderTargetFormat::k_32_32_FLOAT:rex::graphics::xenos::ColorRenderTargetFormat::k_32_FLOAT);
            if(std::find(keys.begin(),keys.end(),key)==keys.end())keys.push_back(key);
        }
        std::fill(desired.begin()+owner.begin_tile,desired.begin()+owner.end_tile,owner.key);
        std::fill(canonical.begin()+owner.begin_tile,canonical.begin()+owner.end_tile,source.key);
        std::fill(depth_unorm.begin()+owner.begin_tile,depth_unorm.begin()+owner.end_tile,owner.depth_unorm_key);
        std::fill(depth_float.begin()+owner.begin_tile,depth_float.begin()+owner.end_tile,owner.depth_float_key);next=owner.end_tile;
    }
    if(next!=tile_count)return false;
    auto matches=[&](const auto& expected) {
        uint32_t position=0;
        for(const auto& [begin,owner]:cache.*MemberOf(OwnersTag{})) {
            if(begin!=position || owner.end_tiles<=begin || owner.end_tiles>tile_count)return false;
            for(uint32_t tile=begin;tile<owner.end_tiles;++tile)if(owner.render_target.key!=expected[tile] ||
                owner.host_depth_render_target_unorm24.key!=depth_unorm[tile] || owner.host_depth_render_target_float24.key!=depth_float[tile])return false;
            position=owner.end_tiles;
        }
        return position==tile_count;
    };
    if(!matches(canonical))return false;
    std::vector<BaseTarget*> targets;targets.reserve(keys.size());
    // Finish all original-target allocations before changing ownership metadata.
    for(auto key:keys) {
        auto* target=(cache.*MemberOf(CreateTargetTag{}))(key);if(!target)return false;targets.push_back(target);
    }
    if(!keys.empty()) {
        if(!(cp.*MemberOf(BeginTag{}))(false))return false;cache.BeginSubmission();
        for(size_t i=0;i<keys.size();++i) {
            std::vector<NativeTransfer> transfers;
            for(const auto& owner:snapshot.owners)if(owner.key==keys[i].key)
                (cache.*MemberOf(ChangeOwnerTag{}))(keys[i],(owner.begin_tile+tile_count-keys[i].base_tiles)&(tile_count-1),
                    owner.end_tile-owner.begin_tile,&transfers,nullptr);
            if(transfers.empty())continue;
            // The native rectangle builder requires each transfer to stay on
            // one side of the destination base. ChangeOwnership can merge
            // adjacent spans across that base, so split again before drawing.
            std::vector<NativeTransfer> split;split.reserve(transfers.size()+1);
            for(const auto& transfer:transfers) {
                if(transfer.start_tiles<keys[i].base_tiles && transfer.end_tiles>keys[i].base_tiles) {
                    split.emplace_back(transfer.start_tiles,uint32_t(keys[i].base_tiles),transfer.source,transfer.host_depth_source);
                    split.emplace_back(uint32_t(keys[i].base_tiles),transfer.end_tiles,transfer.source,transfer.host_depth_source);
                }else split.push_back(transfer);
            }
            transfers=std::move(split);
            const auto& stream=cp.GetDeferredCommandList().*MemberOf(StreamTag{});const size_t start=stream.size();
            (cache.*MemberOf(TransfersTag{}))(1,&targets[i],&transfers,nullptr,nullptr);
            // Native transfer setup is void and can skip failed pipelines or
            // descriptors. Reject an entirely missing transfer submission.
            bool draw=false;size_t cursor=start;
            while(cursor<stream.size()) {
                constexpr size_t words=(sizeof(Header)+sizeof(uintmax_t)-1)/sizeof(uintmax_t);
                if(stream.size()-cursor<words)return false;
                Header header;std::memcpy(&header,stream.data()+cursor,sizeof(header));cursor+=words;
                if(header.arguments_size_elements>stream.size()-cursor)return false;
                draw|=header.command==MemberOf(DrawTag{});cursor+=header.arguments_size_elements;
            }
            if(!draw)return false;
        }
        if(!cp.DrainForResourceChange())return false;
    }
    return matches(desired);
}

static bool TestEdramBindingScope(em2::Em2CommandProcessor& cp,RT& primary) {
    using namespace em2;namespace g=rex::graphics;namespace x=g::xenos;
    auto* memory=cp.*MemberOf(MemoryTag{});auto* shared=(cp.*MemberOf(SharedTag{})).get();const bool mode=cp.*MemberOf(BindlessTag{});
    auto* heap=memory->LookupHeapByType(true,65536);if(!heap)return false;
    std::array<uint32_t,2> allocations{};
    struct Free {rex::memory::BaseHeap* heap;std::array<uint32_t,2>& addresses;~Free(){for(auto a:addresses)if(a)heap->Release(a);}}free{heap,allocations};
    for(auto& address:allocations)if(!heap->Alloc(65536,65536,rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,
        rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite,false,&address))return false;
    auto regs=std::make_unique<g::RegisterFile>();std::fill(std::begin(regs->values),std::end(regs->values),0u);
    auto trace=std::make_unique<g::TraceWriter>(memory->physical_membase());
    const unsigned scale=primary.draw_resolution_scale_x()==1?2:1;
    auto stage=std::make_unique<RT>(*regs,*memory,*trace,scale,scale,cp,mode);
    auto texture=g::d3d12::D3D12TextureCache::Create(*regs,*shared,scale,scale,cp,mode);
    struct Finish {Em2CommandProcessor& cp;std::unique_ptr<RT>& stage;decltype(texture)& texture;
        decltype(regs)& regs;decltype(trace)& trace;
        ~Finish(){if(!cp.DrainForResourceChange()){stage.release();texture.release();regs.release();trace.release();}}}finish{cp,stage,texture,regs,trace};
    if(!texture || !stage->Initialize())return false;
    TargetKey key;key.pitch_tiles_at_32bpp=16;key.resource_format=uint32_t(x::ColorRenderTargetFormat::k_32_FLOAT);
    auto* target=static_cast<HostTarget*>((stage.get()->*MemberOf(CreateTargetTag{}))(key));if(!target)return false;
    GraphicsResourceCheckpoint checkpoint(cp);if(!checkpoint.valid() || !(cp.*MemberOf(BeginTag{}))(false))return false;
    stage->BeginSubmission();(stage.get()->*MemberOf(ChangeOwnerTag{}))(key,0,tile_count,nullptr,nullptr);
    cp.PushTransitionBarrier(target->resource(),target->SetResourceState(D3D12_RESOURCE_STATE_RENDER_TARGET),D3D12_RESOURCE_STATE_RENDER_TARGET);cp.SubmitBarriers();
    const float color[]={.625f,0,0,1};cp.GetDeferredCommandList().D3DClearRenderTargetView(target->descriptor_draw().GetHandle(),color,0,nullptr);
    EdramSystemBindings bindings(cp,checkpoint,*stage);if(!bindings.valid())return false;
    EdramSystemBindings nested(cp,checkpoint,*stage);if(nested.valid() || bindings.Commit())return false;
    auto verify_buffer=[&](ID3D12Resource* buffer,uint64_t offset,uint64_t bytes) {
        ComPtr<ID3D12Resource> readback;
        if(!pixel_test::Buffer(cp.GetD3D12Provider().GetDevice(),bytes,D3D12_HEAP_TYPE_READBACK,&readback) || !(cp.*MemberOf(BeginTag{}))(false))return false;
        cp.PushTransitionBarrier(buffer,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);cp.SubmitBarriers();
        cp.GetDeferredCommandList().D3DCopyBufferRegion(readback.Get(),0,buffer,offset,bytes);
        cp.PushTransitionBarrier(buffer,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);cp.SubmitBarriers();
        if(!cp.DrainForResourceChange()){buffer->AddRef();readback.Detach();return false;}
        void* data=nullptr;D3D12_RANGE read{0,size_t(bytes)},none{0,0};if(FAILED(readback->Map(0,&read,&data)))return false;
        bool good=true;for(uint64_t i=0;i<bytes/4;++i)good&=static_cast<uint32_t*>(data)[i]==std::bit_cast<uint32_t>(.625f);
        readback->Unmap(0,&none);return good;
    };
    PackedEdramSnapshot packed;if(!CapturePackedEdram(cp,*stage,packed) || !verify_buffer(packed.buffer.Get(),0,uint64_t(edram_bytes)*scale*scale))return false;
    const auto vertices=allocations[0]&0x1FFFFFFF,destination=allocations[1]&0x1FFFFFFF;
    const float triangle[]={-.5f,-.5f,31.5f,-.5f,-.5f,31.5f};std::memcpy(memory->TranslatePhysical(vertices),triangle,sizeof(triangle));
    auto set=[&]<class T>(T value){regs->values[T::register_index]=value.value;};
    x::xe_gpu_vertex_fetch_t fetch{};fetch.type=x::FetchConstantType::kVertex;fetch.size=6;fetch.address=vertices/4;
    std::memcpy(&regs->values[g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],&fetch,sizeof(fetch));
    g::reg::PA_SC_SCREEN_SCISSOR_BR screen{};screen.br_x=screen.br_y=32;set(screen);
    g::reg::PA_SC_WINDOW_SCISSOR_TL top{};top.window_offset_disable=1;set(top);
    g::reg::PA_SC_WINDOW_SCISSOR_BR window{};window.br_x=window.br_y=32;set(window);
    g::reg::RB_SURFACE_INFO surface{};surface.surface_pitch=1280;set(surface);
    g::reg::RB_COLOR_INFO color_info{};color_info.color_format=x::ColorRenderTargetFormat::k_32_FLOAT;regs->values[g::XE_GPU_REG_RB_COLOR_INFO]=color_info.value;
    g::reg::RB_COPY_CONTROL control{};control.copy_command=x::CopyCommand::kRaw;set(control);
    g::reg::RB_COPY_DEST_INFO dest{};dest.copy_dest_format=x::ColorFormat(x::TextureFormat::k_32_FLOAT);set(dest);
    g::reg::RB_COPY_DEST_PITCH pitch{};pitch.copy_dest_pitch=pitch.copy_dest_height=32;set(pitch);regs->values[g::XE_GPU_REG_RB_COPY_DEST_BASE]=destination;
    ResolveRangeLedger ledger;ResolveWriteObserver observer{&ledger,[](void* p,const ResolveWriteObservation& event) noexcept {static_cast<ResolveRangeLedger*>(p)->Record(event);}};
    uint32_t written=0,length=0;
    if(!(cp.*MemberOf(BeginTag{}))(false))return false;stage->BeginSubmission();
    {ResolveWriteScope scope(texture.get(),&observer);if(!stage->Resolve(*memory,*shared,*texture,written,length) || !length)return false;}
    ResolvedStorageSnapshot resolved;if(!CaptureResolvedStorage(cp,*texture,*shared,ledger,resolved) || resolved.spans.empty())return false;
    for(const auto& span:resolved.spans)if(!verify_buffer(resolved.buffers[span.buffer_index].Get(),span.offset,uint64_t(span.native.length)*scale*scale))return false;
    if(!bindings.Restore() || bindings.valid())return false;
    {EdramSystemBindings automatic(cp,checkpoint,*stage);if(!automatic.valid())return false;}
    REXLOG_INFO("EDRAM system binding staging passed: bindless={}, scale={}, native packed dump/UAV and real resolve/SRV to independent storage match every word; nested binding and premature commit reject, explicit rollback completed",mode,scale);
    return true;
}

bool em2::TestAuxiliaryDepthCapture(Em2CommandProcessor& cp) {
    struct Fp {unsigned csr=_mm_getcsr();~Fp(){_mm_setcsr(csr);}}fp;
    if(!cp.IsGraphicsWorker())return false;
    const auto old_path=REXCVAR_GET(render_target_path_d3d12);
    const auto old_convert=REXCVAR_GET(depth_float24_convert_in_pixel_shader);
    struct Restore {std::string path;bool convert;~Restore(){REXCVAR_SET(render_target_path_d3d12,path);REXCVAR_SET(depth_float24_convert_in_pixel_shader,convert);}}restore{old_path,old_convert};
    REXCVAR_SET(render_target_path_d3d12,std::string("rtv"));
    REXCVAR_SET(depth_float24_convert_in_pixel_shader,false);
    constexpr uint32_t bits_a=0x3EAAAAAB,bits_b=0x3F123457;
    namespace x=rex::graphics::xenos;
    for(const auto bits:{bits_a,bits_b})for(const bool round:{false,true})
        if(std::bit_cast<uint32_t>(x::Float20e4To32(x::Float32To20e4(std::bit_cast<float>(bits),round)))==bits)return false;
    auto reader=std::make_shared<depth_test::Reader>();if(!reader->Initialize(cp.GetD3D12Provider().GetDevice()))return false;
    auto painter=std::make_shared<depth_test::Painter>();if(!painter->Initialize(cp.GetD3D12Provider().GetDevice()))return false;
    const auto guest_frame=cp.GetCurrentFrame();unsigned tested=0,restores=0,probes_checked=0;
    for(unsigned scale:{1u,2u,3u})for(unsigned msaa=0;msaa<3;++msaa) {
        auto regs=std::make_unique<rex::graphics::RegisterFile>();auto* memory=cp.*MemberOf(MemoryTag{});
        auto trace=std::make_unique<rex::graphics::TraceWriter>(memory->physical_membase());
        auto cache=std::make_unique<RT>(*regs,*memory,*trace,scale,scale,cp,false);
        struct Finish {Em2CommandProcessor& cp;std::unique_ptr<RT>& cache;std::unique_ptr<rex::graphics::TraceWriter>& trace;
            std::unique_ptr<rex::graphics::RegisterFile>& regs;
            ~Finish(){if(!cp.DrainForResourceChange()){cache.release();trace.release();regs.release();}}}finish{cp,cache,trace,regs};
        if(!cache->Initialize())return false;
        AuxiliaryDepthSnapshot empty;if(!CaptureAuxiliaryDepth(cp,*cache,empty) || !empty.targets.empty())return false;
        TargetKey key;key.pitch_tiles_at_32bpp=16;key.is_depth=1;key.resource_format=uint32_t(x::DepthRenderTargetFormat::kD24FS8);
        key.msaa_samples=static_cast<x::MsaaSamples>(msaa);
        auto* target=static_cast<HostTarget*>((cache.get()->*MemberOf(CreateTargetTag{}))(key));if(!target)return false;
        (cache.get()->*MemberOf(ChangeOwnerTag{}))(key,0,tile_count,nullptr,nullptr);
        TargetKey color_key;color_key.pitch_tiles_at_32bpp=16;color_key.resource_format=uint32_t(x::ColorRenderTargetFormat::k_32_FLOAT);
        if(!(cache.get()->*MemberOf(CreateTargetTag{}))(color_key))return false;
        (cache.get()->*MemberOf(ChangeOwnerTag{}))(color_key,123,500,nullptr,nullptr);
        auto paint=[&](uint32_t bits,uint8_t stencil) {
            if(!(cp.*MemberOf(BeginTag{}))(false))return false;
            cp.PushTransitionBarrier(target->resource(),target->SetResourceState(D3D12_RESOURCE_STATE_DEPTH_WRITE),D3D12_RESOURCE_STATE_DEPTH_WRITE);cp.SubmitBarriers();
            cp.GetDeferredCommandList().D3DClearDepthStencilView(target->descriptor_draw().GetHandle(),D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL,
                std::bit_cast<float>(bits),stencil,0,nullptr);return true;
        };
        AuxiliaryDepthSnapshot a,b;
        if(!paint(bits_a,0xA5) || !CaptureAuxiliaryDepth(cp,*cache,a) || a.targets.size()!=1 || a.targets[0].key!=key.key ||
           a.owners.size()!=3 || a.owners[1].key!=color_key.key || a.owners[1].depth_float_key!=key.key)return false;
        const auto* retained=a.targets[0].texture.Get();const auto epoch=a.completed_submission;
        auto& owners=cache.get()->*MemberOf(OwnersTag{});const auto saved=owners;TargetKey missing=key;missing.base_tiles=1;
        owners.begin()->second.host_depth_render_target_float24=missing;
        const auto target_count=(cache.get()->*MemberOf(TargetsTag{})).size();
        const bool invalid_accepted=CaptureAuxiliaryDepth(cp,*cache,a);owners=saved;
        if(invalid_accepted || a.targets[0].texture.Get()!=retained || a.completed_submission!=epoch ||
           (cache.get()->*MemberOf(TargetsTag{})).size()!=target_count)return false;
        if(!paint(bits_b,0x3C) || !CaptureAuxiliaryDepth(cp,*cache,b) || b.targets.size()!=1 || b.targets[0].texture.Get()==retained ||
           !depth_test::ReadSnapshots(cp.GetD3D12Provider().GetDevice(),cp.GetD3D12Provider().GetDirectQueue(),reader,a,b,bits_a,bits_b))return false;
        const D3D12_RECT detail{LONG(13*scale+1),LONG(9*scale+1),LONG(73*scale+2),LONG(45*scale+1)};
        if(!cp.DrainForResourceChange() || !depth_test::PaintPattern(cp.GetD3D12Provider().GetDevice(),cp.GetD3D12Provider().GetDirectQueue(),painter,
            target->resource(),target->descriptor_draw().GetHandle(),target->SetResourceState(D3D12_RESOURCE_STATE_DEPTH_WRITE),detail))return false;
        AuxiliaryDepthSnapshot patterned;if(!CaptureAuxiliaryDepth(cp,*cache,patterned))return false;
        for(unsigned destination_scale:{1u,2u,3u}) {
            auto replacement=std::make_unique<RT>(*regs,*memory,*trace,destination_scale,destination_scale,cp,false);
            struct FinishReplacement {Em2CommandProcessor& cp;std::unique_ptr<RT>& cache;
                ~FinishReplacement(){if(!cp.DrainForResourceChange())cache.release();}}finish_replacement{cp,replacement};
            if(!replacement->Initialize() || !(replacement.get()->*MemberOf(CreateTargetTag{}))(color_key))return false;
            (replacement.get()->*MemberOf(ChangeOwnerTag{}))(color_key,0,tile_count,nullptr,nullptr);
            auto malformed=patterned;malformed.targets.clear();
            if(RestoreAuxiliaryDepth(cp,malformed,*replacement) || !RestoreAuxiliaryDepth(cp,patterned,*replacement))return false;
            AuxiliaryDepthSnapshot restored;
            if(!CaptureAuxiliaryDepth(cp,*replacement,restored) || restored.targets.size()!=1 || restored.targets[0].key!=key.key || restored.owners.size()!=3)return false;
            for(const auto& owner:restored.owners)if(owner.key!=color_key.key || owner.depth_float_key!=key.key)return false;
            const auto destination_desc=restored.targets[0].texture->GetDesc();
            std::vector<std::array<unsigned,2>> coordinates={{0,0},{unsigned(destination_desc.Width)-1,destination_desc.Height-1}};
            for(const auto edge:{detail.left,detail.right})for(int delta:{-1,0,1})
                coordinates.push_back({unsigned(int(edge*destination_scale/scale)+delta),unsigned((detail.top+detail.bottom)*destination_scale/(2*scale))});
            for(const auto edge:{detail.top,detail.bottom})for(int delta:{-1,0,1})
                coordinates.push_back({unsigned((detail.left+detail.right)*destination_scale/(2*scale)),unsigned(int(edge*destination_scale/scale)+delta)});
            for(const auto& coordinate:coordinates) {
                const auto source_x=(uint64_t(coordinate[0])*2+1)*scale/(destination_scale*2);
                const auto source_y=(uint64_t(coordinate[1])*2+1)*scale/(destination_scale*2);
                const bool inside=source_x>=unsigned(detail.left) && source_x<unsigned(detail.right) && source_y>=unsigned(detail.top) && source_y<unsigned(detail.bottom);
                const unsigned depth_bits=inside?bits_b:bits_a,stencil=inside?0x3C:0xA5;
                const std::array<depth_test::Probe,2> probes={depth_test::Probe{unsigned(source_x),unsigned(source_y),depth_bits,stencil,17,0x31},
                    depth_test::Probe{coordinate[0],coordinate[1],depth_bits,stencil,17,0x31}};
                if(!depth_test::ReadSnapshots(cp.GetD3D12Provider().GetDevice(),cp.GetD3D12Provider().GetDirectQueue(),reader,patterned,restored,0,0,&probes)) {
                    REXLOG_ERROR("Auxiliary restore probe mismatch: {} -> {}, msaa={}, pixel={},{}",scale,destination_scale,msaa,coordinate[0],coordinate[1]);return false;
                }
                ++probes_checked;
            }
            ++restores;
        }
        if(scale==2 && msaa==2) {
            // A real packed+auxiliary transaction must agree on MSAA sample
            // identity before the SDK compares float24 bits to host float32.
            (cache.get()->*MemberOf(ChangeOwnerTag{}))(key,0,tile_count,nullptr,nullptr);
            PackedEdramSnapshot packed;AuxiliaryDepthSnapshot extra;
            if(!CapturePackedEdram(cp,*cache,packed) || !CaptureAuxiliaryDepth(cp,*cache,extra))return false;
            auto replacement=std::make_unique<RT>(*regs,*memory,*trace,3,3,cp,false);
            struct FinishCombined {Em2CommandProcessor& cp;std::unique_ptr<RT>& cache;
                ~FinishCombined(){if(!cp.DrainForResourceChange())cache.release();}}finish_combined{cp,replacement};
            if(!replacement->Initialize() || !RestorePackedEdram(cp,packed,*replacement))return false;
            const auto count=(replacement.get()->*MemberOf(TargetsTag{})).size();
            if(RehydratePackedEdramOwners(cp,packed,*replacement) || (replacement.get()->*MemberOf(TargetsTag{})).size()!=count ||
               !RestoreAuxiliaryDepth(cp,extra,*replacement))return false;
            GraphicsResourceCheckpoint checkpoint(cp);if(!checkpoint.valid())return false;
            PipelineShaderSnapshot shaders;if(!CapturePipelineShaders(cp,shaders))return false;
            RendererGraph graph;
            struct RetainGraph {Em2CommandProcessor& cp;RendererGraph& graph;
                ~RetainGraph(){if(!cp.DrainForResourceChange()){graph.pipelines.release();graph.textures.release();graph.targets.release();}}}retain_graph{cp,graph};
            RendererStorageSnapshot storage;storage.packed=packed;storage.auxiliary=extra;
            storage.resolved.scale_x=storage.resolved.scale_y=scale;
            ResolveRangeLedger restored_metadata;
            if(!BuildRendererGraph(cp,checkpoint,shaders,3,3,graph))return false;
            auto mismatched=storage;mismatched.auxiliary.scale_x=1;
            if(RestoreRendererGraph(cp,checkpoint,mismatched,graph,restored_metadata) || restored_metadata.page_count())return false;
            if(!RestoreRendererGraph(cp,checkpoint,std::move(storage),graph,restored_metadata) || restored_metadata.scale_x()!=3 ||
               restored_metadata.scale_y()!=3)return false;
            AuxiliaryDepthSnapshot rehydrated;if(!CaptureAuxiliaryDepth(cp,*graph.targets,rehydrated))return false;
            const std::array<depth_test::Probe,2> probes={depth_test::Probe{1,1,bits_a,0xA5,17,0x31},depth_test::Probe{1,1,bits_a,0xA5,17,0x31}};
            if(!depth_test::ReadSnapshots(cp.GetD3D12Provider().GetDevice(),cp.GetD3D12Provider().GetDirectQueue(),reader,extra,rehydrated,0,0,&probes)) {
                REXLOG_ERROR("Combined graph restore disagrees with native float-depth rehydration");return false;
            }
        }
        ++tested;
    }
    if(cp.GetCurrentFrame()!=guest_frame)return false;
    REXLOG_INFO("Auxiliary host depth capture passed: {} native D24FS8 cases, 1/2/3x and 1/2/4x MSAA, exact float32 bits beyond rounded/truncated guest float24, stencil, historical ownership, deduplication, immutable repaint snapshots, missing-source rejection and unchanged guest frame",tested);
    REXLOG_INFO("Auxiliary depth restoration passed: {} cases across all 1/2/3x scale pairs, {} spatial probes of distinct per-sample depth/stencil data, scaled rectangle boundaries, historical reference attachment and unchanged primary owners; full renderer migration not connected",restores,probes_checked);
    return true;
}

bool em2::TestPackedEdramCapture(Em2CommandProcessor& cp) {
    if(!cp.IsGraphicsWorker())return false;
    const auto old_path=REXCVAR_GET(render_target_path_d3d12);
    struct Restore {std::string path;~Restore(){REXCVAR_SET(render_target_path_d3d12,path);}}restore{old_path};
    REXCVAR_SET(render_target_path_d3d12,std::string("rtv"));
    // Decoder rejection tests use actual SDK command records, never execute
    // them, and require no guess about the pinned command-header layout.
    Deferred sample(cp);sample.D3DDispatch(4,2,1);sample.D3DDispatch(8,3,1);
    auto words=sample.*MemberOf(StreamTag{});std::vector<Dispatch> decoded;
    if(!ReadDispatches(words,decoded) || decoded!=std::vector<Dispatch>{{4,2,1},{8,3,1}})return false;
    Header bad;std::memcpy(&bad,words.data(),sizeof(bad));bad.arguments_size_elements=UINT32_MAX;
    std::memcpy(words.data(),&bad,sizeof(bad));decoded.clear();if(ReadDispatches(words,decoded))return false;
    unsigned tested=0;
    const auto guest_frame=cp.GetCurrentFrame();
    for(unsigned scale:{1u,2u,3u})for(unsigned format=0;format<5;++format)for(unsigned msaa=0;msaa<3;++msaa) {
        if(format==4 && msaa)continue; // NaN payloads use a direct source-texel copy (non-MSAA).
        auto regs=std::make_unique<rex::graphics::RegisterFile>();
        auto* memory=cp.*MemberOf(MemoryTag{});
        auto trace=std::make_unique<rex::graphics::TraceWriter>(memory->physical_membase());
        auto cache=std::make_unique<RT>(*regs,*memory,*trace,scale,scale,cp,false);
        struct Finish {
            Em2CommandProcessor& cp;std::unique_ptr<RT>& cache;
            std::unique_ptr<rex::graphics::TraceWriter>& trace;std::unique_ptr<rex::graphics::RegisterFile>& regs;
            ~Finish(){if(!cp.DrainForResourceChange()){cache.release();trace.release();regs.release();}}
        }finish{cp,cache,trace,regs};
        if(!cache->Initialize())return false;
        namespace x=rex::graphics::xenos;
        const auto color_format=format==0?x::ColorRenderTargetFormat::k_32_FLOAT:
            format==1?x::ColorRenderTargetFormat::k_32_32_FLOAT:x::ColorRenderTargetFormat::k_8_8_8_8;
        TargetKey key;key.pitch_tiles_at_32bpp=16;key.is_depth=format==3;
        key.resource_format=format==3?uint32_t(x::DepthRenderTargetFormat::kD24S8):uint32_t(color_format);
        key.msaa_samples=static_cast<x::MsaaSamples>(msaa);
        // Format4 tests the native integer-alias dump of float NaN payloads.
        if(format==4)key.resource_format=uint32_t(x::ColorRenderTargetFormat::k_32_FLOAT);
        auto* target=static_cast<HostTarget*>(((*cache).*MemberOf(CreateTargetTag{}))(key));
        if(!target)return false;
        ((*cache).*MemberOf(ChangeOwnerTag{}))(key,0,tile_count,nullptr,nullptr);
        const uint32_t samples_x=msaa==2?2:1,samples_y=msaa?2:1;
        const auto desc=target->resource()->GetDesc();
        const uint32_t used_height=tile_count/key.GetPitchTiles()*16*scale/samples_y;
        const std::array<D3D12_RECT,2> regions={{{5,7,LONG(123*scale+1),LONG(41*scale-2)},
            {LONG(desc.Width-127*scale-3),LONG(used_height-29*scale-1),LONG(desc.Width-2),LONG(used_height-3)}}};
        std::array<float,3> base_a={.25f,.5f,.75f},detail_a={.125f,.25f,.875f};
        std::array<float,3> base_b={.75f,.25f,.5f},detail_b={.875f,.75f,.125f};
        if(format==2 && msaa){base_a={1,0,1};detail_a={0,1,0};base_b={0,1,1};detail_b={1,0,0};}
        if(format==4){base_a[0]=std::bit_cast<float>(0x7FC12345u);detail_a[0]=std::bit_cast<float>(0xFFC12345u);
            base_b[0]=std::bit_cast<float>(0x7FC54321u);detail_b[0]=std::bit_cast<float>(0xFFC54321u);}
        auto paint=[&](const std::array<float,3>& base,const std::array<float,3>& detail,bool second) {
            if(!(cp.*MemberOf(BeginTag{}))(false))return false;
            const auto state=format==3?D3D12_RESOURCE_STATE_DEPTH_WRITE:D3D12_RESOURCE_STATE_RENDER_TARGET;
            cp.PushTransitionBarrier(target->resource(),target->SetResourceState(state),state);cp.SubmitBarriers();
            if(format==3) {
                cp.GetDeferredCommandList().D3DClearDepthStencilView(target->descriptor_draw().GetHandle(),D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL,
                    second?1.f:0.f,second?0x33:0x5A,0,nullptr);
                cp.GetDeferredCommandList().D3DClearDepthStencilView(target->descriptor_draw().GetHandle(),D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL,
                    second?0.f:1.f,second?0xCC:0xA5,2,regions.data());
            }else {
                const float color[4]={base[0],base[1],base[2],1.f},pattern[4]={detail[0],detail[1],detail[2],1.f};
                cp.GetDeferredCommandList().D3DClearRenderTargetView(target->descriptor_draw().GetHandle(),color,0,nullptr);
                cp.GetDeferredCommandList().D3DClearRenderTargetView(target->descriptor_draw().GetHandle(),pattern,2,regions.data());
            }
            return true;
        };
        auto read_host_pixel=[&](uint32_t& value,uint32_t x,uint32_t y) {
            // ClearRenderTargetView UNORM rounding is device-dependent at
            // half-way values. Compare native dumping with the actual source
            // texel bytes, copied without using the dump shader.
            ComPtr<ID3D12Resource> host_pixel;
            if(!pixel_test::Buffer(cp.GetD3D12Provider().GetDevice(),256,D3D12_HEAP_TYPE_READBACK,&host_pixel) ||
               !(cp.*MemberOf(BeginTag{}))(false))return false;
            cp.PushTransitionBarrier(target->resource(),target->SetResourceState(D3D12_RESOURCE_STATE_COPY_SOURCE),D3D12_RESOURCE_STATE_COPY_SOURCE);
            cp.SubmitBarriers();
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=target->resource();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource=host_pixel.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint.Footprint={format==2?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_R32_FLOAT,1,1,1,256};
            const D3D12_BOX box{x,y,0,x+1,y+1,1};cp.GetDeferredCommandList().D3DCopyTextureRegion(&to,0,0,0,&from,&box);
            if(!cp.DrainForResourceChange()){host_pixel.Detach();return false;}
            void* data=nullptr;D3D12_RANGE range{0,4};if(FAILED(host_pixel->Map(0,&range,&data)))return false;
            std::memcpy(&value,data,4);D3D12_RANGE none{0,0};host_pixel->Unmap(0,&none);return true;
        };
        PackedEdramSnapshot first,second;
        uint32_t host_a=0xFFFF00FF,host_b=0xFFFFFF00,host_detail_a=0xFF00FF00,host_detail_b=0xFF0000FF;
        const bool direct_host=(format==2 || format==4) && !msaa;
        if(!paint(base_a,detail_a,false) || (direct_host && (!read_host_pixel(host_a,0,0) ||
            !read_host_pixel(host_detail_a,regions[0].left,regions[0].top))) ||
           !CapturePackedEdram(cp,*cache,first) || !first.buffer || first.scale_x!=scale ||
           first.owners.empty() || first.owners.front().begin_tile!=0 || first.owners.back().end_tile!=tile_count ||
           first.owners.front().pixels_are_64_bit!=(format==1) || first.owners.front().is_depth!=(format==3) || first.owners.front().key!=key.key)return false;
        const auto* retained=first.buffer.Get();const auto epoch=first.completed_submission;
        if(!paint(base_b,detail_b,true) || (direct_host && (!read_host_pixel(host_b,0,0) ||
            !read_host_pixel(host_detail_b,regions[0].left,regions[0].top))) ||
           Capture(cp,*cache,first,false) || first.buffer.Get()!=retained || first.completed_submission!=epoch ||
           !CapturePackedEdram(cp,*cache,second) || second.buffer.Get()==first.buffer.Get())return false;
        if(format==4) {
            const auto nan=[](uint32_t value){return (value&0x7F800000)==0x7F800000 && (value&0x007FFFFF);};
            if(!nan(host_a) || !nan(host_b) || !nan(host_detail_a) || !nan(host_detail_b))return false;
        }
        const uint64_t bytes=uint64_t(edram_bytes)*scale*scale;
        ComPtr<ID3D12Resource> readback;
        if(!pixel_test::Buffer(cp.GetD3D12Provider().GetDevice(),bytes*2,D3D12_HEAP_TYPE_READBACK,&readback) ||
           !(cp.*MemberOf(BeginTag{}))(false))return false;
        cp.PushTransitionBarrier(first.buffer.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
        cp.PushTransitionBarrier(second.buffer.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);cp.SubmitBarriers();
        cp.GetDeferredCommandList().D3DCopyBufferRegion(readback.Get(),0,first.buffer.Get(),0,bytes);
        cp.GetDeferredCommandList().D3DCopyBufferRegion(readback.Get(),bytes,second.buffer.Get(),0,bytes);
        if(!cp.DrainForResourceChange()){readback.Detach();first.buffer.Detach();second.buffer.Detach();return false;}
        void* mapped=nullptr;D3D12_RANGE range{0,size_t(bytes*2)};
        if(FAILED(readback->Map(0,&range,&mapped)))return false;
        bool valid=true;const auto* data=static_cast<const uint32_t*>(mapped);uint64_t first_bad=UINT64_MAX;
        for(uint64_t i=0;i<bytes/4;++i) {
            const uint32_t tile=uint32_t(i/(1280*scale*scale)),local=uint32_t(i%(1280*scale*scale));
            uint32_t xword=local%(80*scale);if(format==3)xword=xword<40*scale?xword+40*scale:xword-40*scale;
            const uint32_t words_per_pixel=format==1?2:1;
            const uint32_t x=(tile%key.GetPitchTiles())*(80*scale/words_per_pixel/samples_x)+xword/words_per_pixel/samples_x;
            const uint32_t y=(tile/key.GetPitchTiles())*(16*scale/samples_y)+(local/(80*scale))/samples_y;
            bool detail=false;for(const auto& r:regions)detail|=x>=uint32_t(r.left) && x<uint32_t(r.right) && y>=uint32_t(r.top) && y<uint32_t(r.bottom);
            uint32_t a,b;
            if(format==3){a=detail?0xFFFFFFA5:0x0000005A;b=detail?0x000000CC:0xFFFFFF33;}
            else if(format==2 || format==4){a=detail?host_detail_a:host_a;b=detail?host_detail_b:host_b;}
            else {const auto c=format==1?(xword&1):0;a=std::bit_cast<uint32_t>((detail?detail_a:base_a)[c]);b=std::bit_cast<uint32_t>((detail?detail_b:base_b)[c]);}
            if(data[i]!=a || data[bytes/4+i]!=b){if(first_bad==UINT64_MAX)first_bad=i;valid=false;}
        }
        if(!valid)REXLOG_ERROR("Native capture first mismatch word={} a={:08X} b={:08X}",first_bad,data[first_bad],data[bytes/4+first_bad]);
        std::vector<uint32_t> original(data,data+bytes/4);
        D3D12_RANGE none{0,0};readback->Unmap(0,&none);
        if(!valid){REXLOG_ERROR("Native packed EDRAM capture mismatch: scale={} format={} msaa={}",scale,format,msaa);return false;}
        if(!(cp.*MemberOf(BeginTag{}))(false))return false;
        cp.PushTransitionBarrier(first.buffer.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);cp.SubmitBarriers();
        if(!cp.DrainForResourceChange() || RestorePackedEdram(cp,first,*cache))return false; // Reject a nonempty destination.
        // Equivalent fragmented ownership forces nonzero source offsets and
        // canonical destinations starting inside a native texture tile row.
        auto restoration_source=first;
        auto tail=restoration_source.owners.front();tail.begin_tile=1001;
        restoration_source.owners.front().end_tile=1001;restoration_source.owners.push_back(tail);
        for(unsigned destination_scale:{1u,2u,3u}) {
            auto replacement=std::make_unique<RT>(*regs,*memory,*trace,destination_scale,destination_scale,cp,false);
            struct FinishReplacement {Em2CommandProcessor& cp;std::unique_ptr<RT>& cache;
                ~FinishReplacement(){if(!cp.DrainForResourceChange())cache.release();}}finish_replacement{cp,replacement};
            if(!replacement->Initialize())return false;
            auto malformed=first;malformed.owners.front().begin_tile=1;
            if(RestorePackedEdram(cp,malformed,*replacement) || !RestorePackedEdram(cp,restoration_source,*replacement))return false;
            PackedEdramSnapshot restored;
            if(!CapturePackedEdram(cp,*replacement,restored) || restored.owners.empty() ||
               restored.owners.front().pixels_are_64_bit!=(format==1))return false;
            // Exercise the SDK's real ownership-transfer draw shaders, not a
            // second implementation of color/MSAA/depth conversion. Afterwards
            // capture must originate from the original guest format and MSAA.
            auto invalid_owner=restoration_source;invalid_owner.owners.front().key|=0x80000000u;
            if(RehydratePackedEdramOwners(cp,invalid_owner,*replacement) || !RehydratePackedEdramOwners(cp,restoration_source,*replacement))return false;
            PackedEdramSnapshot original_format;
            if(!CapturePackedEdram(cp,*replacement,original_format) || original_format.owners.empty() ||
               original_format.owners.front().key!=key.key)return false;
            const uint64_t restored_bytes=uint64_t(edram_bytes)*destination_scale*destination_scale;
            ComPtr<ID3D12Resource> restored_readback;
            if(!pixel_test::Buffer(cp.GetD3D12Provider().GetDevice(),restored_bytes*2,D3D12_HEAP_TYPE_READBACK,&restored_readback) ||
               !(cp.*MemberOf(BeginTag{}))(false))return false;
            cp.PushTransitionBarrier(restored.buffer.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);cp.SubmitBarriers();
            cp.PushTransitionBarrier(original_format.buffer.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);cp.SubmitBarriers();
            cp.GetDeferredCommandList().D3DCopyBufferRegion(restored_readback.Get(),0,restored.buffer.Get(),0,restored_bytes);
            cp.GetDeferredCommandList().D3DCopyBufferRegion(restored_readback.Get(),restored_bytes,original_format.buffer.Get(),0,restored_bytes);
            if(!cp.DrainForResourceChange()){restored.buffer.Detach();original_format.buffer.Detach();restored_readback.Detach();return false;}
            void* restored_mapping=nullptr;D3D12_RANGE restored_range{0,size_t(restored_bytes*2)};
            if(FAILED(restored_readback->Map(0,&restored_range,&restored_mapping)))return false;
            const auto* actual=static_cast<const uint32_t*>(restored_mapping);bool matches=true;uint64_t bad=UINT64_MAX;
            const unsigned pair=format==1?2:1;
            for(uint64_t i=0;i<restored_bytes/4;++i) {
                const auto tile=i/(1280*destination_scale*destination_scale),local=i%(1280*destination_scale*destination_scale);
                const auto word_x=local%(80*destination_scale),row=local/(80*destination_scale);
                const auto pixel_sample=word_x/pair;
                const auto sample_x=(((2*(pixel_sample/samples_x)+1)*scale/(2*destination_scale))*samples_x+pixel_sample%samples_x)*pair+word_x%pair;
                const auto sample_y=((2*(row/samples_y)+1)*scale/(2*destination_scale))*samples_y+row%samples_y;
                const auto expected=original[tile*1280*scale*scale+sample_y*80*scale+sample_x];
                if(actual[i]!=expected || actual[restored_bytes/4+i]!=expected){matches=false;if(bad==UINT64_MAX)bad=i;}
            }
            restored_readback->Unmap(0,&none);
            if(!matches){REXLOG_ERROR("Packed EDRAM restore mismatch: {} -> {} format={} msaa={} word={}",scale,destination_scale,format,msaa,bad);return false;}
        }
        ++tested;
    }
    unsigned mixed_cases=0;
    for(unsigned scale:{1u,2u,3u}) {
        auto regs=std::make_unique<rex::graphics::RegisterFile>();auto* memory=cp.*MemberOf(MemoryTag{});
        auto trace=std::make_unique<rex::graphics::TraceWriter>(memory->physical_membase());
        const unsigned destination_scale=scale%3+1;
        auto source=std::make_unique<RT>(*regs,*memory,*trace,scale,scale,cp,false);
        auto destination=std::make_unique<RT>(*regs,*memory,*trace,destination_scale,destination_scale,cp,false);
        struct FinishMixed {
            Em2CommandProcessor& cp;std::unique_ptr<RT>& source;std::unique_ptr<RT>& destination;
            std::unique_ptr<rex::graphics::TraceWriter>& trace;std::unique_ptr<rex::graphics::RegisterFile>& regs;
            ~FinishMixed(){if(!cp.DrainForResourceChange()){source.release();destination.release();trace.release();regs.release();}}
        }finish{cp,source,destination,trace,regs};
        if(!source->Initialize() || !destination->Initialize() || !(cp.*MemberOf(BeginTag{}))(false))return false;
        std::array<TargetKey,2> keys{};std::array<HostTarget*,2> targets{};
        for(unsigned wide=0;wide<2;++wide) {
            keys[wide].base_tiles=wide?2031:37;keys[wide].pitch_tiles_at_32bpp=16;
            keys[wide].resource_format=uint32_t(wide?rex::graphics::xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
                rex::graphics::xenos::ColorRenderTargetFormat::k_8_8_8_8);
            targets[wide]=static_cast<HostTarget*>((source.get()->*MemberOf(CreateTargetTag{}))(keys[wide]));
            if(!targets[wide])return false;
            cp.PushTransitionBarrier(targets[wide]->resource(),targets[wide]->SetResourceState(D3D12_RESOURCE_STATE_RENDER_TARGET),D3D12_RESOURCE_STATE_RENDER_TARGET);
            cp.SubmitBarriers();const float narrow_color[]={1,0,0,1},wide_color[]={.375f,-.75f,0,0};
            cp.GetDeferredCommandList().D3DClearRenderTargetView(targets[wide]->descriptor_draw().GetHandle(),wide?wide_color:narrow_color,0,nullptr);
        }
        (source.get()->*MemberOf(ChangeOwnerTag{}))(keys[0],(2048-keys[0].base_tiles)&2047,224,nullptr,nullptr);
        (source.get()->*MemberOf(ChangeOwnerTag{}))(keys[0],(333+2048-keys[0].base_tiles)&2047,1001-333,nullptr,nullptr);
        (source.get()->*MemberOf(ChangeOwnerTag{}))(keys[1],(1001+2048-keys[1].base_tiles)&2047,2048-1001,nullptr,nullptr);
        auto verify_mixed=[&](PackedEdramSnapshot& value,unsigned snapshot_scale,const char* label) {
        const uint64_t bytes=uint64_t(edram_bytes)*snapshot_scale*snapshot_scale;ComPtr<ID3D12Resource> readback;
        if(!pixel_test::Buffer(cp.GetD3D12Provider().GetDevice(),bytes,D3D12_HEAP_TYPE_READBACK,&readback) || !(cp.*MemberOf(BeginTag{}))(false))return false;
        cp.PushTransitionBarrier(value.buffer.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);cp.SubmitBarriers();
        cp.GetDeferredCommandList().D3DCopyBufferRegion(readback.Get(),0,value.buffer.Get(),0,bytes);
        cp.PushTransitionBarrier(value.buffer.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);cp.SubmitBarriers();
        if(!cp.DrainForResourceChange()){readback.Detach();value.buffer.Detach();return false;}
        void* mapping=nullptr;D3D12_RANGE range{0,size_t(bytes)};if(FAILED(readback->Map(0,&range,&mapping)))return false;
        const auto* actual=static_cast<const uint32_t*>(mapping);bool matches=true;uint64_t bad=UINT64_MAX;uint32_t actual_bad=0,expected_bad=0;
        for(uint64_t i=0;i<bytes/4;++i) {
            const auto tile=i/(1280*snapshot_scale*snapshot_scale);if(tile>=224 && tile<333)continue;
            const uint32_t expected=tile<1001?0xFF0000FF:std::bit_cast<uint32_t>((i&1)?-.75f:.375f);
            if(actual[i]!=expected){matches=false;if(bad==UINT64_MAX){bad=i;actual_bad=actual[i];expected_bad=expected;}}
        }
        D3D12_RANGE none{0,0};readback->Unmap(0,&none);if(!matches){REXLOG_ERROR("Mixed native EDRAM {} mismatch: scale={} word={} actual={:08X} expected={:08X}",label,snapshot_scale,bad,actual_bad,expected_bad);return false;}
        return true;
        };
        PackedEdramSnapshot snapshot,restored;
        if(!CapturePackedEdram(cp,*source,snapshot) || !verify_mixed(snapshot,scale,"source capture") ||
           !RestorePackedEdram(cp,snapshot,*destination))return false;
        PackedEdramSnapshot canonical_check;
        if(!CapturePackedEdram(cp,*destination,canonical_check) || !verify_mixed(canonical_check,destination_scale,"canonical restore"))return false;
        // Rehydrate both original formats using only their owned spans. Empty
        // tiles must remain unowned throughout the complete round trip.
        if(!RehydratePackedEdramOwners(cp,snapshot,*destination))return false;
        if(!CapturePackedEdram(cp,*destination,restored) || restored.owners.size()!=4)return false;
        const std::array<uint32_t,4> begins={0,224,333,1001},ends={224,333,1001,2048},owner_keys={keys[0].key,0,keys[0].key,keys[1].key};
        for(unsigned i=0;i<4;++i)if(restored.owners[i].begin_tile!=begins[i] || restored.owners[i].end_tile!=ends[i] || restored.owners[i].key!=owner_keys[i])return false;
        if(!verify_mixed(restored,destination_scale,"original targets"))return false;
        ++mixed_cases;
    }
    // The real processor cache covers the production descriptor path as well
    // as the transient bindful caches above (including bindless system UAVs
    // when enabled on this device). The title is unstarted in this diagnostic.
    auto& primary=*(cp.*MemberOf(PrimaryTag{}));
    unsigned primary_cases=0,graph_restores=0;
    if(primary.GetPath()==Base::Path::kHostRenderTargets) {
        auto saved_owners=primary.*MemberOf(OwnersTag{});
        struct RestoreOwners {
            Em2CommandProcessor& cp;RT& cache;decltype(saved_owners)& owners;
            ~RestoreOwners(){cp.DrainForResourceChange();(cache.*MemberOf(OwnersTag{}))=std::move(owners);}
        }restore_owners{cp,primary,saved_owners};
        TargetKey key;key.pitch_tiles_at_32bpp=16;key.resource_format=uint32_t(rex::graphics::xenos::ColorRenderTargetFormat::k_32_FLOAT);
        auto* target=static_cast<HostTarget*>((primary.*MemberOf(CreateTargetTag{}))(key));
        if(!target || !(cp.*MemberOf(BeginTag{}))(false))return false;
        (primary.*MemberOf(ChangeOwnerTag{}))(key,0,tile_count,nullptr,nullptr);
        cp.PushTransitionBarrier(target->resource(),target->SetResourceState(D3D12_RESOURCE_STATE_RENDER_TARGET),D3D12_RESOURCE_STATE_RENDER_TARGET);
        cp.SubmitBarriers();const float color[4]={.375f,0,0,1};
        cp.GetDeferredCommandList().D3DClearRenderTargetView(target->descriptor_draw().GetHandle(),color,0,nullptr);
        PackedEdramSnapshot snapshot;if(!CapturePackedEdram(cp,primary,snapshot))return false;
        if(!TestEdramBindingScope(cp,primary) || !(cp.*MemberOf(BeginTag{}))(false))return false;
        cp.PushTransitionBarrier(target->resource(),target->SetResourceState(D3D12_RESOURCE_STATE_RENDER_TARGET),D3D12_RESOURCE_STATE_RENDER_TARGET);cp.SubmitBarriers();
        const float repainted[]={.8125f,0,0,1};cp.GetDeferredCommandList().D3DClearRenderTargetView(target->descriptor_draw().GetHandle(),repainted,0,nullptr);
        if(!CapturePackedEdram(cp,primary,snapshot))return false;
        const uint64_t bytes=uint64_t(edram_bytes)*snapshot.scale_x*snapshot.scale_y;
        ComPtr<ID3D12Resource> readback;
        if(!pixel_test::Buffer(cp.GetD3D12Provider().GetDevice(),bytes,D3D12_HEAP_TYPE_READBACK,&readback) ||
           !(cp.*MemberOf(BeginTag{}))(false))return false;
        cp.PushTransitionBarrier(snapshot.buffer.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);cp.SubmitBarriers();
        cp.GetDeferredCommandList().D3DCopyBufferRegion(readback.Get(),0,snapshot.buffer.Get(),0,bytes);
        if(!cp.DrainForResourceChange()){snapshot.buffer.Detach();readback.Detach();return false;}
        void* mapped=nullptr;D3D12_RANGE range{0,size_t(bytes)};if(FAILED(readback->Map(0,&range,&mapped)))return false;
        const auto* words=static_cast<const uint32_t*>(mapped);bool valid=true;
        for(uint64_t i=0;i<bytes/4;++i)valid&=words[i]==std::bit_cast<uint32_t>(.8125f);
        D3D12_RANGE none{0,0};readback->Unmap(0,&none);if(!valid)return false;
        ++primary_cases;
        GraphicsResourceCheckpoint checkpoint(cp);if(!checkpoint.valid())return false;
        auto* memory=cp.*MemberOf(MemoryTag{});auto* shared=(cp.*MemberOf(SharedTag{})).get();
        auto* heap=memory->LookupHeapByType(true,65536);if(!heap)return false;
        std::array<uint32_t,3> allocations{};
        auto* registers=cp.*MemberOf(RegistersTag{});auto saved_registers=std::make_unique<rex::graphics::RegisterFile>(*registers);
        ResolveRangeLedger saved_ledger;if(!cp.CopyTrackedResolveLedger(saved_ledger))return false;
        struct FixtureRestore {Em2CommandProcessor& cp;rex::memory::BaseHeap* heap;std::array<uint32_t,3>& allocations;
            rex::graphics::RegisterFile& registers;std::unique_ptr<rex::graphics::RegisterFile>& saved;ResolveRangeLedger& ledger;
            ~FixtureRestore(){registers=*saved;for(auto a:allocations)if(a){ledger.InvalidatePages(a&0x1FFFFFFF,(a&0x1FFFFFFF)+65535);heap->Release(a);}
                *(cp.*MemberOf(OwnedLedgerTag{}))=std::move(ledger);}}fixture_restore{cp,heap,allocations,*registers,saved_registers,saved_ledger};
        for(auto& a:allocations)if(!heap->Alloc(65536,65536,rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,
            rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite,false,&a))return false;
        if(!(cp.*MemberOf(OwnedLedgerTag{}))->SeedRanges({},primary.draw_resolution_scale_x(),primary.draw_resolution_scale_y()))return false;
        const auto vertices=allocations[0]&0x1FFFFFFF,destination_address=allocations[1]&0x1FFFFFFF;
        for(auto a:allocations)shared->MemoryInvalidationCallback(a&0x1FFFFFFF,65536,true);
        const float triangle[]={-.5f,-.5f,31.5f,-.5f,-.5f,31.5f};std::memcpy(memory->TranslatePhysical(vertices),triangle,sizeof(triangle));
        auto set=[&]<class T>(T value){registers->values[T::register_index]=value.value;};
        namespace g=rex::graphics;namespace x=g::xenos;
        x::xe_gpu_vertex_fetch_t fetch{};fetch.type=x::FetchConstantType::kVertex;fetch.size=6;fetch.address=vertices/4;
        std::memcpy(&registers->values[g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],&fetch,sizeof(fetch));
        g::reg::PA_SC_SCREEN_SCISSOR_BR screen{};screen.br_x=screen.br_y=32;set(screen);
        g::reg::PA_SC_WINDOW_SCISSOR_TL top{};top.window_offset_disable=1;set(top);
        g::reg::PA_SC_WINDOW_SCISSOR_BR window{};window.br_x=window.br_y=32;set(window);
        g::reg::RB_SURFACE_INFO surface{};surface.surface_pitch=1280;set(surface);
        g::reg::RB_COLOR_INFO color_info{};color_info.color_format=x::ColorRenderTargetFormat::k_32_FLOAT;registers->values[g::XE_GPU_REG_RB_COLOR_INFO]=color_info.value;
        g::reg::RB_COPY_CONTROL control{};control.copy_command=x::CopyCommand::kRaw;set(control);
        g::reg::RB_COPY_DEST_INFO dest{};dest.copy_dest_format=x::ColorFormat(x::TextureFormat::k_32_FLOAT);set(dest);
        g::reg::RB_COPY_DEST_PITCH pitch{};pitch.copy_dest_pitch=pitch.copy_dest_height=32;set(pitch);
        registers->values[g::XE_GPU_REG_RB_COPY_DEST_BASE]=destination_address;
        if(!(cp.*MemberOf(IssueCopyTag{}))() || !cp.DrainForResourceChange())return false;
        RendererStorageSnapshot storage;PipelineShaderSnapshot shader_source;
        if(!CaptureRendererStorage(cp,checkpoint,storage) || !CapturePipelineShaders(cp,shader_source))return false;
        if(storage.resolved.spans.empty())return false;
        for(unsigned destination:{2u,3u}) {
            RendererGraph graph;
            struct RetainGraph {Em2CommandProcessor& cp;RendererGraph& graph;
                ~RetainGraph(){if(!cp.DrainForResourceChange()){graph.pipelines.release();graph.textures.release();graph.targets.release();}}}retain_graph{cp,graph};
            ResolveRangeLedger ledger;
            if(!BuildRendererGraph(cp,checkpoint,shader_source,destination,destination,graph))return false;
            if(destination==2) {
                if(!RestoreRendererGraph(cp,checkpoint,storage,graph,ledger))return false;
            }else {
                auto consumed=storage;
                if(!RestoreRendererGraph(cp,checkpoint,std::move(consumed),graph,ledger))return false;
            }
            EdramSystemBindings bindings(cp,checkpoint,*graph.targets);if(!bindings.valid())return false;
            PackedEdramSnapshot captured;if(!CapturePackedEdram(cp,*graph.targets,captured))return false;
            const uint64_t count=uint64_t(edram_bytes)*destination*destination;
            ComPtr<ID3D12Resource> copy;
            if(!pixel_test::Buffer(cp.GetD3D12Provider().GetDevice(),count,D3D12_HEAP_TYPE_READBACK,&copy) || !(cp.*MemberOf(BeginTag{}))(false))return false;
            cp.PushTransitionBarrier(captured.buffer.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);cp.SubmitBarriers();
            cp.GetDeferredCommandList().D3DCopyBufferRegion(copy.Get(),0,captured.buffer.Get(),0,count);
            cp.PushTransitionBarrier(captured.buffer.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);cp.SubmitBarriers();
            if(!cp.DrainForResourceChange()){copy.Detach();captured.buffer.Detach();return false;}
            void* data=nullptr;D3D12_RANGE region{0,size_t(count)},none{0,0};if(FAILED(copy->Map(0,&region,&data)))return false;
            bool matched=true;for(uint64_t word=0;word<count/4;++word)matched&=static_cast<uint32_t*>(data)[word]==std::bit_cast<uint32_t>(.8125f);
            copy->Unmap(0,&none);if(!matched)return false;
            ResolvedStorageSnapshot resolved;
            if(!CaptureResolvedStorage(cp,*graph.textures,*shared,ledger,resolved) || resolved.spans.empty())return false;
            auto verify_resolved=[&](ResolvedStorageSnapshot& snapshot) {
                for(const auto& span:snapshot.spans) {
                const uint64_t size=uint64_t(span.native.length)*destination*destination;
                copy.Reset();if(!pixel_test::Buffer(cp.GetD3D12Provider().GetDevice(),size,D3D12_HEAP_TYPE_READBACK,&copy) || !(cp.*MemberOf(BeginTag{}))(false))return false;
                auto* buffer=snapshot.buffers[span.buffer_index].Get();
                cp.PushTransitionBarrier(buffer,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);cp.SubmitBarriers();
                cp.GetDeferredCommandList().D3DCopyBufferRegion(copy.Get(),0,buffer,span.offset,size);
                cp.PushTransitionBarrier(buffer,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);cp.SubmitBarriers();
                if(!cp.DrainForResourceChange()){copy.Detach();for(auto& buffer:snapshot.buffers)buffer.Detach();return false;}
                region={0,size_t(size)};if(FAILED(copy->Map(0,&region,&data)))return false;
                matched=true;for(uint64_t word=0;word<size/4;++word)matched&=static_cast<uint32_t*>(data)[word]==std::bit_cast<uint32_t>(.8125f);
                copy->Unmap(0,&none);if(!matched)return false;
            }
                return true;
            };
            if(!verify_resolved(resolved))return false;
            if(!bindings.Restore())return false;
            RendererGraph retired;
            struct RestorePublished {Em2CommandProcessor& cp;const GraphicsResourceCheckpoint& checkpoint;
                RendererGraph& retired;RendererGraph& graph;ResolveRangeLedger& ledger;
                ~RestorePublished(){if(retired.targets && !PublishRendererGraph(cp,checkpoint,retired,ledger,graph)) {
                    retired.pipelines.release();retired.textures.release();retired.targets.release();
                }}}restore_published{cp,checkpoint,retired,graph,ledger};
            auto* original_target=&primary;auto* staged_target=graph.targets.get();
            if(!PublishRendererGraph(cp,checkpoint,graph,ledger,retired) || graph.targets ||
               (cp.*MemberOf(PrimaryTag{})).get()!=staged_target || retired.targets.get()!=original_target)return false;
            ResolveRangeLedger published;
            if(!cp.CopyTrackedResolveLedger(published) || published.scale_x()!=destination || published.scale_y()!=destination ||
               !(cp.*MemberOf(IssueCopyTag{}))() || !cp.DrainForResourceChange())return false;
            RendererStorageSnapshot published_storage;
            if(!CaptureRendererStorage(cp,checkpoint,published_storage) || published_storage.packed.scale_x!=destination ||
               published_storage.resolved.scale_y!=destination || published_storage.resolved.spans.empty() ||
               !verify_resolved(published_storage.resolved))return false;
            // Cold/stale canonical baseline: native scaled writes alone must
            // leave stale AB bytes, then the production path must upload the
            // new CPU CD baseline before page validity is marked.
            const auto cold=allocations[2]&0x1FFFFFFF;
            std::memset(memory->TranslatePhysical(cold),0xAB,4096);
            shared->MemoryInvalidationCallback(cold,4096,true);
            if(!(cp.*MemberOf(BeginTag{}))(false) || !shared->RequestRange(cold,4096) || !cp.DrainForResourceChange())return false;
            std::memset(memory->TranslatePhysical(cold),0xCD,4096);
            shared->MemoryInvalidationCallback(cold,4096,true);
            const float partial_triangle[]={7.5f,7.5f,15.5f,7.5f,7.5f,15.5f};
            std::memcpy(memory->TranslatePhysical(vertices),partial_triangle,sizeof(partial_triangle));
            registers->values[g::XE_GPU_REG_RB_COPY_DEST_BASE]=cold;
            auto verify_cold=[&](uint8_t baseline,const ResolvedStorageSnapshot* restored) {
                ComPtr<ID3D12Resource> result;
                if(!pixel_test::Buffer(cp.GetD3D12Provider().GetDevice(),4096,D3D12_HEAP_TYPE_READBACK,&result) || !(cp.*MemberOf(BeginTag{}))(false))return false;
                shared->UseAsCopySource();cp.SubmitBarriers();
                cp.GetDeferredCommandList().D3DCopyBufferRegion(result.Get(),0,shared->GetBuffer(),cold,4096);
                if(!cp.DrainForResourceChange()){result.Detach();return false;}
                void* bytes=nullptr;D3D12_RANGE read{0,4096};if(FAILED(result->Map(0,&read,&bytes)))return false;
                bool okay=true;size_t covered_bytes=0;
                for(uint32_t offset=0;offset<4096;++offset) {
                    bool covered=false;if(restored)for(const auto& span:restored->spans)
                        covered|=cold+offset>=span.native.address && uint64_t(cold)+offset<uint64_t(span.native.address)+span.native.length;
                    const auto expected=covered?uint8_t(std::bit_cast<uint32_t>(.8125f)>>((offset&3)*8)):baseline;
                    covered_bytes+=covered;okay&=static_cast<const uint8_t*>(bytes)[offset]==expected;
                }
                result->Unmap(0,&none);return okay && (!restored || covered_bytes==8*8*4);
            };
            if(!cp.rex::graphics::d3d12::D3D12CommandProcessor::IssueCopy() || !cp.DrainForResourceChange() || !verify_cold(0xAB,nullptr)) {
                REXLOG_ERROR("Scaled-only baseline negative control did not retain stale canonical bytes");return false;
            }
            shared->MemoryInvalidationCallback(cold,4096,true);
            if(!(cp.*MemberOf(IssueCopyTag{}))() || !cp.DrainForResourceChange() || !verify_cold(0xCD,nullptr)) {
                REXLOG_ERROR("Production scaled resolve failed to preserve CPU baseline before marking GPU validity");return false;
            }
            RendererStorageSnapshot cold_storage;ResolvedStorageSnapshot returned;ResolveRangeLedger returned_ledger;
            if(!CaptureRendererStorage(cp,checkpoint,cold_storage) || !ConvertResolvedStorage(cp,cold_storage.resolved,1,1,returned) ||
               !RestoreResolvedStorage(cp,returned,*retired.textures,returned_ledger) || !verify_cold(0xCD,&returned)) {
                REXLOG_ERROR("Scaled-only partial resolve return to1x corrupted untouched CPU baseline");return false;
            }
            std::memcpy(memory->TranslatePhysical(vertices),triangle,sizeof(triangle));
            registers->values[g::XE_GPU_REG_RB_COPY_DEST_BASE]=destination_address;
            if(!PublishRendererGraph(cp,checkpoint,retired,ledger,graph) || retired.targets ||
               (cp.*MemberOf(PrimaryTag{})).get()!=original_target || graph.targets.get()!=staged_target ||
               !cp.CopyTrackedResolveLedger(published) || published.scale_x()!=storage.packed.scale_x)return false;
            ++graph_restores;
        }
    }
    if(cp.GetCurrentFrame()!=guest_frame)return false;
    REXLOG_INFO("Scaled-only resolve baseline passed:2x and3x actual native partial8x8 writes, bypass negative control retains staleAB, production callback uploads CPU CD before GPU marking, captured conversion/return1x verifies all4096 bytes including every untouched hole; no title");
    REXLOG_INFO("Complete unpublished graph storage restoration passed: {} primary-cache 1x->2/3x round trips, every packed and resolved word, actual owned native resolve, staged system bindings and rollback, full processor graph publication and reverse publication, native IssueCopy plus complete recapture after publication, watched tracker epoch replacement; consuming3x packed/resolved path and separate consuming nonempty auxiliary2x->3x 4xMSAA float-depth precision probe passed; no guest title",graph_restores);
    REXLOG_INFO("Native packed EDRAM capture passed: {} actual host-cache cases at 1/2/3x, 1/2/4x MSAA, patterned 32/64-bit/RGBA8/depth-stencil/NaN data, native dump dispatch coverage, immutable snapshots after repaint, incomplete-dump rejection and unchanged guest frame; not a complete renderer snapshot",tested);
    REXLOG_INFO("Packed EDRAM primary processor-cache readback passed: {} additional case(s), production system descriptors and original ownership restored",primary_cases);
    REXLOG_INFO("Packed EDRAM staging restore passed: {} native recapture round trips, every 1/2/3x transition, fragmented 32/64-bit canonical owners at non-row-aligned tile 1001, nonempty-cache and malformed-metadata rejection; auxiliary depth/resolve/renderer state not restored",tested*3);
    REXLOG_INFO("Packed EDRAM original-format rehydration passed: {} native ownership transfer and recapture cases, original color/depth/stencil/MSAA keys and every packed word verified",tested*3);
    REXLOG_INFO("Packed EDRAM mixed native ownership passed: {} capture/scale/restore/native-transfer/recapture cases, RGBA8 plus 64-bit float owners, nonzero bases37/2031 with wraparound, tile1001 format boundary and unowned gap224..333 preserved",mixed_cases);
    return true;
}

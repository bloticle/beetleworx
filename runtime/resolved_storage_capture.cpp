#include "resolved_storage_capture.h"
#include "gpu_constants_internal.h"
#include <rex/graphics/d3d12/texture_cache.h>
#include <rex/graphics/d3d12/shared_memory.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/trace_writer.h>
#include <rex/logging.h>
#include "native_pixel_fixture.h"
#include "resolved_conversion_test.h"
#include <immintrin.h>
namespace {
using CP=rex::graphics::d3d12::D3D12CommandProcessor;
using TC=rex::graphics::d3d12::D3D12TextureCache;
template<class Tag,auto Member>struct Access {friend auto MemberOf(Tag){return Member;}};
struct BeginTag{friend auto MemberOf(BeginTag);};template struct Access<BeginTag,&CP::BeginSubmission>;
struct HeapsTag{friend auto MemberOf(HeapsTag);};template struct Access<HeapsTag,&TC::scaled_resolve_heaps_>;
struct HeapLogTag{friend auto MemberOf(HeapLogTag);};template struct Access<HeapLogTag,TC::kScaledResolveHeapSizeLog2>;
struct IndexTag{friend auto MemberOf(IndexTag);};template struct Access<IndexTag,&TC::GetCurrentScaledResolveBufferIndex>;
struct BufferTag{friend auto MemberOf(BufferTag);};template struct Access<BufferTag,&TC::GetCurrentScaledResolveBuffer>;
struct SharedTransitionTag{friend auto MemberOf(SharedTransitionTag);};template struct Access<SharedTransitionTag,&rex::graphics::d3d12::D3D12SharedMemory::CommitUAVWritesAndTransitionBuffer>;
struct ValidTag{friend auto MemberOf(ValidTag);};template struct Access<ValidTag,&rex::graphics::TextureCache::IsRangeScaledResolved>;
struct MemoryTag{friend auto MemberOf(MemoryTag);};template struct Access<MemoryTag,&rex::graphics::CommandProcessor::memory_>;
struct SharedTag{friend auto MemberOf(SharedTag);};template struct Access<SharedTag,&CP::shared_memory_>;
constexpr uint64_t max_buffer=64ull*1024*1024;
uint64_t Align16(uint64_t value){return (value+15)&~uint64_t(15);}
}
bool em2::CaptureResolvedStorage(Em2CommandProcessor& cp,TC& cache,rex::graphics::d3d12::D3D12SharedMemory& shared,
    const ResolveRangeLedger& ledger,ResolvedStorageSnapshot& output) {
    struct Fp {unsigned value=_mm_getcsr();~Fp(){_mm_setcsr(value);}}fp;
    if(!cp.IsGraphicsWorker() || !cp.DrainForResourceChange())return false;
    ResolvedStorageSnapshot snapshot;snapshot.scale_x=cache.draw_resolution_scale_x();snapshot.scale_y=cache.draw_resolution_scale_y();
    if(!snapshot.scale_x || snapshot.scale_x>7 || !snapshot.scale_y || snapshot.scale_y>7 ||
       (ledger.scale_x() && (ledger.scale_x()!=snapshot.scale_x || ledger.scale_y()!=snapshot.scale_y)))return false;
    std::vector<ResolvedStorageRange> ranges;if(!ledger.Snapshot(ranges))return false;
    const uint64_t area=uint64_t(snapshot.scale_x)*snapshot.scale_y;const bool scaled=cache.IsDrawResolutionScaled();
    std::vector<uint64_t> sizes;
    for(const auto& range:ranges) {
        const uint32_t group=range.bytes_per_texel==1?8:16;
        if(!range.length || (range.address&(group-1)) || (range.length&(group-1)) ||
           uint64_t(range.address)+range.length>0x20000000)return false;
        if(scaled) {
            const auto& heaps=cache.*MemberOf(HeapsTag{});const auto shift=MemberOf(HeapLogTag{});
            const auto first=(uint64_t(range.address)*area)>>shift,last=((uint64_t(range.address)+range.length)*area-1)>>shift;
            if(last>=heaps.size())return false;
            for(uint64_t i=first;i<=last;++i)if(!heaps[size_t(i)])return false;
        }
        uint32_t position=range.address,left=range.length;
        while(left) {
            if(sizes.empty() || Align16(sizes.back())+group*area>max_buffer)sizes.push_back(0);
            const auto offset=Align16(sizes.back());
            const auto take=uint32_t(std::min(uint64_t(left),((max_buffer-offset)/area)&~uint64_t(group-1)));
            if(!take)return false;
            snapshot.spans.push_back({{position,take,range.bytes_per_texel},uint32_t(sizes.size()-1),offset});
            sizes.back()=offset+take*area;position+=take;left-=take;
        }
    }
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    auto* device=cp.GetD3D12Provider().GetDevice();
    for(auto size:sizes) {
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=Align16(size);
        desc.Height=desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&buffer))))return false;
        snapshot.buffers.push_back(std::move(buffer));
    }
    struct RetainPending {ResolvedStorageSnapshot& snapshot;bool armed=false;
        ~RetainPending(){if(armed)for(auto& buffer:snapshot.buffers)buffer.Detach();}}retain{snapshot};
    if(!snapshot.spans.empty()) {
        if(!(cp.*MemberOf(BeginTag{}))(false))return false;
        for(const auto& span:snapshot.spans) {
            ID3D12Resource* source;uint64_t offset=uint64_t(span.native.address)*area,bytes=uint64_t(span.native.length)*area;
            if(scaled) {
                if(!cache.MakeScaledResolveRangeCurrent(span.native.address,span.native.length))return false;
                const auto index=(cache.*MemberOf(IndexTag{}))();source=(cache.*MemberOf(BufferTag{}))().resource();
                if(offset<(uint64_t(index)<<30))return false;offset-=uint64_t(index)<<30;
                cache.TransitionCurrentScaledResolveRange(D3D12_RESOURCE_STATE_COPY_SOURCE);
            }else {source=shared.GetBuffer();shared.UseAsCopySource();}
            if(!source || offset>source->GetDesc().Width || bytes>source->GetDesc().Width-offset)return false;
            cp.SubmitBarriers();retain.armed=true;
            cp.GetDeferredCommandList().D3DCopyBufferRegion(snapshot.buffers[span.buffer_index].Get(),span.offset,source,offset,bytes);
        }
        if(!cp.DrainForResourceChange())return false;
    }
    retain.armed=false;snapshot.completed_submission=cp.GetCompletedSubmission();output=std::move(snapshot);return true;
}

bool em2::RestoreResolvedStorage(Em2CommandProcessor& cp,const ResolvedStorageSnapshot& source,
    TC& cache,ResolveRangeLedger& output) {
    struct Fp {unsigned value=_mm_getcsr();~Fp(){_mm_setcsr(value);}}fp;
    const bool scaled=cache.IsDrawResolutionScaled();auto* shared=(cp.*MemberOf(SharedTag{})).get();
    if(!cp.IsGraphicsWorker() || !shared || source.scale_x!=cache.draw_resolution_scale_x() ||
       source.scale_y!=cache.draw_resolution_scale_y() || (scaled && source.scale_x*uint64_t(source.scale_y)<4))return false;
    std::vector<ResolvedStorageRange> ranges;std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> used;
    const uint64_t area=uint64_t(source.scale_x)*source.scale_y;
    for(const auto& span:source.spans) {
        if(span.buffer_index>=source.buffers.size() || !source.buffers[span.buffer_index])return false;
        const auto& resource=source.buffers[span.buffer_index];const auto desc=resource->GetDesc();
        const uint64_t bytes=uint64_t(span.native.length)*area;
        if(resource.Get()==shared->GetBuffer() || desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER || !bytes || bytes>(1ull<<30) ||
           span.offset>desc.Width || bytes>desc.Width-span.offset)return false;
        ranges.push_back(span.native);
        if(std::none_of(used.begin(),used.end(),[&](const auto& item){return item.Get()==resource.Get();}))used.push_back(resource);
    }
    ResolveRangeLedger staged;if(!staged.SeedRanges(ranges,source.scale_x,source.scale_y) || !cp.DrainForResourceChange())return false;
    if(!source.spans.empty()) {
        if(!(cp.*MemberOf(BeginTag{}))(false))return false;
        cache.BeginSubmission(cp.GetCurrentSubmission());
        // Allocation can enqueue tile mappings. Even a failed allocation must
        // be drained before the caller discards the partially staged cache.
        for(const auto& range:ranges)if(!(scaled?cache.EnsureScaledResolveMemoryCommitted(range.address,range.length):shared->RequestRange(range.address,range.length))) {
            cp.DrainForResourceChange();return false;
        }
        struct RetainPending {decltype(used)& sources;bool armed=false;
            ~RetainPending(){if(armed)for(auto& source:sources)source.Detach();}}retain{used};
        retain.armed=true;
        for(const auto& buffer:used)cp.PushTransitionBarrier(buffer.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
        for(const auto& span:source.spans) {
            ID3D12Resource* destination;uint64_t offset=uint64_t(span.native.address)*area;
            const uint64_t bytes=uint64_t(span.native.length)*area;
            if(scaled) {
                if(!cache.MakeScaledResolveRangeCurrent(span.native.address,span.native.length))return false;
                destination=(cache.*MemberOf(BufferTag{}))().resource();
                const uint64_t base=uint64_t((cache.*MemberOf(IndexTag{}))())<<30;
                if(offset<base)return false;offset-=base;
                cache.TransitionCurrentScaledResolveRange(D3D12_RESOURCE_STATE_COPY_DEST);
            }else {
                destination=shared->GetBuffer();
                (shared->*MemberOf(SharedTransitionTag{}))(D3D12_RESOURCE_STATE_COPY_DEST);
            }
            if(!destination || offset>destination->GetDesc().Width || bytes>destination->GetDesc().Width-offset)return false;
            cp.SubmitBarriers();
            cp.GetDeferredCommandList().D3DCopyBufferRegion(destination,offset,source.buffers[span.buffer_index].Get(),span.offset,bytes);
        }
        for(const auto& buffer:used)cp.PushTransitionBarrier(buffer.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        cp.SubmitBarriers();if(!cp.DrainForResourceChange())return false;retain.armed=false;
        // This invokes the real native page map and shared-memory GPU-write
        // notifications. Do not fake validity by updating only our ledger.
        for(const auto& range:ranges)cache.MarkRangeAsResolved(range.address,range.length);
        if(scaled)for(const auto& range:ranges)if(!(cache.*MemberOf(ValidTag{}))(range.address,range.length))return false;
    }
    output=std::move(staged);return true;
}

bool em2::TestResolvedStorageCapture(Em2CommandProcessor& cp) {
    if(!cp.IsGraphicsWorker() || !cp.DrainForResourceChange())return false;
    namespace g=rex::graphics;namespace x=g::xenos;
    auto* shared=(cp.*MemberOf(SharedTag{})).get();unsigned tested=0;bool split_seen=false;
    const auto guest_frame=cp.GetCurrentFrame();unsigned converted_cases=0,restored_cases=0;bool conversion_growth_seen=false;
    auto pattern=[](uint64_t word,unsigned pass) {
        uint32_t value=uint32_t(word)^uint32_t(word>>32)^0x7FC12345u^(pass*0xA713591Du);
        value^=value>>16;value*=0x85EBCA6Bu;return value^(value>>13);
    };
    for(const auto scale:std::array<std::array<uint32_t,2>,4>{{{2,2},{3,3},{2,3},{3,2}}}) {
        const auto sx=scale[0],sy=scale[1],area=sx*sy;
        auto regs=std::make_unique<g::RegisterFile>();auto cache=TC::Create(*regs,*shared,sx,sy,cp,false);if(!cache)return false;
        struct Finish {Em2CommandProcessor& cp;std::unique_ptr<TC>& cache;std::unique_ptr<g::RegisterFile>& regs;
            ~Finish(){if(!cp.DrainForResourceChange()){cache.release();regs.release();}}}finish{cp,cache,regs};
        std::vector<g::draw_util::ResolveInfo> infos;
        auto info_for=[](uint32_t base,uint32_t width,uint32_t height,x::TextureFormat format,uint32_t bpp) {
            g::draw_util::ResolveInfo info{};info.coordinate_info.width_div_8=width/8;info.height_div_8=height/8;
            info.copy_dest_base=info.copy_dest_extent_start=base;info.copy_dest_info.copy_dest_format=x::ColorFormat(format);
            info.copy_dest_coordinate_info.pitch_aligned_div_32=(width+31)/32;info.copy_dest_coordinate_info.height_aligned_div_32=(height+31)/32;
            info.copy_dest_extent_length=g::texture_util::GetTiledAddressUpperBound2D(width,height,width,std::countr_zero(bpp));return info;
        };
        infos.push_back(info_for(0x100000,32,32,x::TextureFormat::k_8_8_8_8,4));
        infos.push_back(info_for(0x18000000,32,32,x::TextureFormat::k_16_16_16_16_FLOAT,8));
        for(uint64_t boundary:{1ull<<30,2ull<<30}) {
            const auto native=uint32_t((boundary/area)&~uint64_t(4095));
            if(native>=4096 && uint64_t(native)+4096<0x20000000)infos.push_back(info_for(native-4096,64,32,x::TextureFormat::k_8_8_8_8,4));
        }
        if(sx==sy)infos.push_back(info_for(0x1000000,512,4096,x::TextureFormat::k_8_8_8_8,4)); // Grows past64MiB at3x.
        ResolveRangeLedger ledger;
        for(const auto& info:infos)if(!ledger.Record({cache.get(),info.copy_dest_extent_start,info.copy_dest_extent_length,sx,sy,&info}))return false;
        ResolvedStorageSnapshot empty;ResolveRangeLedger no_data;
        if(!CaptureResolvedStorage(cp,*cache,*shared,no_data,empty) || !empty.buffers.empty())return false;
        if(CaptureResolvedStorage(cp,*cache,*shared,ledger,empty) || !empty.buffers.empty())return false;
        for(const auto& heap:cache.get()->*MemberOf(HeapsTag{}))if(heap)return false; // Missing-source rejection never commits backing.
        std::vector<ResolvedStorageRange> ranges;if(!ledger.Snapshot(ranges))return false;
        auto fill=[&](unsigned pass) {
            if(!(cp.*MemberOf(BeginTag{}))(false))return false;cache->BeginSubmission(cp.GetCurrentSubmission());
            uint64_t bytes=0;for(const auto& range:ranges)bytes+=uint64_t(range.length)*area;
            Microsoft::WRL::ComPtr<ID3D12Resource> upload;
            if(!pixel_test::Buffer(cp.GetD3D12Provider().GetDevice(),bytes,D3D12_HEAP_TYPE_UPLOAD,&upload))return false;
            struct RetainUpload {Microsoft::WRL::ComPtr<ID3D12Resource>& resource;bool armed=false;
                ~RetainUpload(){if(armed)resource.Detach();}}retain_upload{upload};
            void* mapping=nullptr;D3D12_RANGE none{0,0};if(FAILED(upload->Map(0,&none,&mapping)))return false;
            uint64_t position=0;for(const auto& range:ranges) {
                for(uint64_t i=0;i<uint64_t(range.length)*area/4;++i)static_cast<uint32_t*>(mapping)[position/4+i]=pattern(uint64_t(range.address)*area/4+i,pass);
                position+=uint64_t(range.length)*area;
            }
            upload->Unmap(0,&none);position=0;
            for(const auto& range:ranges) {
                if(!cache->EnsureScaledResolveMemoryCommitted(range.address,range.length) || !cache->MakeScaledResolveRangeCurrent(range.address,range.length))return false;
                const auto index=(cache.get()->*MemberOf(IndexTag{}))();auto* resource=(cache.get()->*MemberOf(BufferTag{}))().resource();
                cache->TransitionCurrentScaledResolveRange(D3D12_RESOURCE_STATE_COPY_DEST);cp.SubmitBarriers();
                const auto length=uint64_t(range.length)*area;
                retain_upload.armed=true;
                cp.GetDeferredCommandList().D3DCopyBufferRegion(resource,uint64_t(range.address)*area-(uint64_t(index)<<30),upload.Get(),position,length);position+=length;
            }
            if(!cp.DrainForResourceChange())return false;retain_upload.armed=false;return true;
        };
        ResolvedStorageSnapshot a,b;
        if(!fill(0) || !CaptureResolvedStorage(cp,*cache,*shared,ledger,a) || a.buffers.empty() || a.spans.empty())return false;
        const auto* retained=a.buffers.front().Get();const auto epoch=a.completed_submission;
        ResolveRangeLedger incomplete;for(const auto& info:infos)if(!incomplete.Record({cache.get(),info.copy_dest_extent_start,info.copy_dest_extent_length,sx,sy,&info}))return false;
        const auto missing=info_for(0x3000000,32,32,x::TextureFormat::k_8_8_8_8,4);
        if(!incomplete.Record({cache.get(),missing.copy_dest_extent_start,missing.copy_dest_extent_length,sx,sy,&missing}) ||
           CaptureResolvedStorage(cp,*cache,*shared,incomplete,a) || a.buffers.front().Get()!=retained || a.completed_submission!=epoch)return false;
        if(!fill(1) || !CaptureResolvedStorage(cp,*cache,*shared,ledger,b) || b.buffers.front().Get()==retained)return false;
        if(sx==3 && sy==3){if(a.buffers.size()<2)return false;split_seen=true;}
        if(!cp.DrainForResourceChange())return false;cache.reset(); // Snapshots must survive actual source destruction.
        for(unsigned destination:{1u,2u,3u}) {
            ResolvedStorageSnapshot converted;
            if(!ConvertResolvedStorage(cp,a,destination,destination,converted) || !resolved_test::Verify(cp,converted,sx,sy,0))return false;
            if(destination>1) {
                auto restored=TC::Create(*regs,*shared,destination,destination,cp,false);if(!restored)return false;
                struct RetainCache {Em2CommandProcessor& cp;std::unique_ptr<TC>& cache;
                    ~RetainCache(){if(!cp.DrainForResourceChange())cache.release();}}retain_cache{cp,restored};
                ResolveRangeLedger imported;ResolvedStorageSnapshot recaptured;
                if(!RestoreResolvedStorage(cp,converted,*restored,imported) ||
                   !CaptureResolvedStorage(cp,*restored,*shared,imported,recaptured) ||
                   !resolved_test::Verify(cp,recaptured,sx,sy,0) || !resolved_test::Verify(cp,converted,sx,sy,0))return false;
                // A malformed later attempt must not change existing metadata.
                auto malformed=converted;malformed.spans.front().buffer_index=uint32_t(malformed.buffers.size());
                const auto pages=imported.page_count();
                if(RestoreResolvedStorage(cp,malformed,*restored,imported) || imported.page_count()!=pages)return false;
                ++restored_cases;
            }
            conversion_growth_seen|=converted.buffers.size()>a.buffers.size();
            ++converted_cases;
        }
        for(unsigned pass=0;pass<2;++pass) {
            auto& snapshot=pass?b:a;
            for(const auto& span:snapshot.spans) {
                const uint64_t bytes=uint64_t(span.native.length)*area;Microsoft::WRL::ComPtr<ID3D12Resource> readback;
                if(!pixel_test::Buffer(cp.GetD3D12Provider().GetDevice(),bytes,D3D12_HEAP_TYPE_READBACK,&readback) || !(cp.*MemberOf(BeginTag{}))(false))return false;
                auto* source=snapshot.buffers[span.buffer_index].Get();
                cp.PushTransitionBarrier(source,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);cp.SubmitBarriers();
                cp.GetDeferredCommandList().D3DCopyBufferRegion(readback.Get(),0,source,span.offset,bytes);
                cp.PushTransitionBarrier(source,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);cp.SubmitBarriers();
                if(!cp.DrainForResourceChange()){readback.Detach();for(auto& buffer:snapshot.buffers)buffer.Detach();return false;}
                void* mapping=nullptr;D3D12_RANGE range{0,size_t(bytes)};if(FAILED(readback->Map(0,&range,&mapping)))return false;
                const auto* words=static_cast<const uint32_t*>(mapping);bool correct=true;
                for(uint64_t i=0;i<bytes/4;++i)correct&=words[i]==pattern(uint64_t(span.native.address)*area/4+i,pass);
                D3D12_RANGE none{0,0};readback->Unmap(0,&none);if(!correct){REXLOG_ERROR("Native resolved snapshot mismatch: {}x{}, pass={}, address={:08X}",sx,sy,pass,span.native.address);return false;}
            }
        }
        ++tested;
    }
    if(!split_seen || !conversion_growth_seen || cp.GetCurrentFrame()!=guest_frame)return false;
    REXLOG_INFO("Native scaled resolved-storage capture passed: {} scale pairs, typed mixed ranges, opaque words, 1/2GiB alias-window crossings, >64MiB snapshot packing/splitting, immutable repaint and source eviction, missing-source/no-allocation rejection, unchanged guest frame; gameplay restoration not connected",tested);
    REXLOG_INFO("Captured native resolved snapshot conversion passed: {} cases at destination1/2/3x, mixed32/64-bit ranges, asymmetric sources, contraction and growth across64MiB buffer splits, every converted word verified; gameplay restoration not connected",converted_cases);
    REXLOG_INFO("Native scaled resolved restoration passed: {} cases, destination2/3x, native page validity and shared notifications, recapture every word, source reuse and invalid input retention; gameplay renderer controller not connected",restored_cases);
    return true;
}

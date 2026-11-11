#include "resolved_storage_capture.h"
#include "gpu_constants_internal.h"
#include "scaled_resolve_migration.h"
#include "native_pixel_fixture.h"
#include "resolved_conversion_test.h"
#include <algorithm>
#include <immintrin.h>
namespace {
using Microsoft::WRL::ComPtr;
constexpr uint64_t block_limit=64ull*1024*1024;
uint64_t Align16(uint64_t value){return (value+15)&~uint64_t(15);}
struct Dispatch {uint32_t source_span,destination_span;uint64_t source_offset;};
}
bool em2::ConvertResolvedStorage(Em2CommandProcessor& cp,const ResolvedStorageSnapshot& source,uint32_t dx,uint32_t dy,ResolvedStorageSnapshot& output) {
    struct Fp {unsigned value=_mm_getcsr();~Fp(){_mm_setcsr(value);}}fp;
    const auto sx=source.scale_x,sy=source.scale_y;
    if(!cp.IsGraphicsWorker() || !sx || sx>7 || !sy || sy>7 || !dx || dx>7 || !dy || dy>7 || !cp.DrainForResourceChange())return false;
    const uint64_t source_area=uint64_t(sx)*sy,destination_area=uint64_t(dx)*dy;const bool copy=sx==dx && sy==dy;
    struct Job {
        ScaledResolveMigrator migrator;
        ResolvedStorageSnapshot result;std::vector<ComPtr<ID3D12Resource>> inputs;
        ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Fence> fence;
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);~Job(){if(event)CloseHandle(event);}
    };
    auto job=std::make_unique<Job>();job->result.scale_x=dx;job->result.scale_y=dy;
    std::vector<uint64_t> sizes;std::vector<Dispatch> dispatches;uint64_t previous_end=0;
    std::vector<bool> used(source.buffers.size(),false);
    for(size_t index=0;index<source.spans.size();++index) {
        const auto& span=source.spans[index];const auto bpp=span.native.bytes_per_texel;
        if(bpp!=1 && bpp!=2 && bpp!=4 && bpp!=8 && bpp!=16)return false;const uint32_t group=bpp==1?8:16;
        if(!span.native.length || span.native.address<previous_end || uint64_t(span.native.address)+span.native.length>0x20000000 ||
           (span.native.address&(group-1)) || (span.native.length&(group-1)) || (span.offset&15) ||
           span.buffer_index>=source.buffers.size() || !source.buffers[span.buffer_index])return false;
        previous_end=uint64_t(span.native.address)+span.native.length;const auto desc=source.buffers[span.buffer_index]->GetDesc();
        if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER || (desc.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
           span.offset>desc.Width || uint64_t(span.native.length)*source_area>desc.Width-span.offset)return false;
        used[span.buffer_index]=true;
        uint32_t progress=0;
        while(progress<span.native.length) {
            const auto left=span.native.length-progress;const uint32_t minimum=left==8?8:16;
            if(sizes.empty() || Align16(sizes.back())+minimum*destination_area>block_limit)sizes.push_back(0);
            const auto offset=Align16(sizes.back());
            const auto capacity=((block_limit-offset)/destination_area)&~uint64_t(15);
            const auto take=uint32_t(std::min(uint64_t(left),capacity?capacity:uint64_t(minimum)));
            if(!take || uint64_t(take)*destination_area>block_limit-offset)return false;
            const auto source_offset=span.offset+uint64_t(progress)*source_area;if(source_offset&15)return false;
            job->result.spans.push_back({{span.native.address+progress,take,bpp},uint32_t(sizes.size()-1),offset});
            dispatches.push_back({uint32_t(index),uint32_t(job->result.spans.size()-1),source_offset});
            sizes.back()=offset+uint64_t(take)*destination_area;progress+=take;
        }
    }
    if(source.spans.empty()){job->result.completed_submission=cp.GetCompletedSubmission();output=std::move(job->result);return true;}
    auto* device=cp.GetD3D12Provider().GetDevice();
    if(!job->event || (!copy && !job->migrator.Initialize(device)) ||
       FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&job->allocator))) ||
       FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,job->allocator.Get(),nullptr,IID_PPV_ARGS(&job->list))) ||
       FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&job->fence))))return false;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    for(auto size:sizes) {
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=Align16(size);
        desc.Height=desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags=copy?D3D12_RESOURCE_FLAG_NONE:D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;ComPtr<ID3D12Resource> buffer;
        if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,copy?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,IID_PPV_ARGS(&buffer))))return false;job->result.buffers.push_back(std::move(buffer));
    }
    const auto input_state=copy?D3D12_RESOURCE_STATE_COPY_SOURCE:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    for(size_t i=0;i<used.size();++i)if(used[i]) {
        if(std::any_of(job->inputs.begin(),job->inputs.end(),[&](const auto& input){return input.Get()==source.buffers[i].Get();}))continue;
        job->inputs.push_back(source.buffers[i]);pixel_test::Barrier(job->list.Get(),source.buffers[i].Get(),D3D12_RESOURCE_STATE_COPY_DEST,input_state);
    }
    for(const auto& dispatch:dispatches) {
        const auto& input=source.spans[dispatch.source_span];const auto& destination=job->result.spans[dispatch.destination_span];
        auto* a=source.buffers[input.buffer_index].Get();auto* b=job->result.buffers[destination.buffer_index].Get();
        if(copy)job->list->CopyBufferRegion(b,destination.offset,a,dispatch.source_offset,uint64_t(destination.native.length)*destination_area);
        else {
            const ScaledResolveMigration parameters{sx,sy,dx,dy,destination.native.bytes_per_texel,destination.native.length};
            if(!job->migrator.Record(job->list.Get(),a,dispatch.source_offset,b,destination.offset,parameters))return false;
        }
    }
    for(auto& input:job->inputs)pixel_test::Barrier(job->list.Get(),input.Get(),input_state,D3D12_RESOURCE_STATE_COPY_DEST);
    if(!copy)for(auto& buffer:job->result.buffers)pixel_test::Barrier(job->list.Get(),buffer.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
    if(FAILED(job->list->Close()))return false;ID3D12CommandList* lists[]={job->list.Get()};auto* queue=cp.GetD3D12Provider().GetDirectQueue();queue->ExecuteCommandLists(1,lists);
    if(FAILED(queue->Signal(job->fence.Get(),1)) || FAILED(job->fence->SetEventOnCompletion(1,job->event)) ||
       WaitForSingleObject(job->event,30000)!=WAIT_OBJECT_0 || FAILED(device->GetDeviceRemovedReason())){job.release();return false;}
    job->result.completed_submission=cp.GetCompletedSubmission();output=std::move(job->result);return true;
}

bool em2::TestResolvedStorageConversion(Em2CommandProcessor& cp) {
    if(!cp.IsGraphicsWorker() || !cp.DrainForResourceChange())return false;
    const auto guest_frame=cp.GetCurrentFrame();unsigned tested=0;
    const std::array<std::array<unsigned,2>,5> sources={{{1,1},{2,2},{3,3},{2,3},{3,2}}};
    const std::array<std::array<unsigned,2>,4> destinations={{{1,1},{2,2},{3,3},{3,2}}};
    auto* device=cp.GetD3D12Provider().GetDevice();
    for(const auto source_scale:sources)for(unsigned bpp:{1u,2u,4u,8u,16u}) {
        const auto sx=source_scale[0],sy=source_scale[1];const uint32_t native_bytes=bpp==1?8:16*17,native_address=bpp==1?0x100008:0x100000;
        const uint64_t bytes=uint64_t(native_bytes)*sx*sy,allocation=(bytes+15)/16*16+32;
        struct UploadJob {pixel_test::Fixture gpu;};auto upload=std::make_unique<UploadJob>();auto& f=upload->gpu;
        if(!f.event || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f.allocator))) ||
           FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f.allocator.Get(),nullptr,IID_PPV_ARGS(&f.list))) ||
           FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f.fence))) || !pixel_test::Buffer(device,allocation,D3D12_HEAP_TYPE_UPLOAD,&f.upload))return false;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=allocation;
        desc.Height=desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&f.target))))return false;
        void* mapping=nullptr;D3D12_RANGE none{0,0};if(FAILED(f.upload->Map(0,&none,&mapping)))return false;
        std::memset(mapping,0xA7,size_t(allocation));
        for(uint64_t i=0;i<bytes/4;++i)static_cast<uint32_t*>(mapping)[4+i]=resolved_test::Word(uint64_t(native_address)*sx*sy/4+i,0);
        f.upload->Unmap(0,&none);f.list->CopyBufferRegion(f.target.Get(),0,f.upload.Get(),0,allocation);if(FAILED(f.list->Close()))return false;
        ID3D12CommandList* lists[]={f.list.Get()};auto* queue=cp.GetD3D12Provider().GetDirectQueue();queue->ExecuteCommandLists(1,lists);
        if(FAILED(queue->Signal(f.fence.Get(),1)) || FAILED(f.fence->SetEventOnCompletion(1,f.event)) ||
           WaitForSingleObject(f.event,30000)!=WAIT_OBJECT_0){upload.release();return false;}
        ResolvedStorageSnapshot source;source.scale_x=sx;source.scale_y=sy;source.buffers.push_back(f.target);source.spans.push_back({{native_address,native_bytes,bpp},0,16});
        for(const auto destination:destinations) {
            ResolvedStorageSnapshot converted;
            if(!ConvertResolvedStorage(cp,source,destination[0],destination[1],converted) || !resolved_test::Verify(cp,converted,sx,sy,0))return false;
            const auto* retained=converted.buffers.front().Get();auto malformed=source;malformed.spans.front().buffer_index=UINT32_MAX;
            if(ConvertResolvedStorage(cp,malformed,destination[0],destination[1],converted) || converted.buffers.front().Get()!=retained)return false;
            ++tested;
        }
        if(!resolved_test::Verify(cp,source,sx,sy,0))return false; // Repeated conversions leave source payload/state usable.
    }
    if(cp.GetCurrentFrame()!=guest_frame)return false;
    REXLOG_INFO("Resolved snapshot conversion passed: {} GPU cases, 8/16/32/64/128-bit formats, minimum8-byte native range, equal/asymmetric/1/2/3x scales, opaque per-texel byte oracle, source reuse and malformed-input output retention; native restoration not connected",tested);
    return true;
}

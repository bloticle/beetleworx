#pragma once
#include "resolved_storage_capture.h"
#include "gpu_constants_internal.h"
#include "native_pixel_fixture.h"
#include <rex/logging.h>
namespace em2::resolved_test {
inline uint32_t Word(uint64_t word,unsigned pass) {
    uint32_t value=uint32_t(word)^uint32_t(word>>32)^0x7FC12345u^(pass*0xA713591Du);
    value^=value>>16;value*=0x85EBCA6Bu;return value^(value>>13);
}
inline bool Verify(Em2CommandProcessor& cp,const ResolvedStorageSnapshot& snapshot,unsigned sx,unsigned sy,unsigned pass) {
    struct Job {pixel_test::Fixture gpu;std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> inputs,readbacks;};
    auto job=std::make_unique<Job>();auto& f=job->gpu;auto* device=cp.GetD3D12Provider().GetDevice();
    if(!f.event || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f.allocator))) ||
       FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f.allocator.Get(),nullptr,IID_PPV_ARGS(&f.list))) ||
       FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f.fence))))return false;
    const uint64_t destination_area=uint64_t(snapshot.scale_x)*snapshot.scale_y,source_area=uint64_t(sx)*sy;
    for(const auto& span:snapshot.spans) {
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;const auto bytes=uint64_t(span.native.length)*destination_area;
        if(!pixel_test::Buffer(device,bytes,D3D12_HEAP_TYPE_READBACK,&readback))return false;
        auto input=snapshot.buffers[span.buffer_index];job->inputs.push_back(input);job->readbacks.push_back(readback);
        pixel_test::Barrier(f.list.Get(),input.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
        f.list->CopyBufferRegion(readback.Get(),0,input.Get(),span.offset,bytes);
        pixel_test::Barrier(f.list.Get(),input.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if(FAILED(f.list->Close()))return false;ID3D12CommandList* lists[]={f.list.Get()};auto* queue=cp.GetD3D12Provider().GetDirectQueue();queue->ExecuteCommandLists(1,lists);
    if(FAILED(queue->Signal(f.fence.Get(),1)) || FAILED(f.fence->SetEventOnCompletion(1,f.event)) ||
       WaitForSingleObject(f.event,30000)!=WAIT_OBJECT_0 || FAILED(device->GetDeviceRemovedReason())){job.release();return false;}
    for(size_t index=0;index<snapshot.spans.size();++index) {
        const auto& span=snapshot.spans[index];const auto bpp=span.native.bytes_per_texel,unit=bpp==1?8u:16u,words=unit/4,lanes=unit/bpp;
        const uint64_t bytes=uint64_t(span.native.length)*destination_area;void* mapping=nullptr;D3D12_RANGE range{0,size_t(bytes)};
        if(FAILED(job->readbacks[index]->Map(0,&range,&mapping)))return false;
        const auto* actual=static_cast<const uint32_t*>(mapping);bool correct=true;uint64_t first_bad=UINT64_MAX;
        for(uint64_t i=0;i<bytes/4;++i) {
            const auto group=uint64_t(span.native.address)/unit+i/(words*destination_area);
            const auto local=i%(words*destination_area),phase=local/words,word=local%words;
            const auto py=(2*(phase%snapshot.scale_y)+1)*sy/(2*snapshot.scale_y);
            uint32_t expected=0;
            if(bpp>=4) {
                const auto px=(2*((phase/snapshot.scale_y)*lanes+word/(bpp/4))+1)*sx/(2*snapshot.scale_x);
                const auto source_word=group*source_area*words+(px/lanes*sy+py)*words+(px%lanes)*(bpp/4)+word%(bpp/4);
                expected=Word(source_word,pass);
            }else for(unsigned component=0;component<4;++component) {
                const auto byte=word*4+component;
                const auto px=(2*((phase/snapshot.scale_y)*lanes+byte/bpp)+1)*sx/(2*snapshot.scale_x);
                const auto source_byte=(px%lanes)*bpp+byte%bpp;
                const auto source_word=group*source_area*words+(px/lanes*sy+py)*words+source_byte/4;
                expected|=((Word(source_word,pass)>>((source_byte%4)*8))&255)<<(component*8);
            }
            if(actual[i]!=expected){correct=false;if(first_bad==UINT64_MAX)first_bad=i;}
        }
        D3D12_RANGE none{0,0};job->readbacks[index]->Unmap(0,&none);
        if(!correct){REXLOG_ERROR("Resolved snapshot conversion mismatch: {}x{} -> {}x{}, bpp={}, native={:08X}, word={}",sx,sy,snapshot.scale_x,snapshot.scale_y,bpp,span.native.address,first_bad);return false;}
    }
    return true;
}
}

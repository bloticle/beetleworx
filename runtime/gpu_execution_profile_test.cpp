#include "gpu_execution_profile.h"
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <wrl/client.h>
#include <windows.h>
#include <immintrin.h>
#include <cstring>
#include <memory>

bool em2::TestGpuExecutionTiming(rex::graphics::d3d12::D3D12CommandProcessor& processor) {
    auto& provider=processor.GetD3D12Provider();
    const uint64_t first_serial=processor.GetCurrentSubmission(),second_serial=first_serial+1;
    using Microsoft::WRL::ComPtr;
    auto* device=provider.GetDevice();auto* queue=provider.GetDirectQueue();
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Resource> source,destination;ComPtr<ID3D12Fence> fence;
    if(FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))) ||
       FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))) ||
       FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))))return false;
    constexpr uint64_t size=4*1024*1024;
    D3D12_HEAP_PROPERTIES memory{};memory.Type=D3D12_HEAP_TYPE_UPLOAD;memory.CreationNodeMask=memory.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=size;
    desc.Height=desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if(FAILED(device->CreateCommittedResource(&memory,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&source))))return false;
    memory.Type=D3D12_HEAP_TYPE_READBACK;
    if(FAILED(device->CreateCommittedResource(&memory,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&destination))))return false;
    void* mapped=nullptr;D3D12_RANGE none{0,0};
    if(FAILED(source->Map(0,&none,&mapped)))return false;
    std::memset(mapped,0x6D,size);D3D12_RANGE written{0,size};source->Unmap(0,&written);
    auto profile=std::make_unique<GpuExecutionProfiler>();
    struct Event {HANDLE value=CreateEventW(nullptr,FALSE,FALSE,nullptr);~Event(){if(value)CloseHandle(value);}} event;
    if(!event.value)return false;
    auto submit=[&](uint64_t serial) {
        if(FAILED(list->Close()))return false;
        ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);
        if(FAILED(queue->Signal(fence.Get(),serial)) || FAILED(fence->SetEventOnCompletion(serial,event.value)) || WaitForSingleObject(event.value,15000)!=WAIT_OBJECT_0) {
            // A failed/timed-out diagnostic must not free resources still used
            // by an uncompleted GPU list. Quarantine until this process exits.
            profile.release();source.Detach();destination.Detach();list.Detach();allocator.Detach();fence.Detach();event.value=nullptr;
            REXLOG_ERROR("GPU timestamp diagnostic fence failed/timed out; pending resources retained");return false;
        }
        return true;
    };
    const auto csr=_mm_getcsr();
    rex::graphics::d3d12::DeferredCommandList deferred(processor);
    deferred.Execute(list.Get(),nullptr); // Real native bridge, including the default bypass.

    auto token=profile->Begin(provider,list.Get(),first_serial,0);
    if(token==GpuExecutionProfiler::invalid_token)return false;
    {
        GpuExecutionScope scoped(*profile,processor);
        deferred.Execute(list.Get(),nullptr); // Nested TLS scope and actual SDK replay.
    }
    list->CopyBufferRegion(destination.Get(),0,source.Get(),0,size);
    profile->End(list.Get(),token);profile->Poll(0);
    if(profile->stats().samples || _mm_getcsr()!=csr)return false;
    if(!submit(1))return false;
    profile->Poll(first_serial);
    auto first=profile->stats();
    if(first.samples!=2 || !first.ticks || !first.frequency || first.invalid || _mm_getcsr()!=csr)return false;
    D3D12_RANGE data_range{0,size};if(FAILED(destination->Map(0,&data_range,&mapped)))return false;
    bool bytes=true;for(unsigned i=0;i<16;++i)bytes&=static_cast<uint8_t*>(mapped)[i]==0x6D && static_cast<uint8_t*>(mapped)[size-1-i]==0x6D;
    destination->Unmap(0,&none);if(!bytes)return false;
    if(FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(),nullptr)))return false;
    for(unsigned i=0;i<GpuExecutionProfiler::capacity;++i) {
        token=profile->Begin(provider,list.Get(),second_serial,first_serial);if(token==GpuExecutionProfiler::invalid_token)return false;
        profile->End(list.Get(),token);
    }
    if(profile->Begin(provider,list.Get(),second_serial,first_serial)!=GpuExecutionProfiler::invalid_token || profile->stats().skipped!=1 || profile->stats().samples!=2)return false;
    if(!submit(2))return false;
    profile->Poll(second_serial-1);if(profile->stats().samples!=2)return false; // Fence metadata gates CPU reads.
    profile->Poll(second_serial);const auto all=profile->stats();
    if(all.samples!=66 || all.invalid || _mm_getcsr()!=csr)return false;
    profile->FlushReport();if(_mm_getcsr()!=csr || profile->stats().samples!=66)return false;
    REXLOG_INFO("GPU execution timestamp diagnostic passed: real 4 MiB copy + empty SDK replay {:.3f} ms, frequency {} Hz, {} resolved samples; native delegate/nested scope, fence gating, 64-slot reuse/exhaustion, nonblocking skip and MXCSR preservation (no gameplay)",first.ticks*1000.0/first.frequency,first.frequency,all.samples);
    _mm_setcsr(csr);return true;
}

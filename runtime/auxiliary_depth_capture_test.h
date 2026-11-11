#pragma once
#include "edram_capture.h"
#include "native_pixel_fixture.h"
#include <d3dcompiler.h>
#include <rex/logging.h>
#include <array>
#include <memory>
namespace em2::depth_test {
using Microsoft::WRL::ComPtr;
struct Probe {unsigned x=0,y=0,depth=0,stencil=0,depth_step=0,stencil_xor_step=0;};
struct Reader {
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>,2> pipelines;
    bool Initialize(ID3D12Device* device) {
        const char shader[]=R"(
#if MULTISAMPLED
Texture2DMS<float> depth:register(t0);
Texture2DMS<uint2> stencil:register(t1);
#else
Texture2D<float> depth:register(t0);
Texture2D<uint2> stencil:register(t1);
#endif
RWByteAddressBuffer result:register(u0);
cbuffer Parameters:register(b0){uint2 coordinate;};
[numthreads(4,1,1)]void main(uint id:SV_GroupThreadID) {
#if MULTISAMPLED
 uint width,height,samples;depth.GetDimensions(width,height,samples);
 if(id>=samples)return;
 float d=depth.Load(int2(coordinate),id);uint s=stencil.Load(int2(coordinate),id).g;
#else
 if(id)return;float d=depth.Load(int3(coordinate,0));uint s=stencil.Load(int3(coordinate,0)).g;
#endif
 result.Store2(id*8,uint2(asuint(d),s));
})";
        const D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0};
        std::array<D3D12_ROOT_PARAMETER,3> parameters{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[0].DescriptorTable={1,&range};
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[2].Constants={0,0,2};
        D3D12_ROOT_SIGNATURE_DESC desc{3,parameters.data(),0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> serialized,errors;
        if(FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors)) ||
           FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root))))return false;
        const auto library=LoadLibraryW(L"d3dcompiler_47.dll");if(!library)return false;
        struct Library {HMODULE value;~Library(){FreeLibrary(value);}}close{library};
        const auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(library,"D3DCompile"));if(!compile)return false;
        for(unsigned ms=0;ms<2;++ms) {
            const D3D_SHADER_MACRO defines[]={{"MULTISAMPLED",ms?"1":"0"},{nullptr,nullptr}};ComPtr<ID3DBlob> code;
            if(FAILED(compile(shader,sizeof(shader)-1,"auxiliary-depth-reader",defines,nullptr,"main","cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors))) {
                if(errors)REXLOG_ERROR("Auxiliary depth reader: {}",static_cast<const char*>(errors->GetBufferPointer()));return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=root.Get();pso.CS={code->GetBufferPointer(),code->GetBufferSize()};
            if(FAILED(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&pipelines[ms]))))return false;
        }
        return true;
    }
};
inline bool ReadSnapshots(ID3D12Device* device,ID3D12CommandQueue* queue,const std::shared_ptr<Reader>& reader,
    const AuxiliaryDepthSnapshot& a,const AuxiliaryDepthSnapshot& b,uint32_t depth_a,uint32_t depth_b,
    const std::array<Probe,2>* selected=nullptr) {
    struct Job {
        pixel_test::Fixture gpu;std::shared_ptr<Reader> reader;
        std::array<ComPtr<ID3D12Resource>,2> inputs;ComPtr<ID3D12Resource> output;
    };
    auto job=std::make_unique<Job>();job->reader=reader;auto& f=job->gpu;
    if(a.targets.size()!=1 || b.targets.size()!=1)return false;
    job->inputs={a.targets[0].texture,b.targets[0].texture};
    const std::array<Probe,2> defaults={Probe{0,0,depth_a,0xA5},Probe{0,0,depth_b,0x3C}};const auto& probes=selected?*selected:defaults;
    const auto samples=job->inputs[0]->GetDesc().SampleDesc.Count;if(samples!=job->inputs[1]->GetDesc().SampleDesc.Count || samples>4)return false;
    if(!f.event || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f.allocator))) ||
       FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f.allocator.Get(),nullptr,IID_PPV_ARGS(&f.list))) ||
       FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f.fence))) ||
       !pixel_test::Buffer(device,64,D3D12_HEAP_TYPE_READBACK,&f.readback))return false;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=64;
    desc.Height=desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&job->output))))return false;
    const D3D12_DESCRIPTOR_HEAP_DESC views{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,4,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    if(FAILED(device->CreateDescriptorHeap(&views,IID_PPV_ARGS(&f.views))))return false;
    const auto step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for(unsigned image=0;image<2;++image)for(unsigned plane=0;plane<2;++plane) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=plane?DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension=samples>1?D3D12_SRV_DIMENSION_TEXTURE2DMS:D3D12_SRV_DIMENSION_TEXTURE2D;
        if(samples==1){srv.Texture2D.MipLevels=1;srv.Texture2D.PlaneSlice=plane;}
        auto handle=f.views->GetCPUDescriptorHandleForHeapStart();handle.ptr+=(image*2+plane)*step;
        device->CreateShaderResourceView(job->inputs[image].Get(),&srv,handle);
    }
    ID3D12DescriptorHeap* heaps[]={f.views.Get()};f.list->SetDescriptorHeaps(1,heaps);
    f.list->SetComputeRootSignature(reader->root.Get());f.list->SetPipelineState(reader->pipelines[samples>1].Get());
    for(unsigned image=0;image<2;++image) {
        pixel_test::Barrier(f.list.Get(),job->inputs[image].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto handle=f.views->GetGPUDescriptorHandleForHeapStart();handle.ptr+=image*2*step;
        f.list->SetComputeRootDescriptorTable(0,handle);f.list->SetComputeRootUnorderedAccessView(1,job->output->GetGPUVirtualAddress()+image*32);
        const unsigned coordinate[]={probes[image].x,probes[image].y};f.list->SetComputeRoot32BitConstants(2,2,coordinate,0);
        f.list->Dispatch(1,1,1);
        pixel_test::Barrier(f.list.Get(),job->inputs[image].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    }
    pixel_test::Barrier(f.list.Get(),job->output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    f.list->CopyBufferRegion(f.readback.Get(),0,job->output.Get(),0,64);if(FAILED(f.list->Close()))return false;
    ID3D12CommandList* lists[]={f.list.Get()};queue->ExecuteCommandLists(1,lists);
    if(FAILED(queue->Signal(f.fence.Get(),1)) || FAILED(f.fence->SetEventOnCompletion(1,f.event)) ||
       WaitForSingleObject(f.event,30000)!=WAIT_OBJECT_0 || FAILED(device->GetDeviceRemovedReason())){job.release();return false;}
    void* mapping=nullptr;D3D12_RANGE range{0,64};if(FAILED(f.readback->Map(0,&range,&mapping)))return false;
    const auto* words=static_cast<const uint32_t*>(mapping);bool matches=true;
    for(unsigned image=0;image<2;++image)for(unsigned sample=0;sample<samples;++sample)
        matches&=words[image*8+sample*2]==probes[image].depth+sample*probes[image].depth_step &&
            words[image*8+sample*2+1]==(probes[image].stencil^(sample*probes[image].stencil_xor_step));
    D3D12_RANGE none{0,0};f.readback->Unmap(0,&none);if(!matches)REXLOG_ERROR("Auxiliary depth/stencil precision mismatch: {} samples",samples);
    return matches;
}
}

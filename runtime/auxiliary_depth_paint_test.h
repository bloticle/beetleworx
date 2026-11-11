#pragma once
#include "native_pixel_fixture.h"
#include <d3dcompiler.h>
#include <memory>
namespace em2::depth_test {
using Microsoft::WRL::ComPtr;
struct Painter {
    ComPtr<ID3D12RootSignature> root;
    std::array<std::array<ComPtr<ID3D12PipelineState>,4>,3> pipelines;
    bool Initialize(ID3D12Device* device) {
        const char shader[]=R"(
cbuffer Parameters:register(b0){uint bits;};
float4 vs(uint id:SV_VertexID):SV_Position {
 float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);
}
float ps():SV_Depth{return asfloat(bits);}
)";
        D3D12_ROOT_PARAMETER parameter{};parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameter.Constants={0,0,1};
        D3D12_ROOT_SIGNATURE_DESC desc{1,&parameter,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> serialized,errors,vs,ps;
        if(FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors)) ||
           FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root))))return false;
        const auto library=LoadLibraryW(L"d3dcompiler_47.dll");if(!library)return false;
        struct Library {HMODULE value;~Library(){FreeLibrary(value);}}close{library};
        const auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(library,"D3DCompile"));if(!compile)return false;
        if(FAILED(compile(shader,sizeof(shader)-1,"depth-test-painter",nullptr,nullptr,"vs","vs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&errors)) ||
           FAILED(compile(shader,sizeof(shader)-1,"depth-test-painter",nullptr,nullptr,"ps","ps_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&errors)))return false;
        for(unsigned slot=0;slot<3;++slot)for(unsigned sample=0;sample<(1u<<slot);++sample) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};pso.pRootSignature=root.Get();pso.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pso.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
            pso.SampleMask=1u<<sample;pso.SampleDesc.Count=1u<<slot;pso.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pso.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
            pso.RasterizerState.DepthClipEnable=FALSE;pso.RasterizerState.MultisampleEnable=slot!=0;
            pso.DepthStencilState.DepthEnable=TRUE;pso.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;pso.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
            pso.DepthStencilState.StencilEnable=TRUE;pso.DepthStencilState.StencilReadMask=pso.DepthStencilState.StencilWriteMask=0xFF;
            auto& face=pso.DepthStencilState.FrontFace;face.StencilFailOp=face.StencilDepthFailOp=D3D12_STENCIL_OP_KEEP;
            face.StencilPassOp=D3D12_STENCIL_OP_REPLACE;face.StencilFunc=D3D12_COMPARISON_FUNC_ALWAYS;pso.DepthStencilState.BackFace=face;
            pso.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pso.DSVFormat=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            if(FAILED(device->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(&pipelines[slot][sample]))))return false;
        }
        return true;
    }
};
inline bool PaintPattern(ID3D12Device* device,ID3D12CommandQueue* queue,const std::shared_ptr<Painter>& painter,
    ID3D12Resource* texture,D3D12_CPU_DESCRIPTOR_HANDLE native_dsv,D3D12_RESOURCE_STATES before,const D3D12_RECT& detail) {
    struct Job {pixel_test::Fixture gpu;std::shared_ptr<Painter> painter;};
    auto job=std::make_unique<Job>();job->painter=painter;auto& f=job->gpu;f.target=texture;
    const auto desc=texture->GetDesc();const auto samples=desc.SampleDesc.Count;const unsigned slot=samples==1?0:samples==2?1:2;
    if(samples!=1 && samples!=2 && samples!=4)return false;
    const D3D12_DESCRIPTOR_HEAP_DESC dsv_heap{D3D12_DESCRIPTOR_HEAP_TYPE_DSV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    if(!f.event || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f.allocator))) ||
       FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f.allocator.Get(),nullptr,IID_PPV_ARGS(&f.list))) ||
       FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f.fence))) || FAILED(device->CreateDescriptorHeap(&dsv_heap,IID_PPV_ARGS(&f.rtvs))))return false;
    const auto dsv=f.rtvs->GetCPUDescriptorHandleForHeapStart();device->CopyDescriptorsSimple(1,dsv,native_dsv,D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    if(before!=D3D12_RESOURCE_STATE_DEPTH_WRITE)pixel_test::Barrier(f.list.Get(),texture,before,D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const D3D12_VIEWPORT viewport{0,0,float(desc.Width),float(desc.Height),0,1};const D3D12_RECT full{0,0,LONG(desc.Width),LONG(desc.Height)};
    f.list->RSSetViewports(1,&viewport);f.list->OMSetRenderTargets(0,nullptr,FALSE,&dsv);f.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    f.list->SetGraphicsRootSignature(painter->root.Get());
    for(unsigned region=0;region<2;++region)for(unsigned sample=0;sample<samples;++sample) {
        const uint32_t bits=(region?0x3F123457:0x3EAAAAAB)+sample*17;
        f.list->RSSetScissorRects(1,region?&detail:&full);f.list->OMSetStencilRef((region?0x3C:0xA5)^(sample*0x31));
        f.list->SetGraphicsRoot32BitConstants(0,1,&bits,0);f.list->SetPipelineState(painter->pipelines[slot][sample].Get());f.list->DrawInstanced(3,1,0,0);
    }
    if(FAILED(f.list->Close()))return false;ID3D12CommandList* lists[]={f.list.Get()};queue->ExecuteCommandLists(1,lists);
    if(FAILED(queue->Signal(f.fence.Get(),1)) || FAILED(f.fence->SetEventOnCompletion(1,f.event)) ||
       WaitForSingleObject(f.event,30000)!=WAIT_OBJECT_0 || FAILED(device->GetDeviceRemovedReason())){job.release();return false;}
    return true;
}
}

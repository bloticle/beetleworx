#include "edram_capture.h"
#include "gpu_constants_internal.h"
#include "gpu_sync.h"
#include "native_pixel_fixture.h"
#include <d3dcompiler.h>
#include <immintrin.h>
#include <algorithm>
#include <cstring>
namespace {
using Microsoft::WRL::ComPtr;
constexpr char code[]=R"(
ByteAddressBuffer expected:register(t0);
ByteAddressBuffer actual:register(t1);
RWByteAddressBuffer failures:register(u0);
cbuffer Region:register(b0) {uint offset;uint words;};
groupshared uint group_failure;
[numthreads(256,1,1)] void main(uint3 id:SV_DispatchThreadID,uint lane:SV_GroupIndex) {
 if(lane==0)group_failure=0;
 GroupMemoryBarrierWithGroupSync();
 if(id.x<words && expected.Load(offset+id.x*4)!=actual.Load(offset+id.x*4)) InterlockedOr(group_failure,1);
 GroupMemoryBarrierWithGroupSync();
 if(lane==0 && group_failure) {uint ignored;failures.InterlockedOr(0,1,ignored);}
})";
struct Job {
 ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
 ComPtr<ID3D12Fence> fence;ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pipeline;
 ComPtr<ID3D12Resource> expected,actual,failures,zero,readback;
 HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);~Job(){if(event)CloseHandle(event);}
};
}
bool em2::VerifyPackedEdramSnapshots(Em2CommandProcessor& cp,const PackedEdramSnapshot& expected,
 const PackedEdramSnapshot& actual,const std::vector<PackedEdramOwner>& coverage) {
 struct Fp {unsigned csr=_mm_getcsr();~Fp(){_mm_setcsr(csr);}}fp;
 if(!cp.IsGraphicsWorker() || !expected.scale_x || expected.scale_x>7 || !expected.scale_y || expected.scale_y>7 ||
    expected.scale_x!=actual.scale_x || expected.scale_y!=actual.scale_y)return false;
 uint32_t previous=0;for(const auto& owner:coverage) {
  if(owner.begin_tile<previous || owner.begin_tile>=owner.end_tile || owner.end_tile>2048)return false;previous=owner.end_tile;
 }
 if(std::none_of(coverage.begin(),coverage.end(),[](const auto& owner){return owner.key!=0;}))return true;
 const uint64_t area=uint64_t(expected.scale_x)*expected.scale_y,bytes=2048ull*5120*area;
 for(auto* buffer:{expected.buffer.Get(),actual.buffer.Get()}) {
  if(!buffer)return false;const auto desc=buffer->GetDesc();
  if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER || desc.Width<bytes || (desc.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))return false;
 }
 if(expected.buffer.Get()==actual.buffer.Get())return true;
 if(!cp.DrainForResourceChange())return false;
 auto job=std::make_unique<Job>();job->expected=expected.buffer;job->actual=actual.buffer;
 auto* device=cp.GetD3D12Provider().GetDevice();ComPtr<ID3DBlob> shader,signature,errors;
 static HMODULE compiler=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
 static auto compile=compiler?reinterpret_cast<pD3DCompile>(GetProcAddress(compiler,"D3DCompile")):nullptr;
 if(!compile || !job->event || FAILED(compile(code,sizeof(code)-1,"verify-packed",nullptr,nullptr,"main","cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&shader,&errors)))return false;
 D3D12_ROOT_PARAMETER parameters[4]{};
 parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants={0,0,2};
 parameters[1].ParameterType=parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
 parameters[1].Descriptor={0,0};parameters[2].Descriptor={1,0};
 parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[3].Descriptor={0,0};
 D3D12_ROOT_SIGNATURE_DESC root{};root.NumParameters=4;root.pParameters=parameters;
 if(FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&errors)) ||
    FAILED(device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&job->root))))return false;
 D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=job->root.Get();pipeline.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
 if(FAILED(device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&job->pipeline))) ||
    FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&job->allocator))) ||
    FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,job->allocator.Get(),job->pipeline.Get(),IID_PPV_ARGS(&job->list))) ||
    FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&job->fence))) ||
    !pixel_test::Buffer(device,4,D3D12_HEAP_TYPE_UPLOAD,&job->zero) || !pixel_test::Buffer(device,4,D3D12_HEAP_TYPE_READBACK,&job->readback))return false;
 D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
 auto desc=job->zero->GetDesc();desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
 if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&job->failures))))return false;
 void* mapped=nullptr;D3D12_RANGE none{0,0};if(FAILED(job->zero->Map(0,&none,&mapped)))return false;
 *static_cast<uint32_t*>(mapped)=0;job->zero->Unmap(0,nullptr);
 auto* list=job->list.Get();list->CopyBufferRegion(job->failures.Get(),0,job->zero.Get(),0,4);
 pixel_test::Barrier(list,job->failures.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 pixel_test::Barrier(list,job->expected.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 pixel_test::Barrier(list,job->actual.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 list->SetComputeRootSignature(job->root.Get());list->SetComputeRootShaderResourceView(1,job->expected->GetGPUVirtualAddress());
 list->SetComputeRootShaderResourceView(2,job->actual->GetGPUVirtualAddress());list->SetComputeRootUnorderedAccessView(3,job->failures->GetGPUVirtualAddress());
 for(const auto& owner:coverage)if(owner.key) {
  uint64_t offset=uint64_t(owner.begin_tile)*5120*area,left=uint64_t(owner.end_tile-owner.begin_tile)*5120*area;
  while(left) {
   const auto take=uint32_t(std::min<uint64_t>(left,65535ull*256*4));const uint32_t constants[]={uint32_t(offset),take/4};
   list->SetComputeRoot32BitConstants(0,2,constants,0);list->Dispatch((take/4+255)/256,1,1);offset+=take;left-=take;
  }
 }
 pixel_test::Barrier(list,job->expected.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
 pixel_test::Barrier(list,job->actual.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
 pixel_test::Barrier(list,job->failures.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
 list->CopyBufferRegion(job->readback.Get(),0,job->failures.Get(),0,4);
 if(FAILED(list->Close()))return false;ID3D12CommandList* lists[]={list};auto* queue=cp.GetD3D12Provider().GetDirectQueue();queue->ExecuteCommandLists(1,lists);
 if(FAILED(queue->Signal(job->fence.Get(),1)) || FAILED(job->fence->SetEventOnCompletion(1,job->event)) ||
    WaitForSingleObject(job->event,30000)!=WAIT_OBJECT_0 || FAILED(device->GetDeviceRemovedReason())){job.release();return false;}
 D3D12_RANGE read{0,4};if(FAILED(job->readback->Map(0,&read,&mapped)))return false;
 const bool equal=*static_cast<const uint32_t*>(mapped)==0;job->readback->Unmap(0,&none);return equal;
}

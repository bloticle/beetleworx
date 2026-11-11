#include "native_pixel_fixture.h"
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cmath>
#include <immintrin.h>

namespace em2 {
bool TestStencilPlaneCopyPixels(const rex::ui::d3d12::D3D12Provider& provider,const std::filesystem::path& output){
 using namespace pixel_test;struct Fp{unsigned csr=_mm_getcsr();~Fp(){_mm_setcsr(csr);}}fp;
 auto* device=provider.GetDevice();auto* queue=provider.GetDirectQueue();ComPtr<ID3D12InfoQueue> validation;
 if(FAILED(device->QueryInterface(IID_PPV_ARGS(&validation))))return false;
 D3D12_FEATURE_DATA_D3D12_OPTIONS2 options{};const auto feature=device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2,&options,sizeof(options));
 const unsigned positions=SUCCEEDED(feature)?unsigned(options.ProgrammableSamplePositionsTier):0;
 REXLOG_INFO("Stencil plane pixel probe programmable sample-position tier {}",positions);
 const auto first=validation->GetNumStoredMessagesAllowedByRetrievalFilter();
 auto errors_since=[&](uint64_t begin){unsigned errors=0;for(uint64_t i=begin;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;validation->GetMessage(i,nullptr,&size);std::vector<uint8_t> data(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(data.data());if(SUCCEEDED(validation->GetMessage(i,m,&size)) && m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;REXLOG_ERROR("Stencil plane pixel validation: {}",m->pDescription);}}return errors;};
 const auto compiler=LoadLibraryW(L"d3dcompiler_47.dll");if(!compiler)return false;struct Library{HMODULE h;~Library(){FreeLibrary(h);}}library{compiler};auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler,"D3DCompile"));if(!compile)return false;
 const char vertex[]="float4 main(uint id:SV_VertexID):SV_Position{float2 uv=float2((id<<1)&2,id&2);return float4(uv*float2(2,-2)+float2(-1,1),.5,1);}";
 const char pixel[]="uint main(float4 p:SV_Position,uint s:SV_SampleIndex):SV_Target0{return ((uint(p.x)*17)^(uint(p.y)*31)^(s*73))&255;}";
 const char compute[]=R"(
#if MSAA
Texture2DMS<uint> stencil:register(t0);Texture2DMS<float> depth:register(t1);
#else
Texture2D<uint> stencil:register(t0);Texture2D<float> depth:register(t1);
#endif
RWByteAddressBuffer result:register(u0);cbuffer C:register(b0){uint width,height,samples,pad;};
[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){if(any(p.xy>=uint2(width,height)))return;for(uint s=0;s<samples;++s){
#if MSAA
uint v=stencil.Load(p.xy,s);float z=depth.Load(p.xy,s);
#else
uint v=stencil.Load(int3(p.xy,0));float z=depth.Load(int3(p.xy,0));
#endif
result.Store2(((p.y*width+p.x)*samples+s)*8,uint2(v,asuint(z)));}})";
 ComPtr<ID3DBlob> vs,ps,cs[2],error;constexpr UINT flags=D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3;
 if(FAILED(compile(vertex,sizeof(vertex)-1,"stencil-plane-vs",nullptr,nullptr,"main","vs_5_1",flags,0,&vs,&error)) || FAILED(compile(pixel,sizeof(pixel)-1,"stencil-plane-uint",nullptr,nullptr,"main","ps_5_1",flags,0,&ps,&error)))return false;
 for(unsigned i=0;i<2;++i){D3D_SHADER_MACRO macros[]={{"MSAA",i?"1":"0"},{nullptr,nullptr}};if(FAILED(compile(compute,sizeof(compute)-1,"stencil-plane-oracle",macros,nullptr,"main","cs_5_1",flags,0,&cs[i],&error)))return false;}
 constexpr unsigned width=96,height=80;const D3D12_RECT rectangles[]={{2,3,19,24},{30,11,93,36},{8,18,41,72}};uint64_t checked=0,invalid=0;unsigned cases=0;
 for(unsigned format=0;format<2;++format)for(unsigned samples:{1u,4u})for(unsigned partial=0;partial<(positions?2u:1u);++partial){
  struct Owned:Fixture{ComPtr<ID3D12Resource> values;ComPtr<ID3D12DescriptorHeap> dsv;ComPtr<ID3D12RootSignature> compute_root;};auto f=std::make_unique<Owned>();
  if(!f->event || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f->allocator))) || FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f->allocator.Get(),nullptr,IID_PPV_ARGS(&f->list))) || FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f->fence))))return false;
  D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;desc.Height=height;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=samples;desc.Format=format?DXGI_FORMAT_R32G8X24_TYPELESS:DXGI_FORMAT_R24G8_TYPELESS;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_DEPTH_WRITE,nullptr,IID_PPV_ARGS(&f->target))))return false;
  desc.Format=DXGI_FORMAT_R8_UINT;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&f->textures[0]))))return false;
  const uint64_t bytes=uint64_t(width)*height*samples*8;desc={};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;desc.Height=desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&f->values))) || !Buffer(device,bytes,D3D12_HEAP_TYPE_READBACK,&f->readback))return false;
  D3D12_DESCRIPTOR_HEAP_DESC view_heap{D3D12_DESCRIPTOR_HEAP_TYPE_DSV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};if(FAILED(device->CreateDescriptorHeap(&view_heap,IID_PPV_ARGS(&f->dsv))))return false;view_heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;if(FAILED(device->CreateDescriptorHeap(&view_heap,IID_PPV_ARGS(&f->rtvs))))return false;view_heap={D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,3,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};if(FAILED(device->CreateDescriptorHeap(&view_heap,IID_PPV_ARGS(&f->views))))return false;
  D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};dsv.Format=format?DXGI_FORMAT_D32_FLOAT_S8X24_UINT:DXGI_FORMAT_D24_UNORM_S8_UINT;dsv.ViewDimension=samples==4?D3D12_DSV_DIMENSION_TEXTURE2DMS:D3D12_DSV_DIMENSION_TEXTURE2D;const auto dsv_handle=f->dsv->GetCPUDescriptorHandleForHeapStart();device->CreateDepthStencilView(f->target.Get(),&dsv,dsv_handle);
  D3D12_RENDER_TARGET_VIEW_DESC rtv{};rtv.Format=DXGI_FORMAT_R8_UINT;rtv.ViewDimension=samples==4?D3D12_RTV_DIMENSION_TEXTURE2DMS:D3D12_RTV_DIMENSION_TEXTURE2D;const auto rtv_handle=f->rtvs->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(f->textures[0].Get(),&rtv,rtv_handle);
  const auto step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);auto cpu=f->views->GetCPUDescriptorHandleForHeapStart();D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=format?DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:DXGI_FORMAT_X24_TYPELESS_G8_UINT;srv.ViewDimension=samples==4?D3D12_SRV_DIMENSION_TEXTURE2DMS:D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(1,1,1,1);if(samples==1){srv.Texture2D.MipLevels=1;srv.Texture2D.PlaneSlice=1;}device->CreateShaderResourceView(f->target.Get(),&srv,cpu);
  cpu.ptr+=step;srv.Format=format?DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:DXGI_FORMAT_R24_UNORM_X8_TYPELESS;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;if(samples==1)srv.Texture2D.PlaneSlice=0;device->CreateShaderResourceView(f->target.Get(),&srv,cpu);cpu.ptr+=step;D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R32_TYPELESS;uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;uav.Buffer.NumElements=UINT(bytes/4);uav.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;device->CreateUnorderedAccessView(f->values.Get(),nullptr,&uav,cpu);
  ComPtr<ID3DBlob> serialized;D3D12_ROOT_SIGNATURE_DESC root{};root.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;if(FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error)) || FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&f->root))))return false;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};graphics.pRootSignature=f->root.Get();graphics.VS={vs->GetBufferPointer(),vs->GetBufferSize()};graphics.PS={ps->GetBufferPointer(),ps->GetBufferSize()};graphics.SampleMask=UINT_MAX;graphics.SampleDesc.Count=samples;graphics.NumRenderTargets=1;graphics.RTVFormats[0]=DXGI_FORMAT_R8_UINT;graphics.BlendState.RenderTarget[0].RenderTargetWriteMask=1;graphics.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;graphics.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;graphics.RasterizerState.DepthClipEnable=TRUE;graphics.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;if(FAILED(device->CreateGraphicsPipelineState(&graphics,IID_PPV_ARGS(&f->pipelines[0]))))return false;
  D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};D3D12_ROOT_PARAMETER params[3]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[0].Constants={0,0,4};for(unsigned i=0;i<2;++i){params[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[i+1].DescriptorTable={1,&ranges[i]};}root={3,params};serialized.Reset();if(FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error)) || FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&f->compute_root))))return false;D3D12_COMPUTE_PIPELINE_STATE_DESC compute_pso{};compute_pso.pRootSignature=f->compute_root.Get();compute_pso.CS={cs[samples==4]->GetBufferPointer(),cs[samples==4]->GetBufferSize()};if(FAILED(device->CreateComputePipelineState(&compute_pso,IID_PPV_ARGS(&f->pipelines[1]))))return false;
  f->list->ClearDepthStencilView(dsv_handle,D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL,.625f,0xA5,0,nullptr);
  D3D12_TEXTURE_COPY_LOCATION depth_plane{},r8_plane{};depth_plane.pResource=f->target.Get();depth_plane.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;depth_plane.SubresourceIndex=1;r8_plane.pResource=f->textures[0].Get();r8_plane.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  if(!partial){
   Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_COPY_SOURCE);Barrier(f->list.Get(),f->textures[0].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST);
   f->list->CopyTextureRegion(&r8_plane,0,0,0,&depth_plane,nullptr);
   Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);Barrier(f->list.Get(),f->textures[0].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);
  }else Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_COPY_DEST);
  f->list->OMSetRenderTargets(1,&rtv_handle,FALSE,nullptr);f->list->SetGraphicsRootSignature(f->root.Get());f->list->SetPipelineState(f->pipelines[0].Get());f->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);const D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1};f->list->RSSetViewports(1,&viewport);for(const auto& rect:rectangles){f->list->RSSetScissorRects(1,&rect);f->list->DrawInstanced(3,1,0,0);}
  Barrier(f->list.Get(),f->textures[0].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);if(partial){for(const auto& rect:rectangles){D3D12_BOX box{UINT(rect.left),UINT(rect.top),0,UINT(rect.right),UINT(rect.bottom),1};f->list->CopyTextureRegion(&depth_plane,rect.left,rect.top,0,&r8_plane,&box);}}else f->list->CopyTextureRegion(&depth_plane,0,0,0,&r8_plane,nullptr);Barrier(f->list.Get(),f->target.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  auto* views=f->views.Get();f->list->SetDescriptorHeaps(1,&views);f->list->SetComputeRootSignature(f->compute_root.Get());f->list->SetPipelineState(f->pipelines[1].Get());const unsigned dimensions[]={width,height,samples,0};f->list->SetComputeRoot32BitConstants(0,4,dimensions,0);auto gpu=f->views->GetGPUDescriptorHandleForHeapStart();f->list->SetComputeRootDescriptorTable(1,gpu);gpu.ptr+=2*step;f->list->SetComputeRootDescriptorTable(2,gpu);f->list->Dispatch((width+7)/8,(height+7)/8,1);Barrier(f->list.Get(),f->values.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);f->list->CopyBufferRegion(f->readback.Get(),0,f->values.Get(),0,bytes);
  if(FAILED(f->list->Close()) || errors_since(first))return false;ID3D12CommandList* lists[]={f->list.Get()};queue->ExecuteCommandLists(1,lists);if(FAILED(queue->Signal(f->fence.Get(),1)) || FAILED(f->fence->SetEventOnCompletion(1,f->event)) || WaitForSingleObject(f->event,15000)!=WAIT_OBJECT_0){f.release();return false;}
  void* mapped=nullptr;D3D12_RANGE read{0,SIZE_T(bytes)},empty{};if(FAILED(f->readback->Map(0,&read,&mapped)))return false;const auto* values=static_cast<const uint32_t*>(mapped);const auto* floats=static_cast<const float*>(mapped);
  const auto before=invalid;for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)for(unsigned s=0;s<samples;++s){bool inside=false;for(const auto& r:rectangles)inside|=x>=unsigned(r.left) && x<unsigned(r.right) && y>=unsigned(r.top) && y<unsigned(r.bottom);const auto expected=inside?((x*17)^(y*31)^(s*73))&255:0xA5;const auto index=((y*width+x)*samples+s)*2;const bool bad=values[index]!=expected || !std::isfinite(floats[index+1]) || std::abs(floats[index+1]-.625f)>1e-6f;if(bad && invalid==before)REXLOG_ERROR("Stencil copy pixel mismatch format{} samples{} partial{} at {},{},{} stencil{} expected{} depth{}",format,samples,partial,x,y,s,values[index],expected,floats[index+1]);invalid+=bad;++checked;}f->readback->Unmap(0,&empty);++cases;
 }
 const auto device_errors=errors_since(first);std::filesystem::create_directories(output);std::ofstream report(output/"stencil-plane-copy-pixels.json");report<<"{\"cases\":"<<cases<<",\"sample_position_tier\":"<<positions<<",\"sample_pairs\":"<<checked<<",\"invalid\":"<<invalid<<",\"device_errors\":"<<device_errors<<",\"scope\":\"Actual R8_UINT render texture to/from stencil plane copies. D24S8/D32S8, 1x/4x MSAA, varying per-sample values, disjoint/overlapping edits; untouched stencil and all original depth values checked. Whole-plane and, when supported, partial copies with untouched destination checks. Test shaders, not native transfer shader integration or performance evidence.\"}\n";REXLOG_INFO("Stencil plane GPU pixels: {} cases, {} depth/stencil sample pairs, {} invalid, {} device errors",cases,checked,invalid,device_errors);return !invalid && !device_errors && report.good();
}
}

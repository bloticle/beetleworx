#include "native_pixel_fixture.h"
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <d3d12sdklayers.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

namespace em2 {
// Recording-only capability probe. Invalid candidate copies are discarded;
// nothing is submitted to the GPU. Acceptance still requires a pixel oracle.
bool ProbeStencilPlaneCopies(const rex::ui::d3d12::D3D12Provider& provider,const std::filesystem::path& output,bool* r8_accepted){
 if(r8_accepted)*r8_accepted=false;
 using namespace pixel_test;auto* device=provider.GetDevice();
 ComPtr<ID3D12InfoQueue> validation;if(FAILED(device->QueryInterface(IID_PPV_ARGS(&validation)))){
  REXLOG_ERROR("Stencil plane copy probe requires --d3d12_debug=true; no candidate commands recorded");return false;
 }
 validation->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION,FALSE);validation->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR,FALSE);validation->ClearStorageFilter();validation->ClearRetrievalFilter();
 std::filesystem::create_directories(output);std::ofstream report(output/"stencil-plane-copy-recording.json");
 report<<"{\"candidate_gpu_commands_submitted\":0,\"pixel_correctness_proven\":false,\"cases\":[";
 unsigned cases=0,controls=0,accepted=0,r8_candidates=0;
 for(unsigned format=0;format<2;++format)for(unsigned samples:{1u,4u})for(unsigned route=0;route<3;++route){
  if(route==2 && samples!=1)continue; // Buffer staging cannot address MSAA samples.
  const char* name=route==0?"depth_stencil_control":route==1?"r8_render_texture":"r8_buffer_footprint";
  ComPtr<ID3D12Resource> source,destination;ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
  const auto first=validation->GetNumStoredMessagesAllowedByRetrievalFilter();
  D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
  D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=desc.Height=64;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=samples;desc.Format=format?DXGI_FORMAT_R32G8X24_TYPELESS:DXGI_FORMAT_R24G8_TYPELESS;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  const auto dest_created=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&destination));
  auto source_desc=desc;
  if(route==1){source_desc.Format=DXGI_FORMAT_R8_UINT;source_desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;}
  if(route==2){source_desc={};source_desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;source_desc.Width=256*64;source_desc.Height=source_desc.DepthOrArraySize=source_desc.MipLevels=source_desc.SampleDesc.Count=1;source_desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;}
  const auto source_created=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&source_desc,D3D12_RESOURCE_STATE_COPY_SOURCE,nullptr,IID_PPV_ARGS(&source));
  HRESULT closed=E_FAIL;
  if(SUCCEEDED(dest_created) && SUCCEEDED(source_created)){
   if(FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))) || FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))))return false;
   D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=destination.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=1;
   D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=source.Get();
   if(route==2){src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint.Footprint={DXGI_FORMAT_R8_UINT,64,64,1,256};}
   else{src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.SubresourceIndex=route==0?1:0;}
   list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);closed=list->Close();
  }
  std::vector<std::string> errors;
  for(uint64_t i=first;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;validation->GetMessage(i,nullptr,&size);std::vector<uint8_t> bytes(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());if(SUCCEEDED(validation->GetMessage(i,message,&size)) && message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::string text(message->pDescription);for(auto& c:text)if(static_cast<unsigned char>(c)<32)c=' ';errors.push_back(std::move(text));}}
  const bool valid=SUCCEEDED(dest_created) && SUCCEEDED(source_created) && SUCCEEDED(closed) && errors.empty();
  if(route==0)controls+=valid;else accepted+=valid;
  if(route==1)r8_candidates+=valid;
  if(cases++)report<<',';report<<"{\"format\":"<<format<<",\"samples\":"<<samples<<",\"route\":"<<std::quoted(name)<<",\"recording_accepted\":"<<(valid?"true":"false")<<",\"source_creation_hresult\":"<<uint32_t(source_created)<<",\"destination_creation_hresult\":"<<uint32_t(dest_created)<<",\"close_hresult\":"<<uint32_t(closed)<<",\"errors\":[";
  for(size_t i=0;i<errors.size();++i){if(i)report<<',';report<<std::quoted(errors[i]);REXLOG_INFO("Stencil copy probe rejected {} format{} samples{}: {}",name,format,samples,errors[i]);}report<<"]}";
  REXLOG_INFO("Stencil plane recording probe {} format{} samples{} accepted {}; command list discarded without GPU execution",name,format,samples,valid);
 }
 report<<"],\"valid_controls\":"<<controls<<",\"accepted_candidates\":"<<accepted<<"}\n";
 if(r8_accepted)*r8_accepted=r8_candidates==4;
 return controls==4 && report.good();
}
}

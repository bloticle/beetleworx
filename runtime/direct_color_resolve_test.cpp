#include "direct_color_resolve.h"
#include "native_pixel_fixture.h"
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/render_target_cache.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/trace_writer.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <d3d12sdklayers.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <vector>
namespace {
using namespace em2::pixel_test;
using Cache=rex::graphics::d3d12::D3D12RenderTargetCache;
using CP=rex::graphics::d3d12::D3D12CommandProcessor;
template<class T,auto M>struct Access{friend auto Member(T){return M;}};
template<class T,class V>struct Type{friend auto Value(T){return std::type_identity<V>{};}};
#define MEMBER(tag,type,name) struct tag{friend auto Member(tag);};template struct Access<tag,&type::name>
#define TYPE(tag,type) struct tag{friend auto Value(tag);};template struct Type<tag,type>
MEMBER(Memory,rex::graphics::CommandProcessor,memory_);
MEMBER(Dump,Cache,GetOrCreateDumpPipeline);MEMBER(DumpRoot,Cache,dump_root_signature_color_);
MEMBER(ResolveRoot,Cache,resolve_copy_root_signature_);MEMBER(ResolvePipelines,Cache,resolve_copy_pipelines_);
TYPE(DumpKeyTag,Cache::DumpPipelineKey);TYPE(OffsetsTag,Cache::DumpOffsets);TYPE(PitchesTag,Cache::DumpPitches);
using DumpKey=typename decltype(Value(DumpKeyTag{}))::type;
using Offsets=typename decltype(Value(OffsetsTag{}))::type;
using Pitches=typename decltype(Value(PitchesTag{}))::type;
struct Owned:Fixture {
 std::unique_ptr<rex::graphics::RegisterFile> regs;
 std::unique_ptr<rex::graphics::TraceWriter> trace;
 std::unique_ptr<Cache> cache;
 ComPtr<ID3D12Resource> source,edram,reference,native;
 ComPtr<ID3D12DescriptorHeap> clear_views;
};
struct ValidationReport {
 ComPtr<ID3D12InfoQueue> queue;uint64_t first=0;std::filesystem::path output;bool flushed=false;uint64_t errors=0;
 bool Flush(){
  if(flushed)return errors==0;flushed=true;uint64_t warnings=0,omitted=0;
  std::ofstream details(output/"direct-color-resolve-validation.txt");
  if(queue){const auto end=queue->GetNumStoredMessagesAllowedByRetrievalFilter();
   for(uint64_t i=first;i<end;++i){
    if(i-first>=4096){omitted=end-i;++errors;break;}
    SIZE_T size=0;if(FAILED(queue->GetMessage(i,nullptr,&size))){++errors;continue;}
    std::vector<uint8_t> storage(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
    if(FAILED(queue->GetMessage(i,message,&size))){++errors;continue;}
    const bool bad=message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR;errors+=bad;warnings+=message->Severity==D3D12_MESSAGE_SEVERITY_WARNING;
    details<<i<<' '<<unsigned(message->Severity)<<' '<<message->pDescription<<'\n';
    if(bad&&errors<=16)REXLOG_ERROR("Direct resolve D3D validation: {}",message->pDescription);
   }
  }
  std::ofstream(output/"direct-color-resolve-validation.json")<<"{\"available\":"<<(queue?"true":"false")<<",\"errors\":"<<errors<<",\"warnings\":"<<warnings<<",\"messages_omitted\":"<<omitted<<"}";
  return errors==0;
 }
 ~ValidationReport(){if(!flushed)Flush();}
};
bool UavBuffer(ID3D12Device* device,uint64_t size,ID3D12Resource** out){
 D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
 D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=size;
 desc.Height=desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
 return SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(out)));
}
}
bool em2::TestDirectColorResolve(CP& cp,const std::filesystem::path& output){
 using namespace rex::graphics;
 std::filesystem::create_directories(output);std::ofstream progress(output/"direct-color-resolve-progress.txt");
 auto checkpoint=[&](const char* stage){progress<<stage<<'\n';progress.flush();};
 checkpoint("start");
 const std::array<DirectColorResolveRegion,3> split={{{0,3,4},{3,2,4},{5,3,4}}};
 if(!DirectColorResolveCoverage(split,8,4,11)||DirectColorResolveCoverage(std::span(split).first(2),8,4,11))return false;
 auto overlap=split;overlap[1].offset=2;if(DirectColorResolveCoverage(overlap,8,4,11))return false;
 auto outside=split;outside[2].width=4;if(DirectColorResolveCoverage(outside,8,4,11))return false;
 checkpoint("coverage guards passed");
 constexpr uint32_t sx=2,sy=2,pitch=4,native_width=pitch*80*sx,native_height=256;
 constexpr uint64_t buffer_bytes=2*1024*1024;
 auto f=std::make_unique<Owned>();auto* memory=cp.*Member(Memory{});auto& provider=cp.GetD3D12Provider();auto* device=provider.GetDevice();auto* queue=provider.GetDirectQueue();
 ValidationReport validation;validation.output=output;device->QueryInterface(IID_PPV_ARGS(&validation.queue));if(validation.queue)validation.first=validation.queue->GetNumStoredMessagesAllowedByRetrievalFilter();
 if(!memory||!f->event)return false;
 checkpoint("create command objects");
 if(FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f->allocator)))||
    FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f->allocator.Get(),nullptr,IID_PPV_ARGS(&f->list)))||
    FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f->fence))))return false;
 f->regs=std::make_unique<RegisterFile>();f->trace=std::make_unique<TraceWriter>(memory->physical_membase());
 checkpoint("before cache initialization");
 f->cache=std::make_unique<Cache>(*f->regs,*memory,*f->trace,sx,sy,cp,false);if(!f->cache->Initialize())return false;
 checkpoint("after cache initialization; before direct program creation");
 auto program=PrepareDirectColorResolve(f->cache.get(),device);if(!program)return false;
 checkpoint("after direct program creation");
 struct ProgramLifetime{const Cache* cache;~ProgramLifetime(){ShutdownDirectColorResolve(cache);}}program_lifetime{f->cache.get()};
 DumpKey dump_key;dump_key.msaa_samples=xenos::MsaaSamples::k1X;dump_key.resource_format=0;dump_key.is_depth=0;
 checkpoint("before original dump pipeline");
 auto* dump_pipeline=(f->cache.get()->*Member(Dump{}))(dump_key);if(!dump_pipeline)return false;
 checkpoint("after original dump pipeline; before original resolve pipeline");
 auto* resolve_pipeline=(f->cache.get()->*Member(ResolvePipelines{}))[size_t(draw_util::ResolveCopyShaderIndex::kFast32bpp1x2xMSAA)];
 if(!resolve_pipeline)return false;
 checkpoint("after original pipelines; before resources");
 D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
 D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;texture.Width=native_width;texture.Height=native_height;
 texture.DepthOrArraySize=texture.MipLevels=texture.SampleDesc.Count=1;texture.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
 if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&f->source)))||
    !UavBuffer(device,buffer_bytes,&f->edram)||!UavBuffer(device,buffer_bytes,&f->reference)||!UavBuffer(device,buffer_bytes,&f->native)||
    !Buffer(device,buffer_bytes*2,D3D12_HEAP_TYPE_READBACK,&f->readback))return false;
 checkpoint("after resources; before upload mapping");
 D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};uint64_t upload_size;
 device->GetCopyableFootprints(&texture,0,1,0,&footprint,nullptr,nullptr,&upload_size);
 if(!Buffer(device,upload_size,D3D12_HEAP_TYPE_UPLOAD,&f->upload))return false;
 uint8_t* upload=nullptr;if(FAILED(f->upload->Map(0,nullptr,reinterpret_cast<void**>(&upload))))return false;
 for(uint32_t y=0;y<native_height;++y)for(uint32_t x=0;x<native_width;++x){
  auto* p=upload+footprint.Offset+y*footprint.Footprint.RowPitch+x*4;
  p[0]=uint8_t(x+3*y);p[1]=uint8_t(7*x+y);p[2]=uint8_t(x^y);p[3]=uint8_t(11*x+13*y);
 }
 f->upload->Unmap(0,nullptr);
 checkpoint("after upload mapping");
 D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=f->upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprint;
 D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=f->source.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
 f->list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Barrier(f->list.Get(),f->source.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 D3D12_DESCRIPTOR_HEAP_DESC views{};views.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;views.NumDescriptors=5;views.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
 if(FAILED(device->CreateDescriptorHeap(&views,IID_PPV_ARGS(&f->views))))return false;
 auto clear_desc=views;clear_desc.NumDescriptors=2;clear_desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
 if(FAILED(device->CreateDescriptorHeap(&clear_desc,IID_PPV_ARGS(&f->clear_views))))return false;
 const auto step=device->GetDescriptorHandleIncrementSize(views.Type);
 auto cpu=[&](unsigned i){auto h=f->views->GetCPUDescriptorHandleForHeapStart();h.ptr+=i*step;return h;};
 auto gpu=[&](unsigned i){auto h=f->views->GetGPUDescriptorHandleForHeapStart();h.ptr+=i*step;return h;};
 auto clear_cpu=[&](unsigned i){auto h=f->clear_views->GetCPUDescriptorHandleForHeapStart();h.ptr+=i*step;return h;};
 D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=texture.Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
 device->CreateShaderResourceView(f->source.Get(),&srv,cpu(0));
 D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R32_UINT;uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;uav.Buffer.NumElements=uint32_t(buffer_bytes/4);
 device->CreateUnorderedAccessView(f->edram.Get(),nullptr,&uav,cpu(1));
 srv={};srv.Format=DXGI_FORMAT_R32G32B32A32_UINT;srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Buffer.NumElements=uint32_t(buffer_bytes/16);
 device->CreateShaderResourceView(f->edram.Get(),&srv,cpu(2));
 uav.Format=DXGI_FORMAT_R32G32B32A32_UINT;uav.Buffer.NumElements=uint32_t(buffer_bytes/16);
 device->CreateUnorderedAccessView(f->reference.Get(),nullptr,&uav,cpu(3));device->CreateUnorderedAccessView(f->native.Get(),nullptr,&uav,cpu(4));
 // UAV clear's CPU descriptor must come from a non-shader-visible heap, with
 // contents matching the GPU-visible descriptor. Do not use cpu(3)/cpu(4).
 device->CreateUnorderedAccessView(f->reference.Get(),nullptr,&uav,clear_cpu(0));device->CreateUnorderedAccessView(f->native.Get(),nullptr,&uav,clear_cpu(1));
 checkpoint("after shader descriptors");
 uint64_t signal=0,checked=0,invalid=0,changed_total=0;unsigned cases=0;
 std::vector<uint64_t> changed_per_case;
 auto submit=[&](){if(FAILED(f->list->Close()))return false;ID3D12CommandList* lists[]={f->list.Get()};queue->ExecuteCommandLists(1,lists);if(FAILED(queue->Signal(f->fence.Get(),++signal)))return false;if(f->fence->GetCompletedValue()<signal){if(FAILED(f->fence->SetEventOnCompletion(signal,f->event))||WaitForSingleObject(f->event,10000)!=WAIT_OBJECT_0)return false;}return true;};
 auto reset=[&](){return SUCCEEDED(f->allocator->Reset())&&SUCCEEDED(f->list->Reset(f->allocator.Get(),nullptr));};
 checkpoint("before upload submission");if(!submit()||!reset())return false;checkpoint("after upload submission");
 for(unsigned n=0;n<96;++n){
  progress<<"case "<<n<<'\n';progress.flush();
  if(n){if(!reset())return false;Barrier(f->list.Get(),f->reference.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);Barrier(f->list.Get(),f->native.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);Barrier(f->list.Get(),f->edram.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);}
  const uint32_t base=std::array<uint32_t,3>{0,13,37}[n%3];
  draw_util::ResolveInfo info{};info.color_edram_info.base_tiles=base;info.color_edram_info.pitch_tiles=pitch;info.color_edram_info.msaa_samples=xenos::MsaaSamples::k1X;
  info.coordinate_info.edram_offset_x_div_8=(n/3)%4;info.coordinate_info.edram_offset_y_div_8=(n/12)%2;
  info.coordinate_info.width_div_8=std::array<uint32_t,4>{2,8,14,16}[(n/6)%4];info.height_div_8=std::array<uint32_t,4>{1,2,3,5}[(n/24)%4];
  info.coordinate_info.draw_resolution_scale_x=sx;info.coordinate_info.draw_resolution_scale_y=sy;
  info.color_edram_info.fill_half_pixel_offset=(n/48)%2;
  info.copy_dest_info.copy_dest_format=xenos::ColorFormat::k_8_8_8_8;info.copy_dest_info.copy_dest_endian=static_cast<xenos::Endian128>((n/2)%6);info.copy_dest_info.copy_dest_swap=n%2;
  info.copy_dest_coordinate_info.pitch_aligned_div_32=8;info.copy_dest_coordinate_info.height_aligned_div_32=4;
  info.copy_dest_coordinate_info.offset_x_div_8=(n/8)%3;info.copy_dest_coordinate_info.offset_y_div_8=(n/16)%3;
  uint32_t db,dw,dr,dp;info.GetCopyEdramTileSpan(db,dw,dr,dp);
  DirectColorResolveSource source{f->source.Get(),cpu(0),texture,base,pitch,0,0};
  if(n==0)checkpoint("before first validation");
  if(!ValidateDirectColorResolve(info,source,sx,sy,db,dw,dr,dp)){REXLOG_ERROR("Direct resolve oracle case {} rejected its supported layout",n);return false;}
  if(n==0)checkpoint("after first validation");
  draw_util::ResolveCopyShaderConstants constants;uint32_t gx,gy;
  if(n==0)checkpoint("before first GetCopyShader");
  if(info.GetCopyShader(sx,sy,constants,gx,gy)!=draw_util::ResolveCopyShaderIndex::kFast32bpp1x2xMSAA)return false;
  if(n==0){progress<<"first dispatch groups "<<gx<<' '<<gy<<" packed constants "<<std::hex<<constants.dest_relative.edram_info.packed<<' '<<constants.dest_relative.coordinate_info.packed<<' '<<constants.dest_relative.dest_info.value<<' '<<constants.dest_relative.dest_coordinate_info.packed<<std::dec<<'\n';progress.flush();}
  if(n==0)checkpoint("after first GetCopyShader; before descriptor heaps");
  ID3D12DescriptorHeap* heaps[]={f->views.Get()};f->list->SetDescriptorHeaps(1,heaps);
  const UINT poison[4]={0xCDCDCDCD,0xCDCDCDCD,0xCDCDCDCD,0xCDCDCDCD};
  if(n==0)checkpoint("before first reference poison clear");
  f->list->ClearUnorderedAccessViewUint(gpu(3),clear_cpu(0),f->reference.Get(),poison,0,nullptr);
  if(n==0)checkpoint("after first reference poison clear; before native poison clear");
  f->list->ClearUnorderedAccessViewUint(gpu(4),clear_cpu(1),f->native.Get(),poison,0,nullptr);
  if(n==0)checkpoint("after first native poison clear");
  D3D12_RESOURCE_BARRIER ordering[2]{};for(unsigned i=0;i<2;++i){ordering[i].Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;ordering[i].UAV.pResource=i?f->native.Get():f->reference.Get();}f->list->ResourceBarrier(2,ordering);
  if(n==0)checkpoint("before first dump dispatch");
  f->list->SetComputeRootSignature(f->cache.get()->*Member(DumpRoot{}));f->list->SetPipelineState(dump_pipeline);
  Offsets offsets;offsets.dispatch_first_tile=base;offsets.source_base_tiles=base;Pitches pitches;pitches.dest_pitch=pitch;pitches.source_pitch=pitch;
  f->list->SetComputeRoot32BitConstants(0,1,&offsets,0);f->list->SetComputeRootDescriptorTable(1,gpu(0));f->list->SetComputeRoot32BitConstants(2,1,&pitches,0);f->list->SetComputeRootDescriptorTable(3,gpu(1));
  f->list->Dispatch(native_width/40,native_height/16,1);Barrier(f->list.Get(),f->edram.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if(n==0)checkpoint("before first original resolve dispatch");
  f->list->SetComputeRootSignature(f->cache.get()->*Member(ResolveRoot{}));f->list->SetPipelineState(resolve_pipeline);
  f->list->SetComputeRoot32BitConstants(0,4,&constants.dest_relative,0);f->list->SetComputeRootDescriptorTable(1,gpu(3));f->list->SetComputeRootDescriptorTable(2,gpu(2));f->list->Dispatch(gx,gy,1);
  if(n==0)checkpoint("before first direct resolve dispatch");
  f->list->SetComputeRootSignature(program->root.Get());f->list->SetPipelineState(program->pipeline.Get());const uint32_t source_constants[]={base,pitch,sx,sy};
  f->list->SetComputeRoot32BitConstants(0,4,&constants.dest_relative,0);f->list->SetComputeRoot32BitConstants(1,4,source_constants,0);f->list->SetComputeRootDescriptorTable(2,gpu(0));f->list->SetComputeRootDescriptorTable(3,gpu(4));f->list->Dispatch(gx,gy,1);
  Barrier(f->list.Get(),f->reference.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);Barrier(f->list.Get(),f->native.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
  f->list->CopyBufferRegion(f->readback.Get(),0,f->reference.Get(),0,buffer_bytes);f->list->CopyBufferRegion(f->readback.Get(),buffer_bytes,f->native.Get(),0,buffer_bytes);
  if(n==0)checkpoint("before first result submission");if(!submit())return false;if(n==0)checkpoint("before first result mapping");
  const uint8_t* mapped=nullptr;D3D12_RANGE read{0,size_t(buffer_bytes*2)};if(FAILED(f->readback->Map(0,&read,reinterpret_cast<void**>(const_cast<uint8_t**>(&mapped)))))return false;
  uint64_t changed=0;
  for(uint64_t i=0;i<buffer_bytes;++i){++checked;changed+=mapped[i]!=0xCD;if(mapped[i]!=mapped[buffer_bytes+i]){++invalid;if(invalid<=8)REXLOG_ERROR("Direct resolve raw-byte mismatch case {} offset {}: native {}, original {}",n,i,mapped[buffer_bytes+i],mapped[i]);}}
  changed_per_case.push_back(changed);changed_total+=changed;
  if(!changed){++invalid;REXLOG_ERROR("Direct resolve oracle case {} left the entire reference at poison; rejecting vacuous equality",n);}
  const D3D12_RANGE no_write{0,0};f->readback->Unmap(0,&no_write);++cases;
  if(n==0)checkpoint("after first result comparison");
 }
 std::filesystem::create_directories(output);std::ofstream report(output/"direct-color-resolve.json");
 report<<"{\"cases\":"<<cases<<",\"raw_bytes_compared_including_guards\":"<<checked<<",\"invalid\":"<<invalid<<",\"reference_changed_bytes\":"<<changed_total<<",\"reference_changed_bytes_per_case\":[";
 for(size_t i=0;i<changed_per_case.size();++i)report<<(i?",":"")<<changed_per_case[i];
 report<<"],\"original\":\"actual installed native RGBA8 dump plus fast32 scaled resolve\",\"scope\":\"1x samples, 2x render scale, source bases/offsets, rectangles, destination offsets, six endian modes, red/blue swap, half-pixel fill; mixed-owner rejection remains caller preflight\"}";
 REXLOG_INFO("Direct native color resolve: {} cases, {} raw bytes including untouched guards, {} invalid",cases,checked,invalid);
 checkpoint("completed");
 return invalid==0&&validation.Flush();
}

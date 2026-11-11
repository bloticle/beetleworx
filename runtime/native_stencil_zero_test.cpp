#include "native_pixel_fixture.h"
#include "stencil_zero_proof.h"
#include "stencil_transfer.h"
#include "bounded_transfer_division.h"
#include "stencil_consumer_survey.h"
#include "direct_color_resolve.h"
#include <rex/graphics/util/draw_extent_estimator.h>
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/string.h>
#include "../private/draw_extent_clear_test_data.h"
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/deferred_command_list.h>
#include <rex/graphics/d3d12/render_target_cache.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/trace_writer.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>
#include <immintrin.h>
#include <bit>
REXCVAR_DECLARE(bool,em2_skip_proven_zero_stencil);
REXCVAR_DECLARE(bool,em2_stencil_plane_copy);
REXCVAR_DECLARE(bool,em2_skip_stencil_scratch_clear);
REXCVAR_DECLARE(bool,em2_stencil_plane_barriers);
REXCVAR_DECLARE(bool,em2_coalesce_stencil_copies);
REXCVAR_DECLARE(bool,em2_fuse_depth_stencil_transfers);
REXCVAR_DECLARE(bool,execute_unclipped_draw_vs_on_cpu);
REXCVAR_DECLARE(bool,execute_unclipped_draw_vs_on_cpu_with_scissor);
namespace {
using namespace em2::pixel_test;
using Cache=rex::graphics::d3d12::D3D12RenderTargetCache;
using Base=rex::graphics::RenderTargetCache;
using CP=rex::graphics::d3d12::D3D12CommandProcessor;
template<class T,auto M>struct Access{friend auto Member(T){return M;}};
template<class T,class V>struct Type{friend auto Value(T){return std::type_identity<V>{};}};
#define MEMBER(tag,type,name) struct tag{friend auto Member(tag);};template struct Access<tag,&type::name>
#define TYPE(tag,type) struct tag{friend auto Value(tag);};template struct Type<tag,type>
TYPE(KeyTag,Base::RenderTargetKey);TYPE(HostTag,Cache::D3D12RenderTarget);TYPE(TargetTag,Base::RenderTarget);TYPE(TransferTag,Base::Transfer);
using Key=typename decltype(Value(KeyTag{}))::type;using Host=typename decltype(Value(HostTag{}))::type;using Target=typename decltype(Value(TargetTag{}))::type;using Transfer=typename decltype(Value(TransferTag{}))::type;
MEMBER(Create,Cache,CreateRenderTarget);MEMBER(Clear,Cache,PerformTransfersAndResolveClears);MEMBER(StencilOutput,Cache,use_stencil_reference_output_);
MEMBER(Memory,rex::graphics::CommandProcessor,memory_);
MEMBER(External,CP,current_external_pipeline_);MEMBER(Guest,CP,current_guest_pipeline_);MEMBER(Root,CP,current_graphics_root_signature_);MEMBER(Topology,CP,primitive_topology_);MEMBER(Viewport,CP,ff_viewport_update_needed_);MEMBER(Scissor,CP,ff_scissor_update_needed_);
MEMBER(StencilDirty,CP,ff_stencil_ref_update_needed_);
MEMBER(Bindless,CP,bindless_resources_used_);MEMBER(BindlessHeap,CP,view_bindless_heap_);MEMBER(BindfulHeap,CP,view_bindful_heap_current_);
struct Owned:Fixture{
 std::unique_ptr<rex::graphics::RegisterFile> registers;
 std::unique_ptr<rex::graphics::TraceWriter> trace;
 std::unique_ptr<Cache> cache;
 std::unique_ptr<Host> source,destination;
 ComPtr<ID3D12Resource> values;
};
// Exercise the installed ownership-height estimator with the actual tiny D3D
// clear shader and inputs. This is CPU-only and never changes guest registers,
// render targets or command lists. Private shader/input bytes stay in private/.
bool TestClearDrawExtent(CP& cp,const std::filesystem::path& output){
 using namespace rex::graphics;
 std::filesystem::create_directories(output);
 std::ofstream progress(output/"clear-draw-extent-progress.txt");
 auto checkpoint=[&](const char* stage){progress<<stage<<'\n';progress.flush();};
 checkpoint("start");
 struct Flags {
  bool execute=REXCVAR_GET(execute_unclipped_draw_vs_on_cpu);
  bool scissor=REXCVAR_GET(execute_unclipped_draw_vs_on_cpu_with_scissor);
  ~Flags(){REXCVAR_SET(execute_unclipped_draw_vs_on_cpu,execute);REXCVAR_SET(execute_unclipped_draw_vs_on_cpu_with_scissor,scissor);}
 }flags;
 auto* memory=cp.*Member(Memory{});
 checkpoint("allocate physical page");
 const uint32_t address=memory->SystemHeapAlloc(4096,4096,rex::memory::kSystemHeapPhysical);
 if(!address)return false;
 struct Allocation {rex::memory::Memory* memory;uint32_t address;~Allocation(){memory->SystemHeapFree(address);}}allocation{memory,address};
 // Physical heaps may add a host/address-space offset. The allocation's guest
 // address is not itself an Xenos physical address with high bits stripped.
 const uint32_t physical=memory->GetPhysicalAddress(address);
 auto* physical_bytes=memory->TranslatePhysical<uint8_t*>(physical);
 MEMORY_BASIC_INFORMATION region{};
 const bool query_ok=VirtualQuery(physical_bytes,&region,sizeof(region))==sizeof(region);
 progress<<"guest "<<std::hex<<address<<" physical "<<physical<<" physical_host "<<reinterpret_cast<uintptr_t>(physical_bytes)
  <<" state "<<region.State<<" protect "<<region.Protect<<std::dec<<'\n';progress.flush();
 if(uint64_t(physical)+4096>0x20000000ull||!query_ok||region.State!=MEM_COMMIT||
    (region.Protect&(PAGE_GUARD|PAGE_NOACCESS))||
    reinterpret_cast<uintptr_t>(physical_bytes)+4096>reinterpret_cast<uintptr_t>(region.BaseAddress)+region.RegionSize){
  REXLOG_ERROR("Clear extent physical allocation is not readable: guest {:08X}, physical {:08X}",address,physical);return false;
 }
 // SystemHeapAlloc returns the writable guest alias. The raw physical view may
 // be read-only for GPU memory tracking; do not bypass its protection.
 auto* bytes=memory->TranslateVirtual<uint8_t*>(address);
 checkpoint("analyze captured shader");
 DxbcShader shader(xenos::ShaderType::kVertex,0x1E6883FCCDE1F688ull,
  em2::draw_extent_test_data::shader,std::size(em2::draw_extent_test_data::shader),std::endian::native);
 rex::string::StringBuffer text;shader.AnalyzeUcode(text);
 if(!ShaderInterpreter::CanInterpretShader(shader)||shader.vertex_bindings().size()!=1||
    shader.vertex_bindings()[0].fetch_constant!=0||shader.vertex_bindings()[0].stride_words!=7||
    shader.writes_point_size_edge_flag_kill_vertex()||shader.memexport_eM_written())return false;
 auto regs=std::make_unique<RegisterFile>();
 std::fill(std::begin(regs->values),std::end(regs->values),0u);
 TraceWriter trace(memory->physical_membase());
 DrawExtentEstimator estimator(*regs,*memory,&trace);
 uint32_t cases=0,invalid=0;
 auto check=[&](uint32_t actual,uint32_t expected,const char* label){
  ++cases;if(actual!=expected){++invalid;if(invalid<=8)REXLOG_ERROR("Clear extent {} case {}: actual {}, expected {}",label,cases,actual,expected);}
 };
 auto prepare=[&](const auto& clear){
  std::fill(std::begin(regs->values),std::end(regs->values),0u);
  for(const auto& [index,value]:clear.registers)regs->values[index]=value;
  std::memcpy(bytes,clear.vertices.data(),clear.vertices.size());
  auto fetch=regs->GetVertexFetch(0);fetch.type=xenos::FetchConstantType::kVertex;fetch.address=physical>>2;fetch.size=21;fetch.endian=xenos::Endian::k8in32;
  std::memcpy(&regs->values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],&fetch,sizeof(fetch));
 };
 // Check the allocator's virtual/physical alias relation before allowing the
 // SDK interpreter to dereference the fetch buffer. Fail closed on mismatch.
 prepare(em2::draw_extent_test_data::clears[0]);
 std::array<uint8_t,84> physical_copy{};SIZE_T read=0;
 if(!ReadProcessMemory(GetCurrentProcess(),physical_bytes,physical_copy.data(),physical_copy.size(),&read)||
    read!=physical_copy.size()||physical_copy!=em2::draw_extent_test_data::clears[0].vertices){
  REXLOG_ERROR("Clear extent private allocation virtual/physical aliases differ");return false;
 }
 REXCVAR_SET(execute_unclipped_draw_vs_on_cpu_with_scissor,false);
 checkpoint("captured snapshots");
 for(const auto& clear:em2::draw_extent_test_data::clears){
  prepare(clear);REXCVAR_SET(execute_unclipped_draw_vs_on_cpu,false);
  check(estimator.EstimateMaxY(true,shader),8192,"captured off");
  REXCVAR_SET(execute_unclipped_draw_vs_on_cpu,true);
  check(estimator.EstimateMaxY(false,shader),8192,"captured no-try");
  check(estimator.EstimateVertexMaxY(shader),clear.max_y,"captured vertices");
  check(estimator.EstimateMaxY(true,shader),clear.max_y,"captured on");
 }
 // Half-integer D3D rectangle corners have an independently known exclusive
 // integer bottom, for every Xenos sample count and many screen-space heights.
 checkpoint("synthetic heights");
 for(uint32_t samples=0;samples<3;++samples)for(uint32_t h=1;h<=8192;h+=17){
  prepare(em2::draw_extent_test_data::clears[0]);
  auto surface=regs->Get<reg::RB_SURFACE_INFO>();surface.msaa_samples=static_cast<xenos::MsaaSamples>(samples);
  regs->values[XE_GPU_REG_RB_SURFACE_INFO]=surface.value;
  const uint32_t word=_byteswap_ulong(std::bit_cast<uint32_t>(float(h)-.5f));
  std::memcpy(bytes+2*28+4,&word,4);
  check(estimator.EstimateVertexMaxY(shader),h,"synthetic height");
  check(estimator.EstimateMaxY(true,shader),h,"synthetic ownership");
 }
 checkpoint("fallback cases");
 prepare(em2::draw_extent_test_data::clears[0]);
 const auto height=em2::draw_extent_test_data::clears[0].max_y;
 regs->values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR]=(400u<<16)|640u;
 regs->values[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR]=(400u<<16)|640u;
 check(estimator.EstimateMaxY(true,shader),400,"bounded scissor keeps fallback");
 REXCVAR_SET(execute_unclipped_draw_vs_on_cpu_with_scissor,true);
 check(estimator.EstimateMaxY(true,shader),height,"opt-in bounded scissor");
 REXCVAR_SET(execute_unclipped_draw_vs_on_cpu_with_scissor,false);
 prepare(em2::draw_extent_test_data::clears[0]);
 auto draw=regs->Get<reg::VGT_DRAW_INITIATOR>();draw.source_select=xenos::SourceSelect::kImmediate;
 regs->values[XE_GPU_REG_VGT_DRAW_INITIATOR]=draw.value;
 check(estimator.EstimateVertexMaxY(shader),8192,"unsupported immediate");
 check(estimator.EstimateMaxY(true,shader),8192,"unsupported immediate ownership");
 draw.source_select=xenos::SourceSelect::kAutoIndex;draw.major_mode=xenos::MajorMode::kExplicit;
 regs->values[XE_GPU_REG_VGT_DRAW_INITIATOR]=draw.value;
 auto path=regs->Get<reg::VGT_OUTPUT_PATH_CNTL>();path.path_select=xenos::VGTOutputPath::kTessellationEnable;
 regs->values[XE_GPU_REG_VGT_OUTPUT_PATH_CNTL]=path.value;
 check(estimator.EstimateVertexMaxY(shader),8192,"unsupported tessellation");
 check(estimator.EstimateMaxY(true,shader),8192,"unsupported tessellation ownership");
 prepare(em2::draw_extent_test_data::clears[0]);
 draw=regs->Get<reg::VGT_DRAW_INITIATOR>();draw.num_indices=0;
 regs->values[XE_GPU_REG_VGT_DRAW_INITIATOR]=draw.value;
 check(estimator.EstimateVertexMaxY(shader),0,"empty draw");
 check(estimator.EstimateMaxY(true,shader),0,"empty ownership");
 // Clipped draws continue to use their viewport, independent of the CPU flag.
 prepare(em2::draw_extent_test_data::clears[0]);
 auto clip=regs->Get<reg::PA_CL_CLIP_CNTL>();clip.clip_disable=false;
 regs->values[XE_GPU_REG_PA_CL_CLIP_CNTL]=clip.value;
 auto vte=regs->Get<reg::PA_CL_VTE_CNTL>();vte.vport_y_scale_ena=true;vte.vport_y_offset_ena=true;
 regs->values[XE_GPU_REG_PA_CL_VTE_CNTL]=vte.value;
 for(bool enabled:{false,true}){REXCVAR_SET(execute_unclipped_draw_vs_on_cpu,enabled);check(estimator.EstimateMaxY(true,shader),256,"clipped viewport");}
 std::filesystem::create_directories(output);
 std::ofstream(output/"clear-draw-extent.json")<<"{\"captured_inputs\":32,\"cases\":"<<cases<<",\"invalid\":"<<invalid<<",\"gpu_work\":false,\"full_coverage_or_copy_discard_claim\":false}";
 REXLOG_INFO("Installed clear draw estimator: {} cases, {} invalid; 32 actual clear inputs plus sample/height and conservative fallback coverage",cases,invalid);
 checkpoint("completed");
 return invalid==0;
}
}
namespace em2 {
bool TestNativeStencilZero(rex::graphics::d3d12::D3D12CommandProcessor& cp,const std::filesystem::path& output){
 if(!TestStencilConsumerSurvey())return false;
 struct Fp{unsigned csr=_mm_getcsr();~Fp(){_mm_setcsr(csr);}}fp;
 if(!TestClearDrawExtent(cp,output/"clear-extent"))return false;
 const bool requested_value=REXCVAR_GET(em2_stencil_plane_copy);
 const bool requested_coalescing=REXCVAR_GET(em2_coalesce_stencil_copies);
 const bool requested_fusion=REXCVAR_GET(em2_fuse_depth_stencil_transfers) && requested_value;
 struct FusionFlag{bool saved=REXCVAR_GET(em2_fuse_depth_stencil_transfers);~FusionFlag(){REXCVAR_SET(em2_fuse_depth_stencil_transfers,saved);}}fusion_flag;
 struct ValueFlag{bool saved;~ValueFlag(){REXCVAR_SET(em2_stencil_plane_copy,saved);}}value_flag{requested_value};
 struct ClearFlag{bool saved=REXCVAR_GET(em2_skip_stencil_scratch_clear);~ClearFlag(){REXCVAR_SET(em2_skip_stencil_scratch_clear,saved);}}clear_flag;
 struct PlaneFlag{bool saved=REXCVAR_GET(em2_stencil_plane_barriers);~PlaneFlag(){REXCVAR_SET(em2_stencil_plane_barriers,saved);}}plane_flag;
 struct Flag{bool saved=REXCVAR_GET(em2_skip_proven_zero_stencil);~Flag(){REXCVAR_SET(em2_skip_proven_zero_stencil,saved);}}flag;
 const auto& provider=cp.GetD3D12Provider();auto* device=provider.GetDevice();auto* queue=provider.GetDirectQueue();
 if(!TestBoundedTransferDivision(provider,output/"tile-division"))return false;
 ComPtr<ID3D12InfoQueue> validation;device->QueryInterface(IID_PPV_ARGS(&validation));const auto first=validation?validation->GetNumStoredMessagesAllowedByRetrievalFilter():0;
 if(!TestDirectColorResolve(cp,output/"direct-color-resolve"))return false;
 const auto dll=LoadLibraryW(L"d3dcompiler_47.dll");if(!dll)return false;struct Library{HMODULE h;~Library(){FreeLibrary(h);}}library{dll};auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(dll,"D3DCompile"));if(!compile)return false;
 const char shader[]=R"(
#if MSAA
Texture2DMS<uint> stencil:register(t0);Texture2DMS<float> depth:register(t1);
#else
Texture2D<uint> stencil:register(t0);Texture2D<float> depth:register(t1);
#endif
RWByteAddressBuffer values:register(u0);cbuffer C:register(b0){uint width,height,samples,pad;};
[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){if(any(p.xy>=uint2(width,height)))return;for(uint s=0;s<samples;++s){
#if MSAA
uint v=stencil.Load(p.xy,s);float z=depth.Load(p.xy,s);
#else
uint v=stencil.Load(int3(p.xy,0));float z=depth.Load(int3(p.xy,0));
#endif
values.Store2(((p.y*width+p.x)*samples+s)*8,uint2(v,asuint(z)));}})";
 ComPtr<ID3DBlob> code[2],errors;
 for(unsigned m=0;m<2;++m){D3D_SHADER_MACRO macros[]={{"MSAA",m?"1":"0"},{nullptr,nullptr}};if(FAILED(compile(shader,sizeof(shader)-1,"native-stencil-oracle",macros,nullptr,"main","cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code[m],&errors)))return false;}
 uint64_t invalid=0,checked=0,skipped_total=0,value_meshes=0,omitted_clears=0,plane_transfers=0,copies_saved=0,fused_meshes=0,r8_off_fallback_cases=0;unsigned cases=0;
 for(unsigned scale:{1u,2u})for(unsigned samples:{1u,4u})for(unsigned source_samples:{1u,4u})for(unsigned format=0;format<2;++format)for(unsigned color_source=0;color_source<(requested_fusion?3u:2u);++color_source){
  auto f=std::make_unique<Owned>();auto* memory=cp.*Member(Memory{});if(!memory || !f->event)return false;
  if(FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f->allocator))) || FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f->allocator.Get(),nullptr,IID_PPV_ARGS(&f->list))) || FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f->fence))))return false;
  f->registers=std::make_unique<rex::graphics::RegisterFile>();f->trace=std::make_unique<rex::graphics::TraceWriter>(memory->physical_membase());
  f->cache=std::make_unique<Cache>(*f->registers,*memory,*f->trace,scale,scale,cp,false);if(!f->cache->Initialize())return false;
  f->cache.get()->*Member(StencilOutput{})=false;
  Key src;src.base_tiles=3;src.pitch_tiles_at_32bpp=2;src.is_depth=!color_source;src.resource_format=color_source?(color_source==2?0:(format?uint32_t(rex::graphics::xenos::ColorRenderTargetFormat::k_32_FLOAT):0)):format;src.msaa_samples=source_samples==4?rex::graphics::xenos::MsaaSamples::k4X:rex::graphics::xenos::MsaaSamples::k1X;
  Key dst=src;dst.msaa_samples=samples==4?rex::graphics::xenos::MsaaSamples::k4X:rex::graphics::xenos::MsaaSamples::k1X;dst.is_depth=1;dst.base_tiles=2;dst.pitch_tiles_at_32bpp=1;dst.resource_format=1-format;
  f->source.reset(static_cast<Host*>((f->cache.get()->*Member(Create{}))(src)));f->destination.reset(static_cast<Host*>((f->cache.get()->*Member(Create{}))(dst)));if(!f->source || !f->destination)return false;
  const unsigned source_tile_width=80/(source_samples==4?2:1),source_tile_height=16/(source_samples==4?2:1);
  const unsigned tile_width=80/(samples==4?2:1),tile_height=16/(samples==4?2:1),width=tile_width*scale,height=4*tile_height*scale;const uint64_t bytes=uint64_t(width)*height*samples*8;
  D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,3,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&f->views))))return false;
  D3D12_HEAP_PROPERTIES props{};props.Type=D3D12_HEAP_TYPE_DEFAULT;props.CreationNodeMask=props.VisibleNodeMask=1;
  D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;buffer.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if(FAILED(device->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&f->values))) || !Buffer(device,bytes,D3D12_HEAP_TYPE_READBACK,&f->readback))return false;
  D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};D3D12_ROOT_PARAMETER params[3]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[0].Constants={0,0,4};for(unsigned i=0;i<2;++i){params[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[i+1].DescriptorTable={1,&ranges[i]};}
  ComPtr<ID3DBlob> serialized;D3D12_ROOT_SIGNATURE_DESC root{3,params};if(FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors)) || FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&f->root))))return false;
  D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=f->root.Get();pipeline.CS={code[samples==4]->GetBufferPointer(),code[samples==4]->GetBufferSize()};if(FAILED(device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&f->pipelines[0]))))return false;
  D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=f->destination->resource()->GetDesc().Format==DXGI_FORMAT_R24G8_TYPELESS?DXGI_FORMAT_X24_TYPELESS_G8_UINT:DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;view.Shader4ComponentMapping=D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(1,1,1,1);view.ViewDimension=samples==4?D3D12_SRV_DIMENSION_TEXTURE2DMS:D3D12_SRV_DIMENSION_TEXTURE2D;if(samples==1){view.Texture2D.MipLevels=1;view.Texture2D.PlaneSlice=1;}
  auto cpu=f->views->GetCPUDescriptorHandleForHeapStart();device->CreateShaderResourceView(f->destination->resource(),&view,cpu);const auto step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);cpu.ptr+=step;view.Format=f->destination->resource()->GetDesc().Format==DXGI_FORMAT_R24G8_TYPELESS?DXGI_FORMAT_R24_UNORM_X8_TYPELESS:DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;if(samples==1)view.Texture2D.PlaneSlice=0;device->CreateShaderResourceView(f->destination->resource(),&view,cpu);cpu.ptr+=step;D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R32_TYPELESS;uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;uav.Buffer.NumElements=UINT(bytes/4);uav.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;device->CreateUnorderedAccessView(f->values.Get(),nullptr,&uav,cpu);
  uint64_t serial=0;std::vector<uint32_t> reference;
  for(unsigned scenario=0;scenario<6;++scenario)for(unsigned optimized=0;optimized<2;++optimized)for(unsigned value_path=0;value_path<(requested_value?(requested_fusion?10u:5u):1u);++value_path){
   // 0: original bits. 1..4: separate R8 with clear/barrier combinations.
   // 5..8: fused draw with the same four combinations. 9: fusion requested
   // while R8 is toggled OFF, requiring the complete original fallback.
   const bool use_value=requested_value && value_path!=0 && value_path!=9;
   REXCVAR_SET(em2_stencil_plane_copy,use_value);
   REXCVAR_SET(em2_fuse_depth_stencil_transfers,requested_fusion && value_path>=5);
   const bool omit_clear=value_path==2 || value_path==4 || value_path==6 || value_path==8;
   const bool plane_only=value_path==3 || value_path==4 || value_path==7 || value_path==8;
   REXCVAR_SET(em2_skip_stencil_scratch_clear,omit_clear);
   REXCVAR_SET(em2_stencil_plane_barriers,plane_only);
   REXCVAR_SET(em2_skip_proven_zero_stencil,bool(optimized));
   cp.*Member(External{})=nullptr;cp.*Member(Guest{})=nullptr;cp.*Member(Root{})=nullptr;cp.*Member(Topology{})=D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;cp.*Member(Viewport{})=true;cp.*Member(Scissor{})=true;cp.*Member(StencilDirty{})=true;
   auto clear=[&](Host* target,uint8_t stencil,Transfer::Rectangle rect){Target* targets[]={target};const std::vector<Transfer> none[1];const uint64_t value=0x80000000ull|stencil;(f->cache.get()->*Member(Clear{}))(1,targets,none,&value,&rect);};
   // Destination and source receive identical guest depth bits but different
   // stencil. A fused NOT_EQUAL depth test would suppress the R8 result and
   // fail this check, despite appearing to work when depth changes.
   clear(f->destination.get(),0xA5,{0,0,tile_width,4*tile_height});
   // Complementary patterns exercise every source stencil bit. A low-only
   // value such as 7 cannot detect accidental truncation of the upper bits.
   const uint8_t source_stencil=scenario==1?0xD3:scenario==2?0x2C:scenario==3?0x80:scenario==5?0xFF:0;
   clear(f->source.get(),source_stencil,{0,0,2*source_tile_width,source_tile_height});
   if(scenario==2)clear(f->source.get(),0,{0,0,source_tile_width,source_tile_height});
   if(scenario==3 || scenario==4){REXCVAR_SET(em2_skip_proven_zero_stencil,false);clear(f->source.get(),source_stencil,{0,0,2*source_tile_width,source_tile_height});REXCVAR_SET(em2_skip_proven_zero_stencil,bool(optimized));}
   if(omit_clear){
    // The previous variant wrote the same source values. Poisoning the entire
    // scratch resource prevents retained contents from masking raster holes.
    auto scratch=AcquireStencilTransferScratch(f->cache.get(),cp,f->destination->resource());if(!scratch)return false;
    cp.SubmitBarriers();const float poison[4]={150.f,0,0,0};
    cp.GetDeferredCommandList().D3DClearRenderTargetView(scratch->rtv,poison,0,nullptr);
   }
   Target* targets[]={f->destination.get()};const std::vector<Transfer> copies[1]={{Transfer(3,4,f->source.get(),nullptr),Transfer(4,5,f->source.get(),nullptr)}};const auto before=StencilZeroSkippedDraws(),value_before=StencilValueTransferMeshes(),clear_before=StencilScratchClearOmissions(),plane_before=StencilPlaneBarrierTransfers(),saved_before=StencilCopyCommandsSaved(),fused_before=FusedDepthStencilTransferMeshes();
   // Resolve-clear cutouts must survive the transfer geometry and plane
   // copies; the native clear writes a different stencil/depth value afterward.
   Transfer::Rectangle cutout{2,tile_height+1,tile_width-4,tile_height-2};const uint64_t cutout_value=0xA000000Bull;
   (f->cache.get()->*Member(Clear{}))(1,targets,copies,scenario==5?&cutout_value:nullptr,scenario==5?&cutout:nullptr);const auto skipped=StencilZeroSkippedDraws()-before;
   if(skipped!=(optimized && scenario==0 && !color_source?8:0)){REXLOG_ERROR("Native stencil proof scenario{} scale{} samples{} format{} optimized{}: skipped {}",scenario,scale,samples,format,optimized,skipped);return false;}skipped_total+=skipped;
   const auto value_count=StencilValueTransferMeshes()-value_before;
   if(value_count!=(use_value && !skipped?1:0)){REXLOG_ERROR("Native stencil value path expected a real transfer; got {}",value_count);return false;}value_meshes+=value_count;
   const auto fused_count=FusedDepthStencilTransferMeshes()-fused_before;
   const auto expected_fused=uint64_t(requested_fusion && value_path>=5 && use_value && color_source && src.resource_format==0);
   if(fused_count!=expected_fused){REXLOG_ERROR("Native fused depth/stencil path expected {} combined draws; got {} (source class{} format{} destination{})",expected_fused,fused_count,color_source,uint32_t(src.resource_format),uint32_t(dst.resource_format));return false;}fused_meshes+=fused_count;
   if(value_path==9)++r8_off_fallback_cases;
   const auto clear_count=StencilScratchClearOmissions()-clear_before;
   if(clear_count!=(omit_clear?value_count:0)){REXLOG_ERROR("Native stencil scratch clear path expected {} omissions; got {}",omit_clear?value_count:0,clear_count);return false;}omitted_clears+=clear_count;
   const auto plane_count=StencilPlaneBarrierTransfers()-plane_before;
   if(plane_count!=(plane_only?value_count:0)){REXLOG_ERROR("Native stencil plane barrier path expected {} transfers; got {}",plane_only?value_count:0,plane_count);return false;}plane_transfers+=plane_count;
   const auto saved_count=StencilCopyCommandsSaved()-saved_before;copies_saved+=saved_count;
   if((requested_coalescing && value_count && !saved_count) || (!requested_coalescing && saved_count)){REXLOG_ERROR("Native stencil copy coalescing path did not match requested mode");return false;}
   cp.PushTransitionBarrier(f->destination->resource(),f->destination->SetResourceState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);cp.SubmitBarriers();
   auto* native_heap=(cp.*Member(Bindless{}))?(cp.*Member(BindlessHeap{})):(cp.*Member(BindfulHeap{}));if(!native_heap)return false;f->list->SetDescriptorHeaps(1,&native_heap);cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);cp.GetDeferredCommandList().Reset();
   auto* read_heap=f->views.Get();f->list->SetDescriptorHeaps(1,&read_heap);f->list->SetComputeRootSignature(f->root.Get());f->list->SetPipelineState(f->pipelines[0].Get());const unsigned dimensions[]={width,height,samples,0};f->list->SetComputeRoot32BitConstants(0,4,dimensions,0);auto gpu=f->views->GetGPUDescriptorHandleForHeapStart();f->list->SetComputeRootDescriptorTable(1,gpu);gpu.ptr+=2*step;f->list->SetComputeRootDescriptorTable(2,gpu);f->list->Dispatch((width+7)/8,(height+7)/8,1);
   Barrier(f->list.Get(),f->values.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);f->list->CopyBufferRegion(f->readback.Get(),0,f->values.Get(),0,bytes);Barrier(f->list.Get(),f->values.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
   if(FAILED(f->list->Close()))return false;ID3D12CommandList* lists[]={f->list.Get()};queue->ExecuteCommandLists(1,lists);++serial;if(FAILED(queue->Signal(f->fence.Get(),serial)) || FAILED(f->fence->SetEventOnCompletion(serial,f->event)) || WaitForSingleObject(f->event,15000)!=WAIT_OBJECT_0){f.release();return false;}
   void* mapped=nullptr;D3D12_RANGE range{0,SIZE_T(bytes)},empty{0,0};if(FAILED(f->readback->Map(0,&range,&mapped)))return false;const auto* values=static_cast<const uint32_t*>(mapped);
   const auto previous_invalid=invalid;
   for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)for(unsigned sample=0;sample<samples;++sample){const auto tile=2+y/(tile_height*scale);const bool in_cutout=scenario==5 && x>=cutout.x_pixels*scale && x<(cutout.x_pixels+cutout.width_pixels)*scale && y>=cutout.y_pixels*scale && y<(cutout.y_pixels+cutout.height_pixels)*scale;const uint32_t expected=in_cutout?11:tile==3 || tile==4?(scenario==2 && tile==3?0:source_stencil):0xA5;const auto index=((y*width+x)*samples+sample)*2;const auto actual=values[index];if(actual!=expected && invalid==previous_invalid)REXLOG_ERROR("Native stencil mismatch scale{} samples{} format{} sourceClass{} scenario{} optimized{} valuePath{} at {},{},{}: actual {} expected {}",scale,samples,format,color_source,scenario,optimized,value_path,x,y,sample,actual,expected);invalid+=actual!=expected;if(value_path!=0){invalid+=values[index]!=reference[index] || values[index+1]!=reference[index+1];}++checked;}
   if(value_path==0)reference.assign(values,values+bytes/4);f->readback->Unmap(0,&empty);++cases;if(FAILED(f->allocator->Reset()) || FAILED(f->list->Reset(f->allocator.Get(),nullptr)))return false;
  }
  f->list->Close();
 }
 uint64_t device_errors=0;if(validation)for(uint64_t i=first;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;validation->GetMessage(i,nullptr,&size);std::vector<uint8_t> data(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());if(SUCCEEDED(validation->GetMessage(i,message,&size)) && message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++device_errors;REXLOG_ERROR("Native stencil GPU: {}",message->pDescription);}}
 std::filesystem::create_directories(output);std::ofstream report(output/"native-stencil-zero.json");report<<"{\"cases\":"<<cases<<",\"sample_values\":"<<checked<<",\"invalid\":"<<invalid<<",\"device_errors\":"<<device_errors<<",\"skipped_bit_draws\":"<<skipped_total<<",\"native_value_meshes\":"<<value_meshes<<",\"scratch_clear_omissions\":"<<omitted_clears<<",\"stencil_plane_transfers\":"<<plane_transfers<<",\"copy_commands_saved\":"<<copies_saved<<",\"fused_depth_stencil_meshes\":"<<fused_meshes<<",\"r8_off_fallback_cases\":"<<r8_off_fallback_cases<<",\"mixed_source_batch_tested\":false,\"host_depth_source_fallback_tested\":false,\"scope\":\"Actual native clears and transfer shaders, depth and RGBA8/raw32 color sources; source/destination base, pitch and depth format differ; source and destination 1x/4x MSAA combinations, 1x/2x scaling; zero, nonzero, partial and live off/on proofs, simultaneous resolve-clear cutouts, untouched destination coverage; R8 modes compare exact stencil and depth bits against original eight-pass output, including pre-clear omission with poisoned scratch resources and stencil-only barrier combinations; optional fused MRT+DSV draws checked against original separate draws with unchanged depth, changed stencil, both depth formats, all four fused scratch-clear/plane-barrier combinations (omitted clears poison scratch), R8-off fallback and unsupported-source fallbacks; all batches are single-source, mixed-source batches and host-depth-source provenance are not exercised\"}\n";
 REXLOG_INFO("Native stencil zero: {} cases, {} sample values, {} invalid, {} device errors, {} bit draws skipped",cases,checked,invalid,device_errors,skipped_total);
 report.flush();
 REXLOG_INFO("Native stencil scratch clear omission: {} poisoned-scratch transfers checked",omitted_clears);
 REXLOG_INFO("Native stencil plane barriers: {} plane-only transfers checked",plane_transfers);
 REXLOG_INFO("Native stencil copy coalescing: {} redundant GPU copy commands eliminated",copies_saved);
 REXLOG_INFO("Native fused depth/stencil: {} combined draws checked against original stencil and exact depth bits",fused_meshes);
 return !invalid && !device_errors && skipped_total==(requested_value?(requested_fusion?1280:640):128) && value_meshes==(requested_value?(requested_fusion?4480:1472):0) && omitted_clears==(requested_value?(requested_fusion?2240:736):0) && plane_transfers==(requested_value?(requested_fusion?2240:736):0) && fused_meshes==(requested_fusion?1152:0) && r8_off_fallback_cases==(requested_fusion?576:0) && (!requested_coalescing || copies_saved!=0) && report.good();
}
}

#include "hdr_shader_export.h"
#include "scene_shader_audit.h"
#include "hdr_scene_target.h"
#include "hdr_scene_cache.h"
#include "material_capture.h"
#include "scene_depth_view.h"
#include "scene_geometry.h"
#include "scene_occlusion.h"
#include "scene_raster_pass.h"
#include "hdr_scene_resolve.h"
#include "hdr_output.h"
#include <rex/graphics/d3d12/shared_memory.h>
#include <rex/graphics/d3d12/texture_cache.h>
#include <rex/graphics/d3d12/render_target_cache.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/trace_writer.h>
#include <type_traits>
#include "native_pixel_fixture.h"
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/deferred_command_list.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/logging.h>
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <bit>
#include <cstring>
#include <cmath>
#include <vector>
#include <fstream>
#include <immintrin.h>
using namespace em2::pixel_test;
namespace em2 {
std::shared_ptr<void> RecordSceneAmbientPublicationTest(rex::graphics::d3d12::D3D12CommandProcessor&,rex::graphics::d3d12::D3D12RenderTargetCache&,const std::shared_ptr<const HdrResolvedScene>&,const std::shared_ptr<const HdrResolvedScene>&,rex::graphics::d3d12::D3D12TextureCache&,ID3D12Resource*,ID3D12GraphicsCommandList*);
bool VerifySceneAmbientPublicationTest(const std::shared_ptr<void>&,const std::filesystem::path&);
}
namespace {
constexpr unsigned side=8;constexpr uint64_t stride=256*side;
float Half(uint16_t bits){const unsigned e=(bits>>10)&31,m=bits&1023;return (bits&32768?-1.f:1.f)*(e?std::ldexp(float(1024+m),int(e)-25):std::ldexp(float(m),-24));}
using NativeCache=rex::graphics::d3d12::D3D12RenderTargetCache;
using NativeBase=rex::graphics::RenderTargetCache;
using CP=rex::graphics::d3d12::D3D12CommandProcessor;
using Deferred=rex::graphics::d3d12::DeferredCommandList;
template<class Tag,auto Member>struct Access{friend auto MemberOf(Tag){return Member;}};
template<class Tag,class T>struct TypeAccess{friend auto TypeOf(Tag){return std::type_identity<T>{};}};
struct HostTag{friend auto TypeOf(HostTag);};template struct TypeAccess<HostTag,NativeCache::D3D12RenderTarget>;
struct KeyTag{friend auto TypeOf(KeyTag);};template struct TypeAccess<KeyTag,NativeBase::RenderTargetKey>;
struct BaseTargetTag{friend auto TypeOf(BaseTargetTag);};template struct TypeAccess<BaseTargetTag,NativeBase::RenderTarget>;
struct BindTag{friend auto MemberOf(BindTag);};template struct Access<BindTag,&NativeCache::SetCommandListRenderTargets>;
struct MemoryTag{friend auto MemberOf(MemoryTag);};template struct Access<MemoryTag,&rex::graphics::CommandProcessor::memory_>;
struct SharedTag{friend auto MemberOf(SharedTag);};template struct Access<SharedTag,&CP::shared_memory_>;
struct TextureTag{friend auto MemberOf(TextureTag);};template struct Access<TextureTag,&CP::texture_cache_>;
struct TargetsTag{friend auto MemberOf(TargetsTag);};template struct Access<TargetsTag,&NativeBase::render_targets_>;
struct OwnTag{friend auto MemberOf(OwnTag);};template struct Access<OwnTag,&NativeBase::ChangeOwnership>;
struct StreamTag{friend auto MemberOf(StreamTag);};template struct Access<StreamTag,&Deferred::command_stream_>;
struct GammaTag{friend auto MemberOf(GammaTag);};template struct Access<GammaTag,&NativeBase::last_update_accumulated_color_targets_are_gamma_>;
struct TransferTag{friend auto TypeOf(TransferTag);};template struct TypeAccess<TransferTag,NativeBase::Transfer>;
struct ClearTag{friend auto MemberOf(ClearTag);};template struct Access<ClearTag,&NativeCache::PerformTransfersAndResolveClears>;
struct BarriersTag{friend auto MemberOf(BarriersTag);};template struct Access<BarriersTag,&CP::barriers_>;
using Host=typename decltype(TypeOf(HostTag{}))::type;
using Key=typename decltype(TypeOf(KeyTag{}))::type;
using BaseTarget=typename decltype(TypeOf(BaseTargetTag{}))::type;
using Transfer=typename decltype(TypeOf(TransferTag{}))::type;
struct TransferKeyTag{friend auto TypeOf(TransferKeyTag);};template struct TypeAccess<TransferKeyTag,NativeCache::TransferShaderKey>;
struct AddressTag{friend auto TypeOf(AddressTag);};template struct TypeAccess<AddressTag,NativeCache::TransferAddressConstant>;
struct TransferPipelineTag{friend auto MemberOf(TransferPipelineTag);};template struct Access<TransferPipelineTag,&NativeCache::GetOrCreateTransferPipelines>;
struct TransferRootTag{friend auto MemberOf(TransferRootTag);};template struct Access<TransferRootTag,&NativeCache::transfer_root_signatures_>;
struct TransferModeTag{friend auto MemberOf(TransferModeTag);};template struct Access<TransferModeTag,NativeCache::TransferMode::kColorToColor>;
using TransferKey=typename decltype(TypeOf(TransferKeyTag{}))::type;
using Address=typename decltype(TypeOf(AddressTag{}))::type;
struct BeginTag{friend auto MemberOf(BeginTag);};template struct Access<BeginTag,&CP::BeginSubmission>;
struct ff_viewport_Tag{friend auto MemberOf(ff_viewport_Tag);};template struct Access<ff_viewport_Tag,&CP::ff_viewport_>;
struct ff_scissor_Tag{friend auto MemberOf(ff_scissor_Tag);};template struct Access<ff_scissor_Tag,&CP::ff_scissor_>;
struct ff_viewport_update_needed_Tag{friend auto MemberOf(ff_viewport_update_needed_Tag);};template struct Access<ff_viewport_update_needed_Tag,&CP::ff_viewport_update_needed_>;
struct ff_scissor_update_needed_Tag{friend auto MemberOf(ff_scissor_update_needed_Tag);};template struct Access<ff_scissor_update_needed_Tag,&CP::ff_scissor_update_needed_>;
struct current_guest_pipeline_Tag{friend auto MemberOf(current_guest_pipeline_Tag);};template struct Access<current_guest_pipeline_Tag,&CP::current_guest_pipeline_>;
struct current_external_pipeline_Tag{friend auto MemberOf(current_external_pipeline_Tag);};template struct Access<current_external_pipeline_Tag,&CP::current_external_pipeline_>;
struct current_graphics_root_signature_Tag{friend auto MemberOf(current_graphics_root_signature_Tag);};template struct Access<current_graphics_root_signature_Tag,&CP::current_graphics_root_signature_>;
struct current_graphics_root_bindful_extras_Tag{friend auto MemberOf(current_graphics_root_bindful_extras_Tag);};template struct Access<current_graphics_root_bindful_extras_Tag,&CP::current_graphics_root_bindful_extras_>;
struct current_graphics_root_up_to_date_Tag{friend auto MemberOf(current_graphics_root_up_to_date_Tag);};template struct Access<current_graphics_root_up_to_date_Tag,&CP::current_graphics_root_up_to_date_>;
struct primitive_topology_Tag{friend auto MemberOf(primitive_topology_Tag);};template struct Access<primitive_topology_Tag,&CP::primitive_topology_>;
struct view_bindful_heap_current_Tag{friend auto MemberOf(view_bindful_heap_current_Tag);};template struct Access<view_bindful_heap_current_Tag,&CP::view_bindful_heap_current_>;
struct sampler_bindful_heap_current_Tag{friend auto MemberOf(sampler_bindful_heap_current_Tag);};template struct Access<sampler_bindful_heap_current_Tag,&CP::sampler_bindful_heap_current_>;
struct draw_view_bindful_heap_index_Tag{friend auto MemberOf(draw_view_bindful_heap_index_Tag);};template struct Access<draw_view_bindful_heap_index_Tag,&CP::draw_view_bindful_heap_index_>;
struct draw_sampler_bindful_heap_index_Tag{friend auto MemberOf(draw_sampler_bindful_heap_index_Tag);};template struct Access<draw_sampler_bindful_heap_index_Tag,&CP::draw_sampler_bindful_heap_index_>;
struct bindless_resources_used_Tag{friend auto MemberOf(bindless_resources_used_Tag);};template struct Access<bindless_resources_used_Tag,&CP::bindless_resources_used_>;
struct view_bindless_heap_Tag{friend auto MemberOf(view_bindless_heap_Tag);};template struct Access<view_bindless_heap_Tag,&CP::view_bindless_heap_>;
template<class Tag>struct SaveField{
 CP& cp;using Value=std::remove_cvref_t<decltype(std::declval<CP&>().*MemberOf(Tag{}))>;
 std::array<unsigned char,sizeof(Value)> bytes;
 explicit SaveField(CP& p):cp(p){static_assert(std::is_trivially_copyable_v<Value>);std::memcpy(bytes.data(),&(cp.*MemberOf(Tag{})),sizeof(Value));}
 ~SaveField(){std::memcpy(&(cp.*MemberOf(Tag{})),bytes.data(),sizeof(Value));}
};


struct CaptureFixture:Fixture{
 std::shared_ptr<void> lighting_publication,gamma_lighting_publication;
 std::array<rex::graphics::draw_util::ResolveInfo,6> guest_gamma_info{};
 std::array<em2::HdrGuestTransfer,6> guest_gamma_transfer{};
 std::unique_ptr<rex::graphics::RegisterFile> registers;
 std::unique_ptr<rex::graphics::TraceWriter> trace;
 std::unique_ptr<NativeCache> native;
 std::unique_ptr<em2::HdrSceneCacheRegistration> registration;
 std::array<std::unique_ptr<Host>,2> foreign;
 std::array<std::shared_ptr<em2::HdrSceneTarget>,2> captures;
 std::array<ComPtr<ID3D12PipelineState>,2> transfer_pipelines;
 std::array<std::unique_ptr<Host>,2> ownership_dest;
 std::array<std::shared_ptr<em2::HdrSceneTarget>,2> ownership_hdr,material_sources,material_destinations;
 ComPtr<ID3D12Resource> material_resolved;
 std::array<rex::graphics::draw_util::ResolveInfo,4> resolved_info{};
 std::array<rex::graphics::draw_util::ResolveInfo,7> sample_info{};
 std::array<ComPtr<ID3D12PipelineState>,4> sample_pipelines,surface_sample_pipelines;
 std::array<std::unique_ptr<rex::graphics::d3d12::D3D12TextureCache>,2> sampling_caches;
 std::array<ComPtr<ID3D12RootSignature>,2> sampling_roots;
 std::array<ComPtr<ID3D12PipelineState>,2> sampling_pipelines;
 ComPtr<ID3D12Resource> sampling_output;
 em2::HdrOutputPass swap_output;
 std::unique_ptr<em2::HdrOutputBindings> swap_bindings;
 unsigned swap_queries=0,swap_guards=0;
 std::array<rex::graphics::d3d12::D3D12TextureCache::TextureSRVKey,2> sampling_keys;
 rex::graphics::draw_util::ResolveInfo sampling_info{};
 unsigned sampling_modes=0;
 std::vector<std::shared_ptr<const em2::SceneDepthView>> depth_views;
 em2::SceneGeometryPass geometry_pass;
 em2::SceneOcclusionPass occlusion_pass;
 std::vector<std::shared_ptr<em2::SceneOcclusionBindings>> occlusion_bindings;
 std::vector<std::shared_ptr<em2::SceneGeometryBindings>> geometry_bindings;
 unsigned geometry_allocations=0,geometry_reuses=0;

 ~CaptureFixture(){if(native)for(auto& target:foreign)if(target)(native.get()->*MemberOf(TargetsTag{})).erase(target->key());}
};
bool Texture(ID3D12Device* device,DXGI_FORMAT format,ID3D12Resource** out,unsigned samples=1,unsigned w=side,unsigned h=side,bool uav=false){
 D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
 D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=w;desc.Height=h;
 desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=samples;desc.Format=format;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|(uav?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE);
 return SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(out)));
}
}
bool em2::TestHdrSceneExport(rex::graphics::d3d12::D3D12CommandProcessor& cp,const std::filesystem::path& directory){
 auto& provider=cp.GetD3D12Provider();
 if(!(cp.*MemberOf(BeginTag{}))(false))return false;
 SaveField<ff_viewport_Tag> save_ff_viewport_(cp);
 SaveField<ff_scissor_Tag> save_ff_scissor_(cp);
 SaveField<ff_viewport_update_needed_Tag> save_ff_viewport_update_needed_(cp);
 SaveField<ff_scissor_update_needed_Tag> save_ff_scissor_update_needed_(cp);
 SaveField<current_guest_pipeline_Tag> save_current_guest_pipeline_(cp);
 SaveField<current_external_pipeline_Tag> save_current_external_pipeline_(cp);
 SaveField<current_graphics_root_signature_Tag> save_current_graphics_root_signature_(cp);
 SaveField<current_graphics_root_bindful_extras_Tag> save_current_graphics_root_bindful_extras_(cp);
 SaveField<current_graphics_root_up_to_date_Tag> save_current_graphics_root_up_to_date_(cp);
 SaveField<primitive_topology_Tag> save_primitive_topology_(cp);
 SaveField<view_bindful_heap_current_Tag> save_view_bindful_heap_current_(cp);
 SaveField<sampler_bindful_heap_current_Tag> save_sampler_bindful_heap_current_(cp);
 SaveField<draw_view_bindful_heap_index_Tag> save_draw_view_bindful_heap_index_(cp);
 SaveField<draw_sampler_bindful_heap_index_Tag> save_draw_sampler_bindful_heap_index_(cp);

 // Isolate the native deferred stream without discarding or submitting the
 // caller's pending commands. SetCommandListRenderTargets sees RT->RT states,
 // so this fixture adds no pending transition barriers to the caller.
 auto& stream=cp.GetDeferredCommandList().*MemberOf(StreamTag{});
 struct RestoreStream{std::vector<uintmax_t>& stream;std::vector<uintmax_t> saved;RestoreStream(std::vector<uintmax_t>& s):stream(s){saved.swap(s);}~RestoreStream(){stream.swap(saved);}}restore_stream(stream);
 auto& barriers=cp.*MemberOf(BarriersTag{});
 struct RestoreBarriers{std::vector<D3D12_RESOURCE_BARRIER>& barriers;std::vector<D3D12_RESOURCE_BARRIER> saved;RestoreBarriers(std::vector<D3D12_RESOURCE_BARRIER>& b):barriers(b){saved.swap(b);}~RestoreBarriers(){barriers.swap(saved);}}restore_barriers(barriers);
 std::filesystem::create_directories(directory);
 const char* stage="initialization";bool complete=false;
 std::ofstream steps(directory/"native-bind-progress.txt");auto set_stage=[&](const char* name){stage=name;steps<<name<<std::endl;};set_stage(stage);
 struct Progress{const char*& stage;bool& complete;~Progress(){if(!complete)REXLOG_ERROR("HDR scene export probe failed at {}",stage);}}progress{stage,complete};
 const auto csr=_mm_getcsr();struct FP{unsigned v;~FP(){_mm_setcsr(v);}}fp{csr};
 auto f=std::make_unique<CaptureFixture>();auto* device=provider.GetDevice();auto* queue=provider.GetDirectQueue();
 ComPtr<ID3D12InfoQueue> validation;device->QueryInterface(IID_PPV_ARGS(&validation));
 const auto first=validation?validation->GetNumStoredMessagesAllowedByRetrievalFilter():0;
 if(!f->event || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f->allocator))) ||
 FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f->allocator.Get(),nullptr,IID_PPV_ARGS(&f->list))) ||
 FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f->fence))))return false;
 const auto compiler=LoadLibraryW(L"d3dcompiler_47.dll");if(!compiler)return false;
 struct Library{HMODULE h;~Library(){FreeLibrary(h);}}library{compiler};
 const auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler,"D3DCompile"));if(!compile)return false;
 const char vs_source[]=R"(struct Output{float4 t[16]:TEXCOORD0;float4 position:SV_Position;};
Output main(uint id:SV_VertexID){Output o;float2 uv=float2((id<<1)&2,id&2);
o.position=float4(uv*float2(2,-2)+float2(-1,1),.5,1);[unroll]for(uint i=0;i<16;++i)o.t[i]=float4(uv,0,1);return o;})";
 ComPtr<ID3DBlob> vs,errors,serialized;
 if(FAILED(compile(vs_source,sizeof(vs_source)-1,"hdr-export-fixture",nullptr,nullptr,"main","vs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&errors)))return false;
 std::array<D3D12_ROOT_PARAMETER,4> parameters{};
 for(unsigned i=0;i<4;++i){parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameters[i].Descriptor.ShaderRegister=i;}
 const D3D12_ROOT_SIGNATURE_DESC root{4,parameters.data(),0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
 if(FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors)) ||
 FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&f->root))))return false;
 // Native guest ALLOC/EXEC_END/MAX c0,c0 to color0 export, translated by the
 // same DXBC translator as actual material programs. No HLSL stand-in PS.
 constexpr std::array<uint32_t,6> pixel={0,0x1001C400,0x20000000,0x000F8000,0,0x02000000};
 rex::graphics::DxbcShader shader(rex::graphics::xenos::ShaderType::kPixel,1,pixel.data(),pixel.size(),std::endian::native);
 rex::string::StringBuffer text;shader.AnalyzeUcode(text);
 rex::graphics::DxbcShaderTranslator translator(provider.GetAdapterVendorID(),false,false,false,true,1,1);
 auto* translation=shader.GetOrCreateTranslation(translator.GetDefaultPixelShaderModification(0));
 if(!translator.TranslateAnalyzedShader(*translation) || !translation->is_valid())return false;
 const auto& ps=translation->translated_binary();
 std::ofstream original_file(directory/"native-source.dxbc",std::ios::binary);original_file.write(reinterpret_cast<const char*>(ps.data()),ps.size());original_file.close();
 set_stage("export transformation");const auto hdr=AddHdrSceneExport(ps);
 if(hdr.empty() || AddHdrSceneExport(hdr).size() || AddHdrSceneExport({}).size() ||
 AddHdrSceneExport(std::span(ps).first(35)).size() || AddHdrSceneExport(std::span(ps).first(ps.size()-1)).size())return false;
 set_stage("resources");
 if(!Texture(device,DXGI_FORMAT_R8G8B8A8_UNORM,&f->target,1,96,32) || !Texture(device,DXGI_FORMAT_R8G8B8A8_UNORM,&f->textures[1],4,96,32) ||
 !Buffer(device,8192,D3D12_HEAP_TYPE_UPLOAD,&f->upload) || !Buffer(device,130*stride,D3D12_HEAP_TYPE_READBACK,&f->readback) || !Texture(device,DXGI_FORMAT_R16G16B16A16_FLOAT,&f->material_resolved))return false;
 auto* memory=cp.*MemberOf(MemoryTag{});if(!memory)return false;
 auto* shared=(cp.*MemberOf(SharedTag{})).get();auto* texture=(cp.*MemberOf(TextureTag{})).get();if(!shared || !texture || texture->IsDrawResolutionScaled())return false;
 auto* guest_heap=memory->LookupHeapByType(true,65536);uint32_t guest_allocation=0;
 if(!guest_heap || !guest_heap->Alloc(2*65536,65536,rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite,false,&guest_allocation))return false;
 struct FreeGuest{rex::memory::BaseHeap* heap;uint32_t address;~FreeGuest(){heap->Release(address,nullptr);}}free_guest{guest_heap,guest_allocation};
 const uint32_t guest_vertices=guest_allocation&0x1fffffff,guest_destination=guest_vertices+65536;

 f->registers=std::make_unique<rex::graphics::RegisterFile>();std::memset(f->registers.get(),0,sizeof(*f->registers));
 f->trace=std::make_unique<rex::graphics::TraceWriter>(memory->physical_membase());
 set_stage("native cache initialize");
 f->native=std::make_unique<NativeCache>(*f->registers,*memory,*f->trace,1,1,cp,false);if(!f->native->Initialize())return false;
 // This isolated binding fixture supplies UNORM views directly, bypassing
 // Update's gamma accumulation. Initialize the state Update normally provides.
 (f->native.get()->*MemberOf(GammaTag{}))=0;
 if(!RegisterMaterialCapture(f->native.get(),device,128ull*1024*1024,true,true))return false;f->registration=HdrSceneCacheRegistration::Create(*f->native,device,256ull*1024*1024,HdrSceneEncoding::kNativeLinear);if(!f->registration)return false;
 {
  set_stage("zero-budget native cache");
  NativeCache denied_cache(*f->registers,*memory,*f->trace,1,1,cp,false);
  if(!denied_cache.Initialize())return false;
  auto denied_registration=HdrSceneCacheRegistration::Create(denied_cache,device,0);if(!denied_registration)return false;
  auto denied=AcquireHdrSceneBinding(&denied_cache,f->target.Get());if(!denied.active || denied.target || !denied.rtv.ptr)return false;
 }
 set_stage("native target objects");
 for(unsigned mode=0;mode<2;++mode){
  auto* legacy=mode?f->textures[1].Get():f->target.Get();auto binding=AcquireHdrSceneBinding(f->native.get(),legacy);
  if(!binding.active || !binding.target)return false;f->captures[mode]=binding.target;
  auto pool=std::make_shared<rex::ui::d3d12::D3D12CpuDescriptorPool>(provider,D3D12_DESCRIPTOR_HEAP_TYPE_RTV,3);
  auto draw=pool->AllocateDescriptor();if(!draw)return false;
  D3D12_RENDER_TARGET_VIEW_DESC view{};view.Format=DXGI_FORMAT_R8G8B8A8_UNORM;view.ViewDimension=mode?D3D12_RTV_DIMENSION_TEXTURE2DMS:D3D12_RTV_DIMENSION_TEXTURE2D;
  device->CreateRenderTargetView(legacy,&view,draw.GetHandle());
  Key key;key.pitch_tiles_at_32bpp=2;key.resource_format=0;key.msaa_samples=mode?rex::graphics::xenos::MsaaSamples::k4X:rex::graphics::xenos::MsaaSamples::k1X;
  auto srv_pool=std::make_shared<rex::ui::d3d12::D3D12CpuDescriptorPool>(provider,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,3);
  auto srv=srv_pool->AllocateDescriptor();if(!srv)return false;D3D12_SHADER_RESOURCE_VIEW_DESC source_view{};source_view.Format=view.Format;source_view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  source_view.ViewDimension=mode?D3D12_SRV_DIMENSION_TEXTURE2DMS:D3D12_SRV_DIMENSION_TEXTURE2D;if(!mode)source_view.Texture2D.MipLevels=1;
  device->CreateShaderResourceView(legacy,&source_view,srv.GetHandle());
  f->foreign[mode]=std::make_unique<Host>(key,legacy,std::move(draw),decltype(draw){},decltype(draw){},std::move(srv),decltype(draw){},D3D12_RESOURCE_STATE_RENDER_TARGET);
  (f->native.get()->*MemberOf(TargetsTag{})).emplace(key,f->foreign[mode].get());
  if(!Texture(device,DXGI_FORMAT_R8G8B8A8_UNORM,&f->textures[4+mode],mode?4:1))return false;
  auto dest_draw=pool->AllocateDescriptor();if(!dest_draw)return false;device->CreateRenderTargetView(f->textures[4+mode].Get(),&view,dest_draw.GetHandle());
  auto dest_key=key;dest_key.pitch_tiles_at_32bpp=1;dest_key.base_tiles=1;
  f->ownership_dest[mode]=std::make_unique<Host>(dest_key,f->textures[4+mode].Get(),std::move(dest_draw),decltype(draw){},decltype(draw){},decltype(draw){},decltype(draw){},D3D12_RESOURCE_STATE_RENDER_TARGET);
  auto owned=AcquireHdrSceneBinding(f->native.get(),f->textures[4+mode].Get(),&cp.GetDeferredCommandList());if(!owned.target)return false;f->ownership_hdr[mode]=owned.target;
  cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();

 }

 if(!f->captures[0] || !f->captures[1] || HdrSceneTarget::Create(nullptr) || HdrSceneTarget::Create(f->captures[0]->texture()))return false;
 for(unsigned mode=0;mode<2;++mode)if(!f->captures[mode]->allocation_bytes() || f->captures[mode]->texture()->GetDesc().SampleDesc.Count!=(mode?4:1))return false;
 set_stage("HDR native transfer pipelines");
 for(unsigned mode=0;mode<2;++mode){TransferKey key;key.mode=MemberOf(TransferModeTag{});key.dest_resource_format=key.source_resource_format=0;
  key.dest_msaa_samples=key.source_msaa_samples=mode?rex::graphics::xenos::MsaaSamples::k4X:rex::graphics::xenos::MsaaSamples::k1X;
  key.key|=kHdrTransferKeyBit;auto* pipelines=(f->native.get()->*MemberOf(TransferPipelineTag{}))(key);if(!pipelines || !pipelines[0])return false;f->transfer_pipelines[mode]=pipelines[0];
  auto unsupported=key;unsupported.source_resource_format=14;if((f->native.get()->*MemberOf(TransferPipelineTag{}))(unsupported))return false;
  if(!Texture(device,DXGI_FORMAT_R16G16B16A16_FLOAT,&f->textures[2+mode],mode?4:1))return false;
 }
 if(!Texture(device,DXGI_FORMAT_R16G16B16A16_FLOAT,&f->textures[6]) || !Texture(device,DXGI_FORMAT_R8G8B8A8_UNORM,&f->textures[7]))return false;
 set_stage("pipeline creation");
 for(unsigned mode=0;mode<2;++mode)for(unsigned blend=0;blend<2;++blend)for(unsigned extra=0;extra<2;++extra){
  D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=f->root.Get();p.VS={vs->GetBufferPointer(),vs->GetBufferSize()};
  p.PS={ps.data(),ps.size()};
  p.BlendState.IndependentBlendEnable=blend;
  for(unsigned i=0;i<8;++i){auto& b=p.BlendState.RenderTarget[i];b.BlendEnable=blend && (i==0 || i==4);b.SrcBlend=blend?D3D12_BLEND_SRC_ALPHA:D3D12_BLEND_ONE;
   b.DestBlend=blend?D3D12_BLEND_INV_SRC_ALPHA:D3D12_BLEND_ZERO;b.SrcBlendAlpha=D3D12_BLEND_ONE;b.DestBlendAlpha=D3D12_BLEND_ZERO;
   b.BlendOp=b.BlendOpAlpha=D3D12_BLEND_OP_ADD;b.RenderTargetWriteMask=(i==0 || i==4)?15:0;}
  p.SampleMask=UINT_MAX;p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.RasterizerState.DepthClipEnable=TRUE;
  p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;auto& s=p.DepthStencilState.FrontFace;
  s.StencilFunc=D3D12_COMPARISON_FUNC_ALWAYS;s.StencilFailOp=s.StencilDepthFailOp=s.StencilPassOp=D3D12_STENCIL_OP_KEEP;p.DepthStencilState.BackFace=s;
  p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.SampleDesc.Count=mode?4:1;p.NumRenderTargets=1;
  p.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;
  if(mode==1 && blend==0 && extra==1){
   const auto index=shader.constant_register_map().GetPackedFloatConstantIndex(0);
   const std::string source="cbuffer Values:register(b1){float4 values[128];};float4 main():SV_Target0{return values["+std::to_string(index)+"]; }";
   ComPtr<ID3DBlob> surface_pixel,surface_error;
   if(FAILED(compile(source.data(),source.size(),"surface-samples",nullptr,nullptr,"main","ps_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&surface_pixel,&surface_error)))return false;
   for(unsigned sample=0;sample<4;++sample){auto sample_desc=p;sample_desc.SampleMask=1u<<sample;sample_desc.PS={surface_pixel->GetBufferPointer(),surface_pixel->GetBufferSize()};sample_desc.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;if(FAILED(device->CreateGraphicsPipelineState(&sample_desc,IID_PPV_ARGS(&f->surface_sample_pipelines[sample]))))return false;}
  }
  if(mode==1 && blend==0 && extra==1)for(unsigned sample=0;sample<4;++sample){auto sample_desc=p;sample_desc.SampleMask=1u<<sample;auto sample_built=HdrScenePipeline::Create(sample_desc,*f->captures[mode]);if(!sample_built || FAILED(device->CreateGraphicsPipelineState(&sample_built->desc(),IID_PPV_ARGS(&f->sample_pipelines[sample]))))return false;}
  auto built=HdrScenePipeline::Create(p,*f->captures[mode]);if(!built || built->desc().SampleDesc.Count!=p.SampleDesc.Count ||
   built->desc().CachedPSO.CachedBlobSizeInBytes || built->desc().BlendState.RenderTarget[4].BlendEnable!=p.BlendState.RenderTarget[0].BlendEnable)return false;
  auto mismatch=p;mismatch.SampleDesc.Count=mode?1:4;if(HdrScenePipeline::Create(mismatch,*f->captures[mode]))return false;
  auto stale=p;stale.CachedPSO={ps.data(),ps.size()};auto fresh=HdrScenePipeline::Create(stale,*f->captures[mode]);
  if(!fresh || fresh->desc().CachedPSO.pCachedBlob || fresh->desc().CachedPSO.CachedBlobSizeInBytes)return false;
  auto logic=p;logic.BlendState.RenderTarget[0].LogicOpEnable=TRUE;if(HdrScenePipeline::Create(logic,*f->captures[mode]))return false;
  const auto& pipeline=extra?built->desc():p;
  const auto created=device->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&f->pipelines[mode*4+blend*2+extra]));
  if(FAILED(created)){
   REXLOG_ERROR("HDR export pipeline blend={} extra={} failed {:08X}",blend,extra,uint32_t(created));
   if(validation)for(uint64_t i=first;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes=0;validation->GetMessage(i,nullptr,&bytes);std::vector<uint8_t> data(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());if(SUCCEEDED(validation->GetMessage(i,message,&bytes)))REXLOG_ERROR("HDR export pipeline validation: {}",message->pDescription);}
   return false;
  }
 }
 set_stage("views");
 const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,10,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
 if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&f->rtvs))))return false;
 const auto step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);const auto first_rtv=f->rtvs->GetCPUDescriptorHandleForHeapStart();
 std::array<D3D12_CPU_DESCRIPTOR_HANDLE,10> rtvs{};
 for(unsigned mode=0;mode<2;++mode)for(unsigned i=0;i<5;++i){
  auto& handle=rtvs[mode*5+i];handle.ptr=first_rtv.ptr+(mode*5+i)*step;
  if(i==4){handle=f->captures[mode]->rtv();continue;}
  D3D12_RENDER_TARGET_VIEW_DESC view{};view.ViewDimension=mode?D3D12_RTV_DIMENSION_TEXTURE2DMS:D3D12_RTV_DIMENSION_TEXTURE2D;
  view.Format=i==0?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_R16G16B16A16_FLOAT;
  device->CreateRenderTargetView(i==0?(mode?f->textures[1].Get():f->target.Get()):nullptr,&view,handle);
 }
 // The native transfer root uses one float SRV and one packed address DWORD.
 const D3D12_DESCRIPTOR_HEAP_DESC source_views{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
 if(FAILED(device->CreateDescriptorHeap(&source_views,IID_PPV_ARGS(&f->views))))return false;
 auto source_cpu=f->views->GetCPUDescriptorHandleForHeapStart();
 std::array<D3D12_CPU_DESCRIPTOR_HANDLE,2> transfer_rtvs{};
 for(unsigned mode=0;mode<2;++mode){D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  view.ViewDimension=mode?D3D12_SRV_DIMENSION_TEXTURE2DMS:D3D12_SRV_DIMENSION_TEXTURE2D;if(!mode)view.Texture2D.MipLevels=1;
  device->CreateShaderResourceView(f->captures[mode]->texture(),&view,source_cpu);source_cpu.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  transfer_rtvs[mode].ptr=first_rtv.ptr+(mode*5+4)*step;D3D12_RENDER_TARGET_VIEW_DESC rtv{};rtv.Format=view.Format;rtv.ViewDimension=mode?D3D12_RTV_DIMENSION_TEXTURE2DMS:D3D12_RTV_DIMENSION_TEXTURE2D;
  device->CreateRenderTargetView(f->textures[2+mode].Get(),&rtv,transfer_rtvs[mode]);
 }
 uint8_t* mapped=nullptr;D3D12_RANGE none{0,0};if(FAILED(f->upload->Map(0,&none,reinterpret_cast<void**>(&mapped))))return false;
 std::memset(mapped,0,8192);
 for(unsigned mode=0;mode<2;++mode){auto& system=*reinterpret_cast<rex::graphics::DxbcShaderTranslator::SystemConstants*>(mapped+mode*1024);
  system.flags=(1u<<rex::graphics::DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift)|(1u<<rex::graphics::DxbcShaderTranslator::kSysFlag_AlphaPassIfEqual_Shift)|(1u<<rex::graphics::DxbcShaderTranslator::kSysFlag_AlphaPassIfGreater_Shift);
  for(auto& bias:system.color_exp_bias)bias=1.f;system.sample_count_log2[0]=system.sample_count_log2[1]=mode;
 }
 set_stage("native float constants");const Float4 color{4,2,.5,.25};const auto packed=shader.constant_register_map().GetPackedFloatConstantIndex(0);if(packed>=128)return false;
 const Float4 tile_color{8,4,2,.5};
 const std::array<Float4,4> sample_colors={Float4{2,4,8,.25},Float4{4,8,16,.5},Float4{8,16,32,.75},Float4{16,32,64,1}};
 for(unsigned i=0;i<4;++i)std::memcpy(mapped+4096+i*256+packed*sizeof(color),sample_colors[i].data(),sizeof(Float4));
 const std::array<Float4,4> surface_samples={Float4{.125,.25,2,8},Float4{.25,.5,4,16},Float4{.5,1,8,32},Float4{0,0,0,-1}};
 for(unsigned i=0;i<4;++i)std::memcpy(mapped+5120+i*256+packed*sizeof(color),surface_samples[i].data(),sizeof(Float4));

 std::memcpy(mapped+2048+packed*sizeof(color),color.data(),sizeof(color));std::memcpy(mapped+2304+packed*sizeof(color),tile_color.data(),sizeof(tile_color));
 const float triangle[]={-1,-1,-1,3,3,-1};std::memcpy(mapped+3072,triangle,sizeof(triangle));f->upload->Unmap(0,nullptr);
 f->list->SetGraphicsRootSignature(f->root.Get());for(unsigned i=1;i<4;++i)f->list->SetGraphicsRootConstantBufferView(i,f->upload->GetGPUVirtualAddress()+2048);
 const D3D12_VIEWPORT viewport{0,0,side,side,0,1},source_viewport{0,0,96,32,0,1};const D3D12_RECT scissor{0,0,side,side},source_scissor{0,0,96,32};f->list->RSSetViewports(1,&viewport);f->list->RSSetScissorRects(1,&scissor);
 f->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 rex::graphics::d3d12::DeferredCommandList deferred_clears(cp);
 for(unsigned draw=0;draw<8;++draw){const unsigned mode=draw/4;const float zero[4]{};
  f->list->RSSetViewports(1,&source_viewport);f->list->RSSetScissorRects(1,&source_scissor);
  f->list->SetGraphicsRootConstantBufferView(0,f->upload->GetGPUVirtualAddress()+mode*1024);
  f->list->ClearRenderTargetView(rtvs[mode*5],zero,0,nullptr);
  if(mode){deferred_clears.Reset();if(!f->captures[mode]->Clear(deferred_clears,zero))return false;deferred_clears.Execute(f->list.Get(),nullptr);}
  else if(!f->captures[mode]->Clear(f->list.Get(),zero))return false;
  f->list->SetPipelineState(f->pipelines[draw].Get());
  if(draw%2){
   set_stage("native framebuffer bind");stream.clear();f->native->InvalidateCommandListRenderTargets();BaseTarget* targets[5]={nullptr,f->foreign[mode].get(),nullptr,nullptr,nullptr};
   (f->native.get()->*MemberOf(BindTag{}))(targets);set_stage("native binding recorded");cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);set_stage("native binding executed");stream.clear();
  }else f->list->OMSetRenderTargets(1,rtvs.data()+mode*5,FALSE,nullptr);
  f->list->DrawInstanced(3,1,0,0);
  // Give the adjacent EDRAM tile distinct high-range data. A bad pitch/base
  // mapping must not pass merely because the source was spatially uniform.
  const LONG tile_x=mode?40:80,tile_y=mode?8:16;const D3D12_RECT tile_region{tile_x,0,tile_x+(mode?40:80),tile_y};
  f->list->RSSetScissorRects(1,&tile_region);f->list->SetGraphicsRootConstantBufferView(1,f->upload->GetGPUVirtualAddress()+2304);f->list->DrawInstanced(3,1,0,0);
  f->list->SetGraphicsRootConstantBufferView(1,f->upload->GetGPUVirtualAddress()+2048);f->list->RSSetScissorRects(1,&source_scissor);
  // Execute the actual native resolve-clear routine, not just the HDR helper.
  // A partial region must change while surrounding bright values survive.
  stream.clear();BaseTarget* clear_targets[]={f->foreign[mode].get()};
  const std::vector<Transfer> transfers[1];const uint64_t clear_value=0xFF804020;
  Transfer::Rectangle clear_rectangle{};clear_rectangle.x_pixels=2;clear_rectangle.y_pixels=1;clear_rectangle.width_pixels=3;clear_rectangle.height_pixels=4;
  (f->native.get()->*MemberOf(ClearTag{}))(1,clear_targets,transfers,&clear_value,&clear_rectangle);
  cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();
  if(draw%2){
   set_stage("native HDR ownership shader draw");auto* source=f->captures[mode]->texture();auto* destination=f->textures[2+mode].Get();
   const float sentinel[]={-.5f,.25f,2,1};f->list->ClearRenderTargetView(transfer_rtvs[mode],sentinel,0,nullptr);
   Barrier(f->list.Get(),source,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
   ID3D12DescriptorHeap* heaps[]={f->views.Get()};f->list->SetDescriptorHeaps(1,heaps);
   f->list->SetGraphicsRootSignature((f->native.get()->*MemberOf(TransferRootTag{}))[0]);f->list->SetPipelineState(f->transfer_pipelines[mode].Get());
   auto gpu=f->views->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=mode*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
   f->list->SetGraphicsRootDescriptorTable(0,gpu);Address address;address.dest_pitch=address.source_pitch=1;address.source_to_dest=0;f->list->SetGraphicsRoot32BitConstant(1,address.constant,0);
   const D3D12_VERTEX_BUFFER_VIEW vertices{f->upload->GetGPUVirtualAddress()+3072,sizeof(float)*6,sizeof(float)*2};f->list->IASetVertexBuffers(0,1,&vertices);
   const D3D12_RECT copy_region{1,0,7,7};f->list->RSSetScissorRects(1,&copy_region);f->list->OMSetRenderTargets(1,&transfer_rtvs[mode],FALSE,nullptr);f->list->DrawInstanced(3,1,0,0);
   Barrier(f->list.Get(),source,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
   auto* resolved=f->textures[2].Get();
   if(mode){Barrier(f->list.Get(),destination,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RESOLVE_SOURCE);Barrier(f->list.Get(),resolved,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RESOLVE_DEST);
    f->list->ResolveSubresource(resolved,0,destination,0,DXGI_FORMAT_R16G16B16A16_FLOAT);Barrier(f->list.Get(),destination,D3D12_RESOURCE_STATE_RESOLVE_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);Barrier(f->list.Get(),resolved,D3D12_RESOURCE_STATE_RESOLVE_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);}
   Barrier(f->list.Get(),resolved,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
   D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=resolved;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
   D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=(16+draw/2)*stride;to.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,side,side,1,256};
   f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),resolved,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
   f->list->SetGraphicsRootSignature(f->root.Get());for(unsigned i=0;i<4;++i)f->list->SetGraphicsRootConstantBufferView(i,f->upload->GetGPUVirtualAddress()+(i?2048:mode*1024));f->list->RSSetScissorRects(1,&scissor);
  }
  if(draw%2){
   set_stage("full native ownership transfer");const float sentinel[]={-.5f,.25f,2,1};if(!f->ownership_hdr[mode]->Clear(f->list.Get(),sentinel))return false;
   stream.clear();
   // Seed independent ambient values without a guest draw; the native ownership
   // loop below must copy these values using its EDRAM rectangles and sample map.
   {MaterialPipelineScope scope(f->native.get());if(!StampCurrentMaterialPipeline(f->pipelines[0].Get()) || !StampCurrentMaterialSurfacePipeline(f->pipelines[0].Get()))return false;}
   SetMaterialCaptureDraw(f->native.get(),f->pipelines[0].Get());
   auto material_source=AcquireMaterialCapture(f->native.get(),f->foreign[mode]->resource(),cp.GetDeferredCommandList());auto material_dest=AcquireMaterialCapture(f->native.get(),f->ownership_dest[mode]->resource(),cp.GetDeferredCommandList());
   auto surface_source=AcquireMaterialSurface(f->native.get(),f->foreign[mode]->resource(),cp.GetDeferredCommandList());auto surface_dest=AcquireMaterialSurface(f->native.get(),f->ownership_dest[mode]->resource(),cp.GetDeferredCommandList());
   SetMaterialCaptureDraw(f->native.get(),nullptr);if(!material_source.target || !material_dest.target || !surface_source.target || !surface_dest.target)return false;
   f->material_sources[mode]=material_source.target;f->material_destinations[mode]=material_dest.target;
   const float ambient_a[]={-.25f,2,8+float(draw),.75f},ambient_b[]={.5f,4,16+float(draw),.5f};const LONG source_x=mode?40:80;const D3D12_RECT material_half{source_x,0,source_x+4,32};
   if(!material_source.target->Clear(cp.GetDeferredCommandList(),ambient_a) || !material_source.target->Clear(cp.GetDeferredCommandList(),ambient_b,{&material_half,1}) || !material_dest.target->Clear(cp.GetDeferredCommandList(),sentinel))return false;
   const float surface_a[]={.125f,.5f,2,16+float(draw)},surface_b[]={.75f,.25f,4,32+float(draw)},absent[]={0,0,0,-1};const D3D12_RECT surface_missing{source_x+6,0,source_x+8,32};
   if(!surface_source.target->Clear(cp.GetDeferredCommandList(),surface_a) || !surface_source.target->Clear(cp.GetDeferredCommandList(),surface_b,{&material_half,1}) || !surface_source.target->Clear(cp.GetDeferredCommandList(),absent,{&surface_missing,1}) || !surface_dest.target->Clear(cp.GetDeferredCommandList(),sentinel))return false;
   // Main fixture draws bypass the native guest draw path, so invalidate the
   // external-state cache that path would normally replace before a transfer.
   cp.*MemberOf(current_external_pipeline_Tag{})=nullptr;cp.*MemberOf(current_guest_pipeline_Tag{})=nullptr;
   cp.*MemberOf(current_graphics_root_signature_Tag{})=nullptr;cp.*MemberOf(primitive_topology_Tag{})=D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
   cp.*MemberOf(ff_viewport_update_needed_Tag{})=true;cp.*MemberOf(ff_scissor_update_needed_Tag{})=true;
   BaseTarget* destinations[]={f->ownership_dest[mode].get()};const std::vector<Transfer> copies[1]={{Transfer(1,2,f->foreign[mode].get(),nullptr)}};
   (f->native.get()->*MemberOf(ClearTag{}))(1,destinations,copies,nullptr,nullptr);
   const auto native_scissor=cp.*MemberOf(ff_scissor_Tag{});if(native_scissor.left || native_scissor.top || native_scissor.right!=16384 || native_scissor.bottom!=16384)return false;
   cp.PushTransitionBarrier(f->foreign[mode]->resource(),f->foreign[mode]->SetResourceState(D3D12_RESOURCE_STATE_RENDER_TARGET),D3D12_RESOURCE_STATE_RENDER_TARGET);cp.SubmitBarriers();
   auto* native_heap=(cp.*MemberOf(bindless_resources_used_Tag{}))?(cp.*MemberOf(view_bindless_heap_Tag{})):(cp.*MemberOf(view_bindful_heap_current_Tag{}));
   if(!native_heap)return false;f->list->SetDescriptorHeaps(1,&native_heap);
   cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();
   {auto* source=f->material_destinations[mode]->texture();auto* resolved=mode?f->material_resolved.Get():source;
    if(mode){Barrier(f->list.Get(),source,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RESOLVE_SOURCE);Barrier(f->list.Get(),resolved,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RESOLVE_DEST);f->list->ResolveSubresource(resolved,0,source,0,DXGI_FORMAT_R16G16B16A16_FLOAT);Barrier(f->list.Get(),source,D3D12_RESOURCE_STATE_RESOLVE_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);Barrier(f->list.Get(),resolved,D3D12_RESOURCE_STATE_RESOLVE_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);}
    Barrier(f->list.Get(),resolved,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=resolved;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=(89+draw/2)*stride;to.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,side,side,1,256};f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),resolved,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);}
   {auto current=FindMaterialSurfaceSource(f->native.get(),f->ownership_dest[mode]->resource());if(current.target!=surface_dest.target)return false;auto* source=current.target->texture();auto* resolved=mode?f->material_resolved.Get():source;
    if(mode){Barrier(f->list.Get(),source,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RESOLVE_SOURCE);Barrier(f->list.Get(),resolved,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RESOLVE_DEST);f->list->ResolveSubresource(resolved,0,source,0,DXGI_FORMAT_R16G16B16A16_FLOAT);Barrier(f->list.Get(),source,D3D12_RESOURCE_STATE_RESOLVE_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);Barrier(f->list.Get(),resolved,D3D12_RESOURCE_STATE_RESOLVE_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);}
    Barrier(f->list.Get(),resolved,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=resolved;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=(126+draw/2)*stride;to.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,side,side,1,256};f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),resolved,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);}
   for(unsigned t=0;t<2;++t){auto* resource=t?f->ownership_hdr[mode]->texture():f->textures[4+mode].Get();
    auto* resolved=mode?f->textures[7-t].Get():resource;
    const auto format=t?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM;
    if(mode){Barrier(f->list.Get(),resource,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RESOLVE_SOURCE);Barrier(f->list.Get(),resolved,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RESOLVE_DEST);
     f->list->ResolveSubresource(resolved,0,resource,0,format);Barrier(f->list.Get(),resource,D3D12_RESOURCE_STATE_RESOLVE_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);Barrier(f->list.Get(),resolved,D3D12_RESOURCE_STATE_RESOLVE_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);}
    Barrier(f->list.Get(),resolved,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=resolved;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=((t?20:24)+draw/2)*stride;to.PlacedFootprint.Footprint={format,side,side,1,256};
    f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),resolved,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
   }
   f->list->SetGraphicsRootSignature(f->root.Get());for(unsigned i=0;i<4;++i)f->list->SetGraphicsRootConstantBufferView(i,f->upload->GetGPUVirtualAddress()+(i?2048:mode*1024));
   f->list->RSSetViewports(1,&viewport);f->list->RSSetScissorRects(1,&scissor);f->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  }
  for(unsigned t=0;t<2;++t){auto* resolved=t?f->captures[0]->texture():f->target.Get();
   auto* resource=t?f->captures[mode]->texture():(mode?f->textures[1].Get():f->target.Get());
   if(mode){Barrier(f->list.Get(),resource,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    Barrier(f->list.Get(),resolved,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RESOLVE_DEST);
    f->list->ResolveSubresource(resolved,0,resource,0,t?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM);
    Barrier(f->list.Get(),resource,D3D12_RESOURCE_STATE_RESOLVE_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    Barrier(f->list.Get(),resolved,D3D12_RESOURCE_STATE_RESOLVE_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);
   }
   Barrier(f->list.Get(),resolved,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
   D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=resolved;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
   D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=(draw*2+t)*stride;
   to.PlacedFootprint.Footprint={t?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM,side,side,1,256};const D3D12_BOX visible{0,0,0,side,side,1};f->list->CopyTextureRegion(&to,0,0,0,&from,&visible);
   if((draw%2) && !t){
    auto expected=to;expected.PlacedFootprint.Offset=(28+draw/2)*stride;
    const UINT tile_x=mode?40:80;const D3D12_BOX adjacent{tile_x,0,0,tile_x+side,side,1};f->list->CopyTextureRegion(&expected,0,0,0,&from,&adjacent);
   }
   Barrier(f->list.Get(),resolved,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);}
  if(draw%2){
   namespace g=rex::graphics;namespace x=g::xenos;
   set_stage("native HDR scene resolve");
   const float source_x=mode?40.f:80.f;const float vertices[]={source_x-.5f,-.5f,source_x+7.5f,-.5f,source_x-.5f,7.5f};std::memcpy(memory->TranslatePhysical(guest_vertices),vertices,sizeof(vertices));
   auto& regs=*f->registers;std::fill(std::begin(regs.values),std::end(regs.values),0u);
   auto set=[&]<class T>(T value){regs.values[T::register_index]=value.value;};
   x::xe_gpu_vertex_fetch_t fetch{};fetch.type=x::FetchConstantType::kVertex;fetch.size=6;fetch.address=guest_vertices/4;std::memcpy(&regs.values[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],&fetch,sizeof(fetch));
   g::reg::PA_SC_SCREEN_SCISSOR_BR screen{};screen.br_x=160;screen.br_y=32;set(screen);
   g::reg::PA_SC_WINDOW_SCISSOR_TL top{};top.window_offset_disable=1;set(top);g::reg::PA_SC_WINDOW_SCISSOR_BR window{};window.br_x=160;window.br_y=32;set(window);
   g::reg::RB_SURFACE_INFO surface{};surface.surface_pitch=mode?80:160;surface.msaa_samples=mode?x::MsaaSamples::k4X:x::MsaaSamples::k1X;set(surface);
   g::reg::RB_COLOR_INFO color_info{};color_info.color_format=x::ColorRenderTargetFormat::k_8_8_8_8;regs.values[g::XE_GPU_REG_RB_COLOR_INFO]=color_info.value;
   g::reg::RB_COPY_CONTROL control{};control.copy_command=x::CopyCommand::kRaw;control.color_clear_enable=1;control.copy_sample_select=mode?x::CopySampleSelect::k0123:x::CopySampleSelect::k0;set(control);
   g::reg::RB_COPY_DEST_INFO dest{};dest.copy_dest_format=x::ColorFormat(x::TextureFormat::k_8_8_8_8);set(dest);
   g::reg::RB_COPY_DEST_PITCH pitch{};pitch.copy_dest_pitch=160;pitch.copy_dest_height=32;set(pitch);regs.values[g::XE_GPU_REG_RB_COPY_DEST_BASE]=guest_destination;regs.values[g::XE_GPU_REG_RB_COLOR_CLEAR]=0xff102030;
   auto& info=f->resolved_info[draw/2];if(!g::draw_util::GetResolveInfo(regs,*memory,*f->trace,1,1,false,false,info))return false;
   (f->native.get()->*MemberOf(OwnTag{}))(f->foreign[mode]->key(),1,1,nullptr,nullptr);
   // Seed raw material attributes independently of scene colour. The native
   // copy below must capture them before its source clear, including validity.
   {MaterialPipelineScope scope(f->native.get());if(!StampCurrentMaterialPipeline(f->pipelines[0].Get()) || !StampCurrentMaterialSurfacePipeline(f->pipelines[0].Get()))return false;}
   SetMaterialCaptureDraw(f->native.get(),f->pipelines[0].Get());auto surface_binding=AcquireMaterialSurface(f->native.get(),f->foreign[mode]->resource(),cp.GetDeferredCommandList());SetMaterialCaptureDraw(f->native.get(),nullptr);if(!surface_binding.target)return false;
   const float surface_a[]={.125f,.5f,2,16+float(draw)},surface_b[]={.75f,.25f,4,32+float(draw)},unavailable[]={0,0,0,-1};
   const LONG origin=LONG(source_x);const D3D12_RECT half{origin,0,origin+4,32},missing{origin+6,0,origin+8,32};
   if(!surface_binding.target->Clear(cp.GetDeferredCommandList(),surface_a) || !surface_binding.target->Clear(cp.GetDeferredCommandList(),surface_b,{&half,1}) || !surface_binding.target->Clear(cp.GetDeferredCommandList(),unavailable,{&missing,1}))return false;
   uint32_t written=0,length=0;if(!f->native->Resolve(*memory,*shared,*texture,written,length) || !length)return false;
   rex::thread::global_critical_region region;auto lock=region.Acquire();auto scene=FindHdrSceneResolve(f->native.get(),info);if(!scene || scene->texture()->GetDesc().Width!=side || scene->texture()->GetDesc().Height!=side)return false;
   auto ambient_scene=FindMaterialSceneResolve(f->native.get(),info);if(!ambient_scene || scene->encoding()!=HdrSceneEncoding::kNativeLinear || ambient_scene->encoding()!=HdrSceneEncoding::kNativeLinear || ambient_scene==scene || ambient_scene->texture()==scene->texture() || ambient_scene->layer()!=SceneResolveLayer::kMaterialAmbient || scene->layer()!=SceneResolveLayer::kHdr)return false;
   auto surface_scene=FindMaterialSurfaceResolve(f->native.get(),info);if(!surface_scene || surface_scene==scene || surface_scene==ambient_scene || surface_scene->layer()!=SceneResolveLayer::kMaterialSurface || surface_scene->texture()==scene->texture())return false;
   CommitMaterialSurfaceResolve(f->native.get(),std::const_pointer_cast<HdrResolvedScene>(surface_scene),false);CommitHdrSceneResolve(f->native.get(),std::const_pointer_cast<HdrResolvedScene>(surface_scene),true);CommitMaterialSceneResolve(f->native.get(),std::const_pointer_cast<HdrResolvedScene>(surface_scene),true);if(FindMaterialSurfaceResolve(f->native.get(),info))return false;CommitMaterialSurfaceResolve(f->native.get(),std::const_pointer_cast<HdrResolvedScene>(surface_scene),true);
   auto wrong=info;wrong.copy_dest_base+=4096;if(FindHdrSceneResolve(f->native.get(),wrong) || FindMaterialSceneResolve(f->native.get(),wrong))return false;
   CommitMaterialSceneResolve(f->native.get(),std::const_pointer_cast<HdrResolvedScene>(ambient_scene),false);CommitHdrSceneResolve(f->native.get(),std::const_pointer_cast<HdrResolvedScene>(ambient_scene),true);if(FindMaterialSceneResolve(f->native.get(),info))return false;CommitMaterialSceneResolve(f->native.get(),std::const_pointer_cast<HdrResolvedScene>(ambient_scene),true);
   auto* legacy=f->foreign[mode].get();cp.PushTransitionBarrier(legacy->resource(),legacy->SetResourceState(D3D12_RESOURCE_STATE_RENDER_TARGET),D3D12_RESOURCE_STATE_RENDER_TARGET);cp.SubmitBarriers();
   ID3D12DescriptorHeap* native_heaps[]={(cp.*MemberOf(bindless_resources_used_Tag{}))?(cp.*MemberOf(view_bindless_heap_Tag{})):(cp.*MemberOf(view_bindful_heap_current_Tag{}))};f->list->SetDescriptorHeaps(1,native_heaps);
   cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();
   const auto read_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;Barrier(f->list.Get(),scene->texture(),read_state,D3D12_RESOURCE_STATE_COPY_SOURCE);
   D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=scene->texture();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=(32+draw/2)*stride;to.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,side,side,1,256};f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),scene->texture(),D3D12_RESOURCE_STATE_COPY_SOURCE,read_state);
   Barrier(f->list.Get(),ambient_scene->texture(),read_state,D3D12_RESOURCE_STATE_COPY_SOURCE);from.pResource=ambient_scene->texture();to.PlacedFootprint.Offset=(93+draw/2)*stride;f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),ambient_scene->texture(),D3D12_RESOURCE_STATE_COPY_SOURCE,read_state);
   Barrier(f->list.Get(),surface_scene->texture(),read_state,D3D12_RESOURCE_STATE_COPY_SOURCE);from.pResource=surface_scene->texture();to.PlacedFootprint.Offset=(115+draw/2)*stride;f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),surface_scene->texture(),D3D12_RESOURCE_STATE_COPY_SOURCE,read_state);
   f->list->SetGraphicsRootSignature(f->root.Get());for(unsigned i=0;i<4;++i)f->list->SetGraphicsRootConstantBufferView(i,f->upload->GetGPUVirtualAddress()+(i?2048:mode*1024));f->list->RSSetViewports(1,&viewport);f->list->RSSetScissorRects(1,&scissor);f->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  }
 }
 set_stage("distinct native MSAA resolve samples");
 // Repaint four host samples independently through the real translated guest
 // shader and extra HDR export. All seven guest selectors then resolve these
 // distinct values, with nonzero positive/negative bias and R/B swapping.
 stream.clear();f->native->InvalidateCommandListRenderTargets();BaseTarget* sample_targets[5]={nullptr,f->foreign[1].get(),nullptr,nullptr,nullptr};(f->native.get()->*MemberOf(BindTag{}))(sample_targets);cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();
 f->list->SetGraphicsRootSignature(f->root.Get());for(unsigned i=0;i<4;++i)f->list->SetGraphicsRootConstantBufferView(i,f->upload->GetGPUVirtualAddress()+(i?2048:1024));f->list->RSSetViewports(1,&source_viewport);f->list->RSSetScissorRects(1,&source_scissor);f->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 for(unsigned sample=0;sample<4;++sample){f->list->SetPipelineState(f->sample_pipelines[sample].Get());f->list->SetGraphicsRootConstantBufferView(1,f->upload->GetGPUVirtualAddress()+4096+sample*256);f->list->DrawInstanced(3,1,0,0);}
 SetMaterialCaptureDraw(f->native.get(),f->pipelines[0].Get());auto sample_surface=AcquireMaterialSurface(f->native.get(),f->foreign[1]->resource(),cp.GetDeferredCommandList());SetMaterialCaptureDraw(f->native.get(),nullptr);if(!sample_surface.target)return false;
 cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();
 {auto target=sample_surface.target->rtv();f->list->OMSetRenderTargets(1,&target,FALSE,nullptr);for(unsigned sample=0;sample<4;++sample){f->list->SetPipelineState(f->surface_sample_pipelines[sample].Get());f->list->SetGraphicsRootConstantBufferView(1,f->upload->GetGPUVirtualAddress()+5120+sample*256);f->list->DrawInstanced(3,1,0,0);}}
 for(unsigned selection=0;selection<7;++selection){
  namespace g=rex::graphics;namespace x=g::xenos;auto& regs=*f->registers;
  g::reg::RB_COPY_CONTROL control{};control.copy_command=x::CopyCommand::kConvert;control.copy_sample_select=x::CopySampleSelect(selection);regs.values[control.register_index]=control.value;
  g::reg::RB_COPY_DEST_INFO dest{};dest.copy_dest_format=x::ColorFormat(x::TextureFormat::k_8_8_8_8);dest.copy_dest_swap=selection&1;dest.copy_dest_exp_bias=int(selection%3)-1;regs.values[dest.register_index]=dest.value;
  auto& info=f->sample_info[selection];if(!g::draw_util::GetResolveInfo(regs,*memory,*f->trace,1,1,false,false,info))return false;
  uint32_t written=0,length=0;if(!f->native->Resolve(*memory,*shared,*texture,written,length) || !length)return false;
  rex::thread::global_critical_region region;auto lock=region.Acquire();auto scene=FindHdrSceneResolve(f->native.get(),info);if(!scene)return false;
  ID3D12DescriptorHeap* native_heaps[]={(cp.*MemberOf(bindless_resources_used_Tag{}))?(cp.*MemberOf(view_bindless_heap_Tag{})):(cp.*MemberOf(view_bindful_heap_current_Tag{}))};f->list->SetDescriptorHeaps(1,native_heaps);cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();
  const auto read_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;Barrier(f->list.Get(),scene->texture(),read_state,D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=scene->texture();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=(36+selection)*stride;to.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,side,side,1,256};f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),scene->texture(),D3D12_RESOURCE_STATE_COPY_SOURCE,read_state);
  auto surface=FindMaterialSurfaceResolve(f->native.get(),info);if(!surface)return false;Barrier(f->list.Get(),surface->texture(),read_state,D3D12_RESOURCE_STATE_COPY_SOURCE);from.pResource=surface->texture();to.PlacedFootprint.Offset=(119+selection)*stride;f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),surface->texture(),D3D12_RESOURCE_STATE_COPY_SOURCE,read_state);
 }

 set_stage("native scene view association");
 // Real native Resolve / guest writes with fixture camera metadata. This
 // verifies provenance propagation and rejection, not a live camera capture.
 {namespace g=rex::graphics;namespace x=g::xenos;auto& regs=*f->registers;
  const auto old_surface=regs.values[g::reg::RB_SURFACE_INFO::register_index],old_control=regs.values[g::reg::RB_COPY_CONTROL::register_index],old_pitch=regs.values[g::reg::RB_COPY_DEST_PITCH::register_index];
  struct RestoreViewRegisters{g::RegisterFile& regs;uint32_t surface,control,pitch;~RestoreViewRegisters(){regs.values[g::reg::RB_SURFACE_INFO::register_index]=surface;regs.values[g::reg::RB_COPY_CONTROL::register_index]=control;regs.values[g::reg::RB_COPY_DEST_PITCH::register_index]=pitch;}}restore_view{regs,old_surface,old_control,old_pitch};
  g::reg::RB_SURFACE_INFO surface{};surface.surface_pitch=160;surface.msaa_samples=x::MsaaSamples::k1X;regs.values[surface.register_index]=surface.value;
  g::reg::RB_COPY_CONTROL control{};control.copy_command=x::CopyCommand::kConvert;control.copy_sample_select=x::CopySampleSelect::k0;regs.values[control.register_index]=control.value;
  g::reg::RB_COPY_DEST_PITCH pitch{};pitch.copy_dest_pitch=pitch.copy_dest_height=32;regs.values[pitch.register_index]=pitch.value;
  SceneViewIdentity metadata;metadata.frame=cp.GetCurrentFrame();metadata.submission=cp.GetCurrentSubmission();metadata.width=metadata.height=8;metadata.draw=1;
  const auto view=std::make_shared<const SceneViewIdentity>(metadata);unsigned linked=0,rejected=0;
  for(unsigned selection=0;selection<5;++selection){
   const float x0=selection==4?80.f:0.f;const float vertices[]={x0-.5f,-.5f,x0+7.5f,-.5f,x0-.5f,7.5f};std::memcpy(memory->TranslatePhysical(guest_vertices),vertices,sizeof(vertices));
   auto selected=view;if(selection==2){auto stale=metadata;++stale.frame;selected=std::make_shared<const SceneViewIdentity>(stale);}if(selection==3){auto wrong=metadata;wrong.width=16;selected=std::make_shared<const SceneViewIdentity>(wrong);}
   if(selection==1)InvalidateHdrSceneView(f->native.get(),f->foreign[0]->resource());else if(!BindHdrSceneView(f->native.get(),f->foreign[0]->resource(),selected))return false;
   (f->native.get()->*MemberOf(OwnTag{}))(f->foreign[0]->key(),0,2,nullptr,nullptr);
   g::draw_util::ResolveInfo info;if(!g::draw_util::GetResolveInfo(regs,*memory,*f->trace,1,1,false,false,info))return false;
   uint32_t written=0,length=0;if(!f->native->Resolve(*memory,*shared,*texture,written,length) || !length)return false;
   {rex::thread::global_critical_region region;auto lock=region.Acquire();auto colour=FindHdrSceneResolve(f->native.get(),info),ambient=FindMaterialSceneResolve(f->native.get(),info);if(!colour || !ambient)return false;
    if(!selection){if(colour->view()!=view || ambient->view()!=view)return false;InvalidateHdrSceneView(f->native.get(),f->foreign[0]->resource());if(colour->view()!=view || ambient->view()!=view)return false;f->lighting_publication=RecordSceneAmbientPublicationTest(cp,*f->native,colour,ambient,*texture,f->foreign[0]->resource(),f->list.Get());if(!f->lighting_publication)return false;++linked;}
    else {if(colour->view() || ambient->view() || colour->lighting_applied())return false;++rejected;}}
   cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();cp.SetExternalPipeline(nullptr);stream.clear();
  }
  if(linked!=1 || rejected!=4)return false;
  std::ofstream report(directory/"scene-view-resolve-results.json");report<<"{\"association_cases\":5,\"native_copies\":6,\"linked\":"<<linked<<",\"rejected\":"<<rejected<<",\"scope\":\"Five native association cases plus one nested publication callback copy, actual guest writes and HDR/ambient images, fixture camera metadata. Identity full-grid link; source tag invalidation preserves already resolved link; missing, stale, wrong size and relocated source grid reject association. No live camera capture or displayed composition.\"}\n";
  if(!report.good())return false;
  REXLOG_INFO("Native scene view association: {} linked, {} missing/stale/size/relocated rejected; fixture camera",linked,rejected);
 }

 set_stage("native HDR texture sampling");
 // A separate full8x8 resolve at destination origin is eligible for a complete
 // texture replacement. The earlier offset partial resolves are not.
 {namespace g=rex::graphics;namespace x=g::xenos;auto& regs=*f->registers;
  const float vertices[]={-.5f,-.5f,7.5f,-.5f,-.5f,7.5f};std::memcpy(memory->TranslatePhysical(guest_vertices),vertices,sizeof(vertices));
  g::reg::RB_COPY_CONTROL control{};control.copy_command=x::CopyCommand::kConvert;control.copy_sample_select=x::CopySampleSelect::k0123;regs.values[control.register_index]=control.value;
  g::reg::RB_COPY_DEST_INFO dest{};dest.copy_dest_format=x::ColorFormat(x::TextureFormat::k_8_8_8_8);regs.values[dest.register_index]=dest.value;
  g::reg::RB_COPY_DEST_PITCH pitch{};pitch.copy_dest_pitch=pitch.copy_dest_height=32;regs.values[pitch.register_index]=pitch.value;
  (f->native.get()->*MemberOf(OwnTag{}))(f->foreign[1]->key(),0,1,nullptr,nullptr);
  set_stage("native sampling full resolve info");if(!g::draw_util::GetResolveInfo(regs,*memory,*f->trace,1,1,false,false,f->sampling_info))return false;
  set_stage("native sampling full resolve copy");uint32_t written=0,length=0;if(!f->native->Resolve(*memory,*shared,*texture,written,length) || !length)return false;
  cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();
 }
 const bool native_bindless=cp.*MemberOf(bindless_resources_used_Tag{});f->sampling_modes=native_bindless?2:1;
 const char sampling_shader[]=R"(
 cbuffer Params:register(b0){uint descriptor_index;};
 #if BINDLESS
 Texture2DArray<float4> source_image[]:register(t0);
 #else
 Texture2DArray<float4> source_image:register(t0);
 #endif
 RWByteAddressBuffer output_data:register(u0);
 [numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){
 #if BINDLESS
 float4 v=source_image[descriptor_index].Load(int4(p.xy,0,0));
 #else
 float4 v=source_image.Load(int4(p.xy,0,0));
 #endif
 output_data.Store4(p.y*256+p.x*16,asuint(v));})";
 set_stage("native sampling buffer");D3D12_RESOURCE_DESC buffer_desc{};buffer_desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer_desc.Width=stride;buffer_desc.Height=buffer_desc.DepthOrArraySize=buffer_desc.MipLevels=buffer_desc.SampleDesc.Count=1;buffer_desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;buffer_desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
 D3D12_HEAP_PROPERTIES sampling_heap{};sampling_heap.Type=D3D12_HEAP_TYPE_DEFAULT;sampling_heap.CreationNodeMask=sampling_heap.VisibleNodeMask=1;
 if(FAILED(device->CreateCommittedResource(&sampling_heap,D3D12_HEAP_FLAG_NONE,&buffer_desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&f->sampling_output))))return false;
 for(unsigned mode=0;mode<f->sampling_modes;++mode){
  set_stage(mode?"native sampling bindless pipeline":"native sampling bindful pipeline");
  D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,mode?UINT_MAX:1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};
  D3D12_ROOT_PARAMETER parameters[3]{};for(unsigned i=0;i<2;++i){parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[i].DescriptorTable={1,&ranges[i]};}parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[2].Constants={0,0,1};
  const D3D12_ROOT_SIGNATURE_DESC desc{3,parameters};ComPtr<ID3DBlob> bytes,error,shader;const D3D_SHADER_MACRO macros[]={{"BINDLESS",mode?"1":"0"},{nullptr,nullptr}};
  if(FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&bytes,&error)) || FAILED(device->CreateRootSignature(0,bytes->GetBufferPointer(),bytes->GetBufferSize(),IID_PPV_ARGS(&f->sampling_roots[mode]))) || FAILED(compile(sampling_shader,sizeof(sampling_shader)-1,"hdr-native-sampling",macros,nullptr,"main","cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3|(mode?D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES:0),0,&shader,&error))){REXLOG_ERROR("HDR sampling shader/root: {}",error?static_cast<const char*>(error->GetBufferPointer()):"no diagnostics");return false;}
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=f->sampling_roots[mode].Get();pso.CS={shader->GetBufferPointer(),shader->GetBufferSize()};if(FAILED(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&f->sampling_pipelines[mode]))))return false;
  set_stage(mode?"native sampling bindless cache":"native sampling bindful cache");f->sampling_caches[mode]=rex::graphics::d3d12::D3D12TextureCache::Create(*f->registers,*shared,1,1,cp,mode!=0);if(!f->sampling_caches[mode])return false;
  f->sampling_caches[mode]->BeginSubmission(cp.GetCurrentSubmission());f->sampling_caches[mode]->BeginFrame();AttachHdrSceneTextureCache(f->native.get(),f->sampling_caches[mode].get());
 }
 for(unsigned fallback=0;fallback<2;++fallback){
  set_stage(fallback?"native sampling CPU fallback":"native sampling HDR views");
  if(fallback){
   // Actual CPU invalidation with an unchanged guest texture key must break
   // descriptor reuse. Repaint the CPU backing with a known tiled RGBA value.
   rex::thread::global_critical_region region;auto lock=region.Acquire();shared->MemoryInvalidationCallback(guest_destination,65536,true);
   auto* data=static_cast<uint8_t*>(memory->TranslatePhysical(guest_destination));for(unsigned i=0;i<65536;i+=4){data[i]=32;data[i+1]=64;data[i+2]=128;data[i+3]=255;}
   for(unsigned mode=0;mode<f->sampling_modes;++mode){rex::graphics::d3d12::D3D12Shader::TextureBinding binding{};binding.dimension=rex::graphics::xenos::FetchOpDimension::k2D;if(f->sampling_caches[mode]->AreActiveTextureSRVKeysUpToDate(&f->sampling_keys[mode],&binding,1))return false;}
  }
  {rex::thread::global_critical_region region;auto lock=region.Acquire();auto ambient=FindMaterialSceneResolve(f->native.get(),f->sampling_info);auto surface=FindMaterialSurfaceResolve(f->native.get(),f->sampling_info);if(bool(ambient)!=(!fallback) || bool(surface)!=(!fallback))return false;for(unsigned mode=0;mode<f->sampling_modes;++mode){HdrTextureQuery query{f->sampling_info.copy_dest_base,8,8,uint32_t(f->sampling_info.copy_dest_coordinate_info.pitch_aligned_div_32),uint32_t(f->sampling_info.copy_dest_info.copy_dest_endian),1,1};auto surface_texture=FindMaterialSurfaceTexture(f->sampling_caches[mode].get(),query);if(bool(surface_texture)!=(!fallback) || (surface_texture && surface_texture->layer()!=SceneResolveLayer::kMaterialSurface))return false;auto selected=FindMaterialSceneTexture(f->sampling_caches[mode].get(),query);if(bool(selected)!=(!fallback) || (selected && selected->layer()!=SceneResolveLayer::kMaterialAmbient))return false;}}
  for(unsigned mode=0;mode<f->sampling_modes;++mode)for(unsigned swizzle=0;swizzle<2;++swizzle){
   namespace g=rex::graphics;namespace x=g::xenos;auto* cache=f->sampling_caches[mode].get();
   x::xe_gpu_texture_fetch_t fetch{};fetch.type=x::FetchConstantType::kTexture;fetch.dimension=x::DataDimension::k2DOrStacked;fetch.tiled=1;fetch.pitch=1;fetch.base_address=guest_destination>>12;fetch.format=x::TextureFormat::k_8_8_8_8;fetch.size_2d.width=fetch.size_2d.height=7;fetch.swizzle=swizzle?(2|(1<<3)|(0<<6)|(5<<9)):(0|(1<<3)|(2<<6)|(3<<9));
   std::memcpy(&f->registers->values[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],&fetch,sizeof(fetch));cache->TextureFetchConstantWritten(0);
   cp.*MemberOf(current_external_pipeline_Tag{})=nullptr;cache->RequestTextures(1);cp.SubmitBarriers();
   ID3D12DescriptorHeap* native_heaps[]={native_bindless?(cp.*MemberOf(view_bindless_heap_Tag{})):(cp.*MemberOf(view_bindful_heap_current_Tag{}))};f->list->SetDescriptorHeaps(1,native_heaps);cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();
   {rex::thread::global_critical_region region;auto lock=region.Acquire();auto selected=FindHdrSwapScene(cache,fetch);if(bool(selected)!=(!fallback && !swizzle)){REXLOG_ERROR("HDR swap selection mismatch: fallback={} mode={} swizzle={} selected={}",fallback,mode,swizzle,bool(selected));return false;}++f->swap_queries;
    if(selected && !mode){
     for(unsigned guard=0;guard<6;++guard){auto unsupported=fetch;switch(guard){case 0:unsupported.mip_max_level=1;unsupported.mip_address=fetch.base_address+32;break;case 1:unsupported.tiled=0;break;case 2:unsupported.format=x::TextureFormat::k_16_16_FLOAT;break;case 3:unsupported.size_2d.width=6;break;case 4:unsupported.swizzle^=1;break;case 5:unsupported.base_address+=16;break;}if(FindHdrSwapScene(cache,unsupported))return false;++f->swap_guards;}
     set_stage("native deferred HDR output");if(!f->swap_output.Initialize(device) || !Texture(device,kHdrOutputFormat,&f->textures[8],1,side,side,true))return false;f->swap_bindings=f->swap_output.CreateBindings(selected->texture(),f->textures[8].Get());if(!f->swap_bindings)return false;
     Barrier(f->list.Get(),f->textures[8].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
     std::array<uint32_t,8> fragmented{};
     if(native_bindless){for(auto& index:fragmented){index=cp.RequestPersistentViewBindlessDescriptor();if(index==UINT32_MAX)return false;}std::sort(fragmented.begin(),fragmented.end());for(unsigned i=0;i<8;i+=2)cp.ReleaseViewBindlessDescriptorImmediately(fragmented[i]);}
     for(unsigned output=0;output<2;++output){/* Each isolated Execute needs its own native pipeline declaration. */cp.*MemberOf(current_external_pipeline_Tag{})=nullptr;if(!f->swap_output.Record(cp,*f->swap_bindings,{1,output?400.f:200.f,1000}))return false;if(native_bindless && !output)for(unsigned i=1;i<8;i+=2)cp.ReleaseViewBindlessDescriptorImmediately(fragmented[i]);ID3D12DescriptorHeap* output_heaps[]={native_bindless?(cp.*MemberOf(view_bindless_heap_Tag{})):(cp.*MemberOf(view_bindful_heap_current_Tag{}))};f->list->SetDescriptorHeaps(1,output_heaps);cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();
      Barrier(f->list.Get(),f->textures[8].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=f->textures[8].Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=(51+output)*stride;to.PlacedFootprint.Footprint={kHdrOutputFormat,side,side,1,256};f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),f->textures[8].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
     }
    }
   }
   g::d3d12::D3D12Shader::TextureBinding binding{};binding.dimension=x::FetchOpDimension::k2D;
   auto depth_view=em2::AcquireSceneDepthView(*cache,fetch);if(!depth_view || depth_view->hdr_source()!=(!fallback) || em2::AcquireSceneDepthView(*cache,fetch,depth_view)!=depth_view)return false;
   {auto stale=fetch;stale.base_address+=1;if(em2::AcquireSceneDepthView(*cache,stale))return false;auto unsupported=fetch;unsupported.exp_adjust=1;if(em2::AcquireSceneDepthView(*cache,unsupported))return false;}
   f->depth_views.push_back(depth_view);
   if(!f->geometry_pass.Initialize(device))return false;auto geometry=f->geometry_pass.CreateBindings(depth_view,f->geometry_bindings.empty()?nullptr:f->geometry_bindings.back());if(!geometry)return false;{const auto previous=f->geometry_bindings.empty()?nullptr:f->geometry_bindings.back();const bool same=previous&&previous->positions()->GetDesc().Width==geometry->positions()->GetDesc().Width&&previous->positions()->GetDesc().Height==geometry->positions()->GetDesc().Height;if(previous&&((previous->positions()==geometry->positions())!=same || (previous->normals()==geometry->normals())!=same))return false;if(same)++f->geometry_reuses;else ++f->geometry_allocations;}
   em2::SceneGeometrySettings geometry_settings{};geometry_settings.clip_from_uv={2,-2,-1,1};for(unsigned i=0;i<4;++i)geometry_settings.inverse_view_projection[i][i]=1;
   cp.*MemberOf(current_external_pipeline_Tag{})=nullptr;if(!f->geometry_pass.Record(cp,*geometry,geometry_settings))return false;
   {ID3D12DescriptorHeap* heaps[]={native_bindless?(cp.*MemberOf(view_bindless_heap_Tag{})):(cp.*MemberOf(view_bindful_heap_current_Tag{}))};f->list->SetDescriptorHeaps(1,heaps);cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();}
   const auto geometry_read=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
   for(unsigned output=0;output<2;++output){auto* texture=output?geometry->normals():geometry->positions();Barrier(f->list.Get(),texture,geometry_read,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=texture;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=(61+output*8+fallback*4+mode*2+swizzle)*stride;to.PlacedFootprint.Footprint={output?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT,side,side,1,256};f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),texture,D3D12_RESOURCE_STATE_COPY_SOURCE,geometry_read);}
   f->geometry_bindings.push_back(geometry);

   std::array<rex::ui::d3d12::util::DescriptorCpuGpuHandlePair,2> views;if(!cp.RequestOneUseSingleViewDescriptors(2,views.data()))return false;
   uint32_t descriptor=0;
   if(mode){descriptor=cache->GetActiveTextureBindlessSRVIndex(binding);if(cache->GetActiveTextureBindlessSRVIndex(binding)!=descriptor)return false;}
   else cache->WriteActiveTextureBindfulSRV(binding,views[0].first);
   cache->WriteActiveTextureSRVKeys(&f->sampling_keys[mode],&binding,1);if(!cache->AreActiveTextureSRVKeysUpToDate(&f->sampling_keys[mode],&binding,1))return false;
   if(!fallback && !mode && !swizzle){shared->MemoryInvalidationCallback(guest_vertices,4096,true);if(!cache->AreActiveTextureSRVKeysUpToDate(&f->sampling_keys[mode],&binding,1))return false;}
   D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R32_TYPELESS;uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;uav.Buffer.NumElements=stride/4;uav.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;device->CreateUnorderedAccessView(f->sampling_output.Get(),nullptr,&uav,views[1].first);
   native_heaps[0]=native_bindless?(cp.*MemberOf(view_bindless_heap_Tag{})):(cp.*MemberOf(view_bindful_heap_current_Tag{}));f->list->SetDescriptorHeaps(1,native_heaps);
   f->list->SetComputeRootSignature(f->sampling_roots[mode].Get());f->list->SetPipelineState(f->sampling_pipelines[mode].Get());
   f->list->SetComputeRootDescriptorTable(0,mode?cp.GetViewBindlessHeapGPUStart():views[0].second);f->list->SetComputeRootDescriptorTable(1,views[1].second);f->list->SetComputeRoot32BitConstant(2,descriptor,0);f->list->Dispatch(1,1,1);
   Barrier(f->list.Get(),f->sampling_output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);f->list->CopyBufferRegion(f->readback.Get(),(43+fallback*4+mode*2+swizzle)*stride,f->sampling_output.Get(),0,stride);Barrier(f->list.Get(),f->sampling_output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
   std::array<rex::ui::d3d12::util::DescriptorCpuGpuHandlePair,2> owned_views;if(!cp.RequestOneUseSingleViewDescriptors(2,owned_views.data()))return false;
   device->CopyDescriptorsSimple(1,owned_views[0].first,depth_view->srv(),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);device->CreateUnorderedAccessView(f->sampling_output.Get(),nullptr,&uav,owned_views[1].first);
   native_heaps[0]=native_bindless?(cp.*MemberOf(view_bindless_heap_Tag{})):(cp.*MemberOf(view_bindful_heap_current_Tag{}));f->list->SetDescriptorHeaps(1,native_heaps);
   f->list->SetComputeRootSignature(f->sampling_roots[0].Get());f->list->SetPipelineState(f->sampling_pipelines[0].Get());
   f->list->SetComputeRootDescriptorTable(0,owned_views[0].second);f->list->SetComputeRootDescriptorTable(1,owned_views[1].second);f->list->SetComputeRoot32BitConstant(2,0,0);f->list->Dispatch(1,1,1);
   Barrier(f->list.Get(),f->sampling_output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);f->list->CopyBufferRegion(f->readback.Get(),(53+fallback*4+mode*2+swizzle)*stride,f->sampling_output.Get(),0,stride);Barrier(f->list.Get(),f->sampling_output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
 }
 set_stage("geometry discontinuity fixtures");
 for(unsigned pattern=0;pattern<3;++pattern){
  const unsigned width=pattern?8:5,height=pattern?8:3;
  {rex::thread::global_critical_region region;auto lock=region.Acquire();shared->MemoryInvalidationCallback(guest_destination,65536,true);auto* data=static_cast<uint8_t*>(memory->TranslatePhysical(guest_destination));std::memset(data,0,65536);
   for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){const bool foreground=pattern==1?x<4:pattern==2?x==3:false;const auto offset=rex::graphics::texture_util::GetTiledOffset2D(x,y,32,2);data[offset]=foreground?224:32;data[offset+1]=64;data[offset+2]=128;data[offset+3]=255;}}
  auto fetch=f->registers->GetTextureFetch(0);fetch.size_2d.width=width-1;fetch.size_2d.height=height-1;fetch.swizzle=0|(1<<3)|(2<<6)|(3<<9);std::memcpy(&f->registers->values[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],&fetch,sizeof(fetch));auto* cache=f->sampling_caches[0].get();cache->TextureFetchConstantWritten(0);
  cp.*MemberOf(current_external_pipeline_Tag{})=nullptr;cache->RequestTextures(1);cp.SubmitBarriers();{ID3D12DescriptorHeap* heaps[]={native_bindless?(cp.*MemberOf(view_bindless_heap_Tag{})):(cp.*MemberOf(view_bindful_heap_current_Tag{}))};f->list->SetDescriptorHeaps(1,heaps);cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();}
  auto view=em2::AcquireSceneDepthView(*cache,fetch);if(!view || view->hdr_source())return false;auto geometry=f->geometry_pass.CreateBindings(view,f->geometry_bindings.empty()?nullptr:f->geometry_bindings.back());if(!geometry)return false;{const auto previous=f->geometry_bindings.empty()?nullptr:f->geometry_bindings.back();const bool same=previous&&previous->positions()->GetDesc().Width==geometry->positions()->GetDesc().Width&&previous->positions()->GetDesc().Height==geometry->positions()->GetDesc().Height;if(previous&&((previous->positions()==geometry->positions())!=same || (previous->normals()==geometry->normals())!=same))return false;if(same)++f->geometry_reuses;else ++f->geometry_allocations;}
  em2::SceneGeometrySettings settings{};settings.clip_from_uv={2,-2,-1,1};for(unsigned i=0;i<4;++i)settings.inverse_view_projection[i][i]=1;
  if(!pattern){settings.inverse_view_projection[2][0]=.25f;settings.inverse_view_projection[2][1]=.125f;settings.camera_and_discontinuity={10000000,-20000000,30000000,.25f};for(unsigned i=0;i<3;++i)settings.inverse_view_projection[i][3]=settings.camera_and_discontinuity[i];}
  auto invalid=settings;invalid.inverse_view_projection[3]={0,0,0,0};if(f->geometry_pass.Record(cp,*geometry,invalid))return false;
  cp.*MemberOf(current_external_pipeline_Tag{})=nullptr;if(!f->geometry_pass.Record(cp,*geometry,settings) || !f->geometry_pass.Record(cp,*geometry,settings))return false;
  {ID3D12DescriptorHeap* heaps[]={native_bindless?(cp.*MemberOf(view_bindless_heap_Tag{})):(cp.*MemberOf(view_bindful_heap_current_Tag{}))};f->list->SetDescriptorHeaps(1,heaps);cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();}
  const auto read=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  for(unsigned output=0;output<2;++output){auto* resource=output?geometry->normals():geometry->positions();Barrier(f->list.Get(),resource,read,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=resource;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=(77+output*3+pattern)*stride;to.PlacedFootprint.Footprint={output?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT,width,height,1,256};f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),resource,D3D12_RESOURCE_STATE_COPY_SOURCE,read);}
  if(!f->occlusion_pass.Initialize(device))return false;
  for(unsigned enabled=0;enabled<2;++enabled){auto ao=f->occlusion_pass.CreateBindings(geometry,f->occlusion_bindings.empty()?nullptr:f->occlusion_bindings.back());if(!ao)return false;
   const auto previous=f->occlusion_bindings.empty()?nullptr:f->occlusion_bindings.back();if(previous){const bool same=previous->result()->GetDesc().Width==ao->result()->GetDesc().Width && previous->result()->GetDesc().Height==ao->result()->GetDesc().Height;if((previous->result()==ao->result())!=same)return false;}
   em2::SceneOcclusionSettings options{2,4,.01f,float(enabled)};
   for(unsigned invalid=0;invalid<3;++invalid){auto bad=options;if(invalid==0)bad.world_radius=0;else if(invalid==1)bad.pixel_radius=129;else bad.bias=std::numeric_limits<float>::quiet_NaN();if(f->occlusion_pass.Record(cp,*ao,bad))return false;}
   cp.*MemberOf(current_external_pipeline_Tag{})=nullptr;if(!f->occlusion_pass.Record(cp,*ao,options) || (enabled && !f->occlusion_pass.Record(cp,*ao,options)))return false;
   {ID3D12DescriptorHeap* heaps[]={native_bindless?(cp.*MemberOf(view_bindless_heap_Tag{})):(cp.*MemberOf(view_bindful_heap_current_Tag{}))};f->list->SetDescriptorHeaps(1,heaps);cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();}
   Barrier(f->list.Get(),ao->result(),read,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=ao->result();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=(83+pattern*2+enabled)*stride;to.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,(width+1)/2,(height+1)/2,1,256};f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),ao->result(),D3D12_RESOURCE_STATE_COPY_SOURCE,read);f->occlusion_bindings.push_back(ao);
  }
  f->depth_views.push_back(view);f->geometry_bindings.push_back(geometry);
 }
 set_stage("native gamma guest views");
 {namespace g=rex::graphics;namespace x=g::xenos;auto& regs=*f->registers;auto* legacy=f->foreign[0].get();auto* resource=legacy->resource();const auto extent=resource->GetDesc();const D3D12_RECT full{0,0,LONG(extent.Width),LONG(extent.Height)};
  const float linear[]={.125f,.25f,.5f,.75f};
  x::xe_gpu_vertex_fetch_t resolve_fetch{};resolve_fetch.type=x::FetchConstantType::kVertex;resolve_fetch.size=6;resolve_fetch.address=guest_vertices/4;std::memcpy(&regs.values[g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],&resolve_fetch,sizeof(resolve_fetch));
  g::reg::PA_SC_SCREEN_SCISSOR_BR screen{};screen.br_x=160;screen.br_y=32;regs.values[screen.register_index]=screen.value;g::reg::PA_SC_WINDOW_SCISSOR_TL top{};top.window_offset_disable=1;regs.values[top.register_index]=top.value;g::reg::PA_SC_WINDOW_SCISSOR_BR bottom{};bottom.br_x=160;bottom.br_y=32;regs.values[bottom.register_index]=bottom.value;regs.values[g::XE_GPU_REG_RB_COPY_DEST_BASE]=guest_destination;

  const float vertices[]={-.5f,-.5f,7.5f,-.5f,-.5f,7.5f};std::memcpy(memory->TranslatePhysical(guest_vertices),vertices,sizeof(vertices));
  g::reg::RB_SURFACE_INFO surface{};surface.surface_pitch=160;surface.msaa_samples=x::MsaaSamples::k1X;regs.values[surface.register_index]=surface.value;
  g::reg::RB_COPY_DEST_PITCH pitch{};pitch.copy_dest_pitch=pitch.copy_dest_height=32;regs.values[pitch.register_index]=pitch.value;
  for(unsigned selection=0;selection<6;++selection){const bool srgb=selection==3;
   if(selection==0 || srgb || selection==4){cp.PushTransitionBarrier(resource,legacy->SetResourceState(D3D12_RESOURCE_STATE_RENDER_TARGET),D3D12_RESOURCE_STATE_RENDER_TARGET);cp.SubmitBarriers();cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();cp.SetExternalPipeline(nullptr);stream.clear();
    float encoded[4];for(unsigned c=0;c<4;++c)encoded[c]=c==3?linear[c]:srgb?(linear[c]<=.0031308f?linear[c]*12.92f:1.055f*std::pow(linear[c],1.f/2.4f)-.055f):x::LinearToPWLGamma(linear[c]);
    f->list->ClearRenderTargetView(legacy->descriptor_draw().GetHandle(),encoded,0,nullptr);InvalidateHdrSceneDestination(f->native.get(),resource);MirrorHdrSceneClear(f->native.get(),resource,encoded,full,cp.GetDeferredCommandList());auto source=AcquireHdrSceneBinding(f->native.get(),resource,&cp.GetDeferredCommandList(),srgb,!srgb);if(!source.target || source.guest_transfer!=(srgb?HdrGuestTransfer::kSrgb:HdrGuestTransfer::kXenosPwl) || !source.target->Clear(cp.GetDeferredCommandList(),linear))return false;
   }
   g::reg::RB_COLOR_INFO colour{};colour.color_format=(selection<2 || selection>=4)?x::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:x::ColorRenderTargetFormat::k_8_8_8_8;regs.values[g::XE_GPU_REG_RB_COLOR_INFO]=colour.value;
   g::reg::RB_COPY_CONTROL control{};control.copy_command=selection==1 || selection==2 || selection==4?x::CopyCommand::kConvert:x::CopyCommand::kRaw;control.copy_sample_select=x::CopySampleSelect::k0;regs.values[control.register_index]=control.value;
   g::reg::RB_COPY_DEST_INFO dest{};dest.copy_dest_format=x::ColorFormat(x::TextureFormat::k_8_8_8_8);dest.copy_dest_exp_bias=(selection==2 || selection>=4)?1:0;regs.values[dest.register_index]=dest.value;
   (f->native.get()->*MemberOf(OwnTag{}))(legacy->key(),0,1,nullptr,nullptr);auto& info=f->guest_gamma_info[selection];if(!g::draw_util::GetResolveInfo(regs,*memory,*f->trace,1,1,false,false,info))return false;uint32_t written=0,length=0;if(!f->native->Resolve(*memory,*shared,*texture,written,length) || !length)return false;
   {rex::thread::global_critical_region region;auto lock=region.Acquire();auto image=FindHdrSceneResolve(f->native.get(),info);if(!image || !image->guest_texture() || !image->guest_srv_identity())return false;f->guest_gamma_transfer[selection]=image->guest_transfer();if(image->guest_transfer()!=HdrGuestTransfer::kRaw && (image->guest_texture()==image->texture() || image->guest_srv_identity()==image->srv_identity()))return false;
    const auto revision=image->revision();auto* encoded=image->guest_texture();const auto identity=image->guest_srv_identity();const auto pending_size=stream.size();
    if(!RefreshHdrSceneGuestView(f->native.get(),cp,std::const_pointer_cast<HdrResolvedScene>(image)) || image->revision()!=revision || image->guest_texture()!=encoded || image->guest_srv_identity()!=identity || stream.size()!=pending_size)return false;
    cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();cp.SetExternalPipeline(nullptr);stream.clear();
    for(unsigned image_index=0;image_index<2;++image_index){auto* sample=image_index?encoded:image->texture();constexpr auto state=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;Barrier(f->list.Get(),sample,state,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=sample;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=(97+selection*3+image_index)*stride;to.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,8,8,1,256};f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),sample,D3D12_RESOURCE_STATE_COPY_SOURCE,state);}}
   shared->UseAsCopySource();cp.SubmitBarriers();cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();cp.SetExternalPipeline(nullptr);stream.clear();f->list->CopyBufferRegion(f->readback.Get(),(99+selection*3)*stride,shared->GetBuffer(),info.copy_dest_base,4);shared->UseForReading();cp.SubmitBarriers();cp.GetDeferredCommandList().Execute(f->list.Get(),nullptr);stream.clear();cp.SetExternalPipeline(nullptr);stream.clear();
   REXLOG_INFO("Native gamma resolve case{} format{} command{} exponent{} guest transfer{}",selection,uint32_t(info.color_edram_info.format),uint32_t(info.rb_copy_control.copy_command),int32_t(info.copy_dest_info.copy_dest_exp_bias),uint32_t(f->guest_gamma_transfer[selection]));
  }
  SceneViewIdentity metadata;metadata.frame=cp.GetCurrentFrame();metadata.submission=cp.GetCurrentSubmission();metadata.width=metadata.height=8;metadata.draw=999;const auto view=std::make_shared<const SceneViewIdentity>(metadata);if(!BindHdrSceneView(f->native.get(),resource,view))return false;
  uint32_t written=0,length=0;if(!f->native->Resolve(*memory,*shared,*texture,written,length) || !length)return false;
  {rex::thread::global_critical_region region;auto lock=region.Acquire();auto colour=FindHdrSceneResolve(f->native.get(),f->guest_gamma_info[5]),ambient=FindMaterialSceneResolve(f->native.get(),f->guest_gamma_info[5]);if(!colour || !ambient || colour->view()!=view || colour->guest_transfer()!=HdrGuestTransfer::kXenosPwl)return false;f->gamma_lighting_publication=RecordSceneAmbientPublicationTest(cp,*f->native,colour,ambient,*texture,resource,f->list.Get());if(!f->gamma_lighting_publication)return false;}

 }

 set_stage("submission");if(FAILED(f->list->Close()))return false;ID3D12CommandList* lists[]={f->list.Get()};queue->ExecuteCommandLists(1,lists);
 if(FAILED(queue->Signal(f->fence.Get(),1)) || FAILED(f->fence->SetEventOnCompletion(1,f->event)) || WaitForSingleObject(f->event,30000)!=WAIT_OBJECT_0){f.release();return false;}
 if(!VerifySceneAmbientPublicationTest(f->lighting_publication,directory/"ambient-publication"))return false;
 if(!VerifySceneAmbientPublicationTest(f->gamma_lighting_publication,directory/"ambient-publication-gamma"))return false;
 D3D12_RANGE read{0,130*stride};if(FAILED(f->readback->Map(0,&read,reinterpret_cast<void**>(&mapped))))return false;
 unsigned guest_gamma_bad=0;double guest_gamma_error=0;for(unsigned selection=0;selection<6;++selection)for(unsigned c=0;c<4;++c){uint16_t bits;std::memcpy(&bits,mapped+(98+selection*3)*stride+c*2,2);const double encoded=Half(bits),native=double(mapped[(99+selection*3)*stride+c])/255.0;const auto error=std::abs(std::clamp(encoded,0.,1.)-native);guest_gamma_error=std::max(guest_gamma_error,error);guest_gamma_bad+=!std::isfinite(encoded) || error>2./255.;if(error>2./255.)REXLOG_INFO("Guest gamma mismatch case{} component{} guest{} native{}",selection,c,encoded,native);}
 std::ofstream guest_gamma_report(directory/"native-guest-gamma-results.json");guest_gamma_report<<"{\"native_copies\":6,\"components\":24,\"invalid\":"<<guest_gamma_bad<<",\"max_error\":"<<guest_gamma_error<<",\"formats\":[";for(unsigned i=0;i<6;++i)guest_gamma_report<<(i?",":"")<<f->guest_gamma_info[i].color_edram_info.format;guest_gamma_report<<"],\"transfers\":[";for(unsigned i=0;i<6;++i)guest_gamma_report<<(i?",":"")<<uint32_t(f->guest_gamma_transfer[i]);guest_gamma_report<<"],\"scope\":\"Native Resolve raw gamma, converted gamma, unsigned reinterpretation with exponent bias, and hardware-sRGB-storage raw case. Canonical FP16 source and encoded legacy R8 seeded consistently. First resolved guest FP16 pixel versus actual native packed RGBA8 destination with UNORM clamp and two-byte-step quantization tolerance. No live title shader/postprocess proof.\"}\n";REXLOG_INFO("Native guest gamma views:24 components invalid{}, max difference{}",guest_gamma_bad,guest_gamma_error);if(guest_gamma_bad || !guest_gamma_report.good())return false;

 unsigned bad=0,ldr_differences=0;double max_error=0;
 for(unsigned mode=0;mode<2;++mode)for(unsigned blend=0;blend<2;++blend)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x){
  const auto* control=mapped+(mode*8+blend*4)*stride+y*256+x*4;const auto* actual=mapped+(mode*8+blend*4+2)*stride+y*256+x*4;ldr_differences+=std::memcmp(control,actual,4)!=0;
  for(unsigned c=0;c<4;++c){uint16_t v;std::memcpy(&v,mapped+(mode*8+blend*4+3)*stride+y*256+x*8+c*2,2);
   const bool cleared=x>=2 && x<5 && y>=1 && y<5;const double clear_color[]={32./255,64./255,128./255,1};
   const double expected=cleared?clear_color[c]:color[c]*(blend && c<3?.25:1);const double error=std::abs(Half(v)-expected);max_error=std::max(max_error,error);bad+=!std::isfinite(Half(v)) || error>.001;}
 }
 unsigned transfer_bad=0;double transfer_error=0;
 for(unsigned image=0;image<4;++image)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)for(unsigned c=0;c<4;++c){
  uint16_t bits;std::memcpy(&bits,mapped+(16+image)*stride+y*256+x*8+c*2,2);const double sentinel[]={-.5,.25,2,1},clear_color[]={32./255,64./255,128./255,1};
  const bool copied=x>=1 && x<7 && y<7,cleared=x>=2 && x<5 && y>=1 && y<5;
  const double expected=!copied?sentinel[c]:cleared?clear_color[c]:color[c]*((image%2) && c<3?.25:1);
  const double error=std::abs(Half(bits)-expected);transfer_error=std::max(transfer_error,error);transfer_bad+=!std::isfinite(Half(bits)) || error>.001;
 }
 REXLOG_INFO("Native HDR ownership shader variant:4 draws1x/4xMSAA,1024 components, invalid {}, max error {}; high-range passthrough and untouched clipped-out sentinel",transfer_bad,transfer_error);
 std::ofstream transfer_report(directory/"hdr-transfer-results.json");transfer_report<<"{\n\"draws\":4,\n\"components\":1024,\n\"invalid\":"<<transfer_bad<<",\n\"max_error\":"<<transfer_error<<",\n\"scope\":\"native generated same-format transfer shader with FP16 sources/destinations, identity address, clipped draw; native ownership loop not integrated\"\n}\n";
 unsigned ownership_bad=0,ownership_legacy_differences=0;double ownership_error=0;
 for(unsigned image=0;image<4;++image)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x){
  ownership_legacy_differences+=std::memcmp(mapped+(24+image)*stride+y*256+x*4,mapped+(28+image)*stride+y*256+x*4,4)!=0;
  for(unsigned c=0;c<4;++c){uint16_t bits;std::memcpy(&bits,mapped+(20+image)*stride+y*256+x*8+c*2,2);const double clear_color[]={32./255,64./255,128./255,1};
   const double expected=!(image%2)?tile_color[c]:c<3?tile_color[c]*.5+color[c]*.125:tile_color[c];
   const double error=std::abs(Half(bits)-expected);ownership_error=std::max(ownership_error,error);ownership_bad+=!std::isfinite(Half(bits)) || error>.001;
  }
 }
 unsigned material_bad=0;double material_error=0;
 for(unsigned image=0;image<4;++image)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)for(unsigned c=0;c<4;++c){const double a[]={-.25,2,9.+2*image,.75},b[]={.5,4,17.+2*image,.5};uint16_t bits;std::memcpy(&bits,mapped+(89+image)*stride+y*256+x*8+c*2,2);const double error=std::abs(Half(bits)-(x<4?b[c]:a[c]));material_error=std::max(material_error,error);material_bad+=!std::isfinite(Half(bits)) || error!=0;}
 const auto material_transfers=GetMaterialCaptureStats(f->native.get()).transfers;
 std::ofstream material_report(directory/"material-ownership-results.json");material_report<<"{\"mirrored_draws\":"<<material_transfers<<",\"components\":1024,\"invalid\":"<<material_bad<<",\"max_error\":"<<material_error<<",\"scope\":\"Actual native ownership loop, same native descriptor allocation and FP16 transfer PSO; independent ambient source values, negative/high-range and alpha, half-tile discontinuity, 1x/4x MSAA plus hardware readback resolve. HDR mirror and original legacy outputs checked alongside. Seeded clears, not game material draws or authoritative resolve-to-texture tracking.\"}\n";
 REXLOG_INFO("Native ambient ownership: {} transfers, invalid {}, error {}",material_transfers,material_bad,material_error);
 unsigned material_resolve_bad=0;double material_resolve_error=0;
 for(unsigned image=0;image<4;++image)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)for(unsigned c=0;c<4;++c){const double a[]={-.25,2,9.+2*image,.75},b[]={.5,4,17.+2*image,.5};uint16_t bits;std::memcpy(&bits,mapped+(93+image)*stride+y*256+x*8+c*2,2);const double error=std::abs(Half(bits)-(x<4?b[c]:a[c]));material_resolve_error=std::max(material_resolve_error,error);material_resolve_bad+=!std::isfinite(Half(bits)) || error!=0;}
 std::ofstream material_resolve_report(directory/"material-resolve-results.json");material_resolve_report<<"{\"native_copies\":4,\"components\":1024,\"invalid\":"<<material_resolve_bad<<",\"max_error\":"<<material_resolve_error<<",\"scope\":\"Actual native Resolve captures ambient before source clear and guest RGBA8 packing; separate HDR/ambient images at same destination. 1x/4x MSAA, half-tile boundary, negative/high-range and alpha. Exact metadata and cross-layer commit guards; full texture lookup in two binding modes and real CPU destination invalidation. Seeded ambient sources, no live scene composition.\"}\n";
 REXLOG_INFO("Native ambient resolves:4 copies, invalid {}, error {}",material_resolve_bad,material_resolve_error);
 unsigned surface_bad=0,surface_components=0;double surface_error=0;
 auto check_surface=[&](unsigned image,unsigned x,unsigned y,unsigned c,double expected){uint16_t bits;std::memcpy(&bits,mapped+image*stride+y*256+x*8+c*2,2);const double actual=Half(bits),error=std::abs(actual-expected);surface_error=std::max(surface_error,error);surface_bad+=!std::isfinite(actual) || error!=0;++surface_components;};
 for(unsigned image=0;image<4;++image)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)for(unsigned c=0;c<4;++c){const double a[]={.125,.5,2,17.+2*image},b[]={.75,.25,4,33.+2*image};check_surface(115+image,x,y,c,x>=6?(c==3?-1:0):(x<4?b[c]:a[c]));}
 for(unsigned selection=0;selection<7;++selection){const unsigned mask=selection<4?1u<<selection:selection==4?3:selection==5?12:15;for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)for(unsigned c=0;c<4;++c){double expected=0;unsigned count=0;bool missing=false;for(unsigned guest=0;guest<4;++guest)if(mask&(1u<<guest)){const unsigned host=((guest&1)<<1)|(guest>>1);const unsigned channel=(selection&1) && c<3?2-c:c;expected+=surface_samples[host][channel];missing|=surface_samples[host][3]<0;++count;}check_surface(119+selection,x,y,c,missing?(c==3?-1:0):expected/count);}}
 std::ofstream surface_report(directory/"surface-resolve-results.json");surface_report<<"{\"native_copies\":11,\"components\":"<<surface_components<<",\"invalid\":"<<surface_bad<<",\"max_error\":"<<surface_error<<",\"scope\":\"Native Resolve, separate raw surface layer; 1x/4xMSAA, before source clear, half-tile and unavailable pixels, all seven distinct sample selectors, mixed unavailable sample rejection, specular exponents above one, RGB permutation without colour exponent bias, cross-layer commit rejection, texture lookup and CPU invalidation. Controlled seeded attributes, no live lighting consumer.\"}\n";REXLOG_INFO("Native raw surface resolves:{} components invalid{}, max error{}",surface_components,surface_bad,surface_error);if(surface_bad || !surface_report.good())return false;
 unsigned surface_copy_bad=0;double surface_copy_error=0;
 for(unsigned image=0;image<4;++image)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)for(unsigned c=0;c<4;++c){const double a[]={.125,.5,2,17.+2*image},b[]={.75,.25,4,33.+2*image};const double expected=x>=6?(c==3?-1:0):(x<4?b[c]:a[c]);uint16_t bits;std::memcpy(&bits,mapped+(126+image)*stride+y*256+x*8+c*2,2);const double actual=Half(bits),error=std::abs(actual-expected);surface_copy_error=std::max(surface_copy_error,error);surface_copy_bad+=!std::isfinite(actual) || error!=0;}
 const auto surface_copies=GetMaterialCaptureStats(f->native.get()).surface_transfers;
 std::ofstream surface_copy_report(directory/"surface-ownership-results.json");surface_copy_report<<"{\"mirrored_draws\":"<<surface_copies<<",\"components\":1024,\"invalid\":"<<surface_copy_bad<<",\"max_error\":"<<surface_copy_error<<",\"scope\":\"Actual native ownership loop and shared descriptor allocation, FP16 surface copy at1x/4xMSAA, source pitch2/base0 to destination pitch1/base1 across a tile boundary. Independent diffuse/exponent data, values above one and unavailable alpha-1 pixels; destination remains available, original HDR/ambient/guest outputs tested alongside. Controlled seeded attributes, no live lighting consumer.\"}\n";
 REXLOG_INFO("Native surface ownership:{} transfers,1024 components invalid{}, error{}",surface_copies,surface_copy_bad,surface_copy_error);if(surface_copies!=4 || surface_copy_bad || !surface_copy_report.good())return false;
 unsigned resolve_bad=0;double resolve_error=0;
 for(unsigned image=0;image<4;++image)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)for(unsigned c=0;c<4;++c){uint16_t bits;std::memcpy(&bits,mapped+(32+image)*stride+y*256+x*8+c*2,2);const double expected=!(image%2)?tile_color[c]:c<3?tile_color[c]*.5+color[c]*.125:tile_color[c];const double error=std::abs(Half(bits)-expected);resolve_error=std::max(resolve_error,error);resolve_bad+=!std::isfinite(Half(bits)) || error>.001;}
 unsigned sample_bad=0;double sample_error=0;
 for(unsigned selection=0;selection<7;++selection){const unsigned mask=selection<4?1u<<selection:selection==4?3:selection==5?12:15;
  for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)for(unsigned c=0;c<4;++c){const unsigned channel=(selection&1) && c<3?2-c:c;double expected=0;unsigned count=0;for(unsigned guest=0;guest<4;++guest)if(mask&(1u<<guest)){const auto host=((guest&1)<<1)|(guest>>1);expected+=sample_colors[host][channel];++count;}expected=std::ldexp(expected/count,int(selection%3)-1);uint16_t bits;std::memcpy(&bits,mapped+(36+selection)*stride+y*256+x*8+c*2,2);const double error=std::abs(Half(bits)-expected);sample_error=std::max(sample_error,error);sample_bad+=!std::isfinite(Half(bits)) || error>.001;}}
 REXLOG_INFO("Native HDR distinct-sample resolve:7 selectors,1792 components, invalid {}, max error {}; bias and R/B swap checked",sample_bad,sample_error);
 REXLOG_INFO("Native HDR scene resolve:4 native copies before clear,1x/4xMSAA,1024 components, invalid {}, max error {}",resolve_bad,resolve_error);
 std::ofstream resolve_report(directory/"hdr-resolve-results.json");resolve_report<<"{\n\"native_copies\":4,\n\"components\":1024,\n\"invalid\":"<<resolve_bad<<",\n\"max_error\":"<<resolve_error<<",\n\"sample_selectors\":7,\n\"sample_components\":1792,\n\"invalid_sample_components\":"<<sample_bad<<",\n\"max_sample_error\":"<<sample_error<<",\n\"scope\":\"actual native Resolve with native ownership, GPU compute FP16 resolve, legacy guest write, subsequent source clear, exact metadata lookup and CPU invalidation; fixture-owned sources\"\n}\n";
 unsigned sampling_bad=0;double sampling_error=0;
 for(unsigned fallback=0;fallback<2;++fallback)for(unsigned mode=0;mode<f->sampling_modes;++mode)for(unsigned swizzle=0;swizzle<2;++swizzle)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)for(unsigned c=0;c<4;++c){const double hdr[]={7.5,15,30,.625},ldr[]={32./255,64./255,128./255,1};const unsigned channel=swizzle && c<3?2-c:c;const double expected=swizzle && c==3?1:(fallback?ldr[channel]:hdr[channel]);float actual;std::memcpy(&actual,mapped+(43+fallback*4+mode*2+swizzle)*stride+y*256+x*16+c*4,4);const double error=std::abs(actual-expected);sampling_error=std::max(sampling_error,error);sampling_bad+=!std::isfinite(actual) || error>.000001;}
 unsigned ao_bad=0,ao_components=0;double ao_min=1;unsigned ao_contact_pixels=0;
 for(unsigned pattern=0;pattern<3;++pattern)for(unsigned enabled=0;enabled<2;++enabled){const unsigned width=pattern?4:3,height=pattern?4:2;
  for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){double value[4];for(unsigned c=0;c<4;++c){uint16_t bits;std::memcpy(&bits,mapped+(83+pattern*2+enabled)*stride+y*256+x*8+c*2,2);value[c]=Half(bits);ao_bad+=!std::isfinite(value[c]);++ao_components;}
   const double visibility=value[3],length=value[0]*value[0]+value[1]*value[1]+value[2]*value[2];ao_bad+=visibility<0 || visibility>1;
   if(!enabled || !pattern)ao_bad+=visibility!=1;
   const bool invalid_normal=pattern==2 && x==1;if(invalid_normal)ao_bad+=visibility!=1 || length!=0;else ao_bad+=std::abs(length-1)>.002;
   if(enabled && pattern==1 && x<2)ao_bad+=visibility!=1; // Foreground plane must not shadow itself.
   if(enabled && pattern && x==2){ao_bad+=visibility>=.995;++ao_contact_pixels;ao_min=std::min(ao_min,visibility);}
  }}
 std::ofstream ao_report(directory/"scene-occlusion-results.json");ao_report<<"{\"dispatches\":9,\"bindings\":"<<f->occlusion_bindings.size()<<",\"components\":"<<ao_components<<",\"invalid\":"<<ao_bad<<",\"contact_pixels\":"<<ao_contact_pixels<<",\"minimum_visibility\":"<<ao_min<<",\"scope\":\"Native deferred geometry horizon AO and bent normals, half-resolution. Tilted plane has no self-occlusion, depth step/thin foreground occlude background, invalid normals remain unoccluded, disabled strength exact1, unit bent normals, odd dimensions, output reuse, repeated transitions and rejected invalid settings. Synthetic geometry; no live lighting integration.\"}\n";
 REXLOG_INFO("Scene geometry occlusion: {} components, invalid{}, contact pixels{}, minimum visibility{}",ao_components,ao_bad,ao_contact_pixels,ao_min);if(ao_bad || !ao_report.good() || f->occlusion_bindings.size()!=6)return false;
 unsigned geometry_bad=0;double geometry_error=0;unsigned geometry_components=0;
 for(unsigned fallback=0;fallback<2;++fallback)for(unsigned mode=0;mode<f->sampling_modes;++mode)for(unsigned swizzle=0;swizzle<2;++swizzle)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)for(unsigned c=0;c<4;++c){
  float position;uint16_t normal;const auto index=fallback*4+mode*2+swizzle;std::memcpy(&position,mapped+(61+index)*stride+y*256+x*16+c*4,4);std::memcpy(&normal,mapped+(69+index)*stride+y*256+x*8+c*2,2);
  const double z=1-(swizzle?128.:32.)/255.;const double expected_position=!fallback?0:c==0?2*(double(x)+.5)/side-1:c==1?1-2*(double(y)+.5)/side:c==2?z:1;
  const double expected_normal=!fallback?0:c==2?-1:c==3?1:0;
  for(auto pair:{std::pair<double,double>{position,expected_position},{Half(normal),expected_normal}}){const auto delta=std::abs(pair.first-pair.second);geometry_error=std::max(geometry_error,delta);geometry_bad+=!std::isfinite(pair.first)||delta>1e-5;++geometry_components;}}
 for(unsigned pattern=0;pattern<3;++pattern){const unsigned width=pattern?8:5,height=pattern?8:3;for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)for(unsigned c=0;c<4;++c){const bool foreground=pattern==1?x<4:pattern==2?x==3:false;const double z=1-(foreground?224.:32.)/255.;const bool normal_valid=!(pattern==2&&x==3);float position;uint16_t normal;std::memcpy(&position,mapped+(77+pattern)*stride+y*256+x*16+c*4,4);std::memcpy(&normal,mapped+(80+pattern)*stride+y*256+x*8+c*2,2);
  const double px=2*(double(x)+.5)/width-1,py=1-2*(double(y)+.5)/height;const double expected_position=c==0?px:c==1?py:c==2?z+(!pattern?.25*px+.125*py:0):1;const double expected_normal=!normal_valid?0:c==3?1:!pattern?(c==0?.25:c==1?.125:-1)/std::sqrt(1+.25*.25+.125*.125):c==2?-1:0;
  for(unsigned channel=0;channel<2;++channel){const double actual=channel?Half(normal):position,expected=channel?expected_normal:expected_position;const double delta=std::abs(actual-expected);geometry_error=std::max(geometry_error,delta);geometry_bad+=!std::isfinite(actual)||delta>(channel && !pattern?5e-4:1e-5);++geometry_components;}}}
 std::ofstream geometry_report(directory/"scene-geometry-results.json");geometry_report<<"{\"output_allocations\":"<<f->geometry_allocations<<",\"output_reuses\":"<<f->geometry_reuses<<",\"dispatches\":"<<f->geometry_bindings.size()+3<<",\"components\":"<<geometry_components<<",\"invalid\":"<<geometry_bad<<",\"max_error\":"<<geometry_error<<",\"scope\":\"Native deferred geometry compute on owned native/HDR views: independent CPU camera-relative planar positions and unit normals; invalid depths, borders, swizzles, both cache modes, 5x3 odd tilted plane at large camera coordinates, depth step, thin foreground, repeated dispatch transitions and invalid matrix rejection. Explicit synthetic clip mapping; no actual game geometry or lighting.\"}\n";
 REXLOG_INFO("Scene geometry compute: {} dispatches, {} components, invalid{}, max error{}",f->geometry_bindings.size()+3,geometry_components,geometry_bad,geometry_error);if(geometry_bad || !geometry_report.good() || f->geometry_allocations!=3 || f->geometry_reuses!=f->geometry_bindings.size()-3)return false;
 unsigned depth_bad=0;double depth_error=0;
 for(unsigned fallback=0;fallback<2;++fallback)for(unsigned mode=0;mode<f->sampling_modes;++mode)for(unsigned swizzle=0;swizzle<2;++swizzle)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)for(unsigned c=0;c<4;++c){float a,b;const auto offset=y*256+x*16+c*4;std::memcpy(&a,mapped+(43+fallback*4+mode*2+swizzle)*stride+offset,4);std::memcpy(&b,mapped+(53+fallback*4+mode*2+swizzle)*stride+offset,4);const double error=std::abs(double(a)-b);depth_error=std::max(depth_error,error);depth_bad+=!std::isfinite(b) || error>1e-6;}
 std::ofstream depth_report(directory/"scene-depth-view-results.json");depth_report<<"{\"cache_modes\":"<<f->sampling_modes<<",\"views\":"<<f->depth_views.size()<<",\"components\":"<<f->sampling_modes*1024<<",\"invalid\":"<<depth_bad<<",\"max_error\":"<<depth_error<<",\"scope\":\"Owned native/HDR read SRVs versus original native texture sampling, both swizzles and CPU-invalidation fallback; retained resources through GPU fence. No game depth or scene draw proven.\"}\n";
 REXLOG_INFO("Scene depth views: {} owned SRVs, invalid{}, max error{}",f->depth_views.size(),depth_bad,depth_error);if(depth_bad || !depth_report.good())return false;
 REXLOG_INFO("Native HDR texture bindings: {} modes, identity/custom swizzle, HDR and CPU-written legacy fallback; invalid {}, max error {}",f->sampling_modes,sampling_bad,sampling_error);
 std::ofstream sampling_report(directory/"hdr-texture-results.json");sampling_report<<"{\n\"modes\":"<<f->sampling_modes<<",\n\"components\":"<<f->sampling_modes*1024<<",\n\"invalid\":"<<sampling_bad<<",\n\"max_error\":"<<sampling_error<<"\n}\n";
 unsigned swap_bad=0;double swap_error=0;for(unsigned output=0;output<2;++output)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)for(unsigned c=0;c<4;++c){const double white=output?400:200,magnitude=30*white,room=1000-white,delta=magnitude-white,mapped_white=white+room*(delta/(room+delta));const double input[]={7.5,15,30};const double expected=c==3?1:input[c]*white*(mapped_white/magnitude)/80;uint16_t bits;std::memcpy(&bits,mapped+(51+output)*stride+y*256+x*8+c*2,2);const double actual=Half(bits),error=std::abs(actual-expected);swap_error=std::max(swap_error,error);swap_bad+=!std::isfinite(actual) || error>.0007*std::max(1.,std::abs(expected))+1e-5;}
 std::ofstream swap_report(directory/"hdr-swap-results.json");swap_report<<"{\n\"components\":512,\n\"invalid\":"<<swap_bad<<",\n\"max_error\":"<<swap_error<<",\n\"fetch_queries\":"<<f->swap_queries<<",\n\"rejection_guards\":"<<f->swap_guards<<",\n\"scope\":\"native fetch selection and deferred output pass from authoritative native resolve; synthetic scene, no full IssueSwap/presentation or title\"\n}\n";REXLOG_INFO("HDR native swap source/output: queries {}, guards {}, invalid {}, max error {}; no full presentation",f->swap_queries,f->swap_guards,swap_bad,swap_error);
 {rex::thread::global_critical_region region;auto lock=region.Acquire();if(FindHdrSceneResolve(f->native.get(),f->sampling_info))return false;ShutdownHdrSceneResolves(f->native.get());for(unsigned mode=0;mode<f->sampling_modes;++mode){rex::graphics::d3d12::D3D12Shader::TextureBinding binding{};binding.dimension=rex::graphics::xenos::FetchOpDimension::k2D;if(f->sampling_caches[mode]->AreActiveTextureSRVKeysUpToDate(&f->sampling_keys[mode],&binding,1))return false;}}

 const auto ownership_draws=f->registration->stats().transfers;
 REXLOG_INFO("Full native HDR ownership transfers: {} mirrored draws, {} bad HDR components, {} legacy pixel differences, max error {}",ownership_draws,ownership_bad,ownership_legacy_differences,ownership_error);
 std::ofstream ownership_report(directory/"hdr-ownership-results.json");ownership_report<<"{\n\"mirrored_draws\":"<<ownership_draws<<",\n\"invalid_hdr_components\":"<<ownership_bad
  <<",\n\"legacy_pixel_differences\":"<<ownership_legacy_differences<<",\n\"max_error\":"<<ownership_error<<",\n\"scope\":\"actual nonempty native transfer routine and deferred Execute, source pitch2/base0 to destination pitch1/base1, tile1 address, fixture-owned96x32 source and8x8 destination1x/4xMSAA\"\n}\n";
 std::ofstream pixels(directory/"native-capture-readback.bin",std::ios::binary);
 pixels.write(reinterpret_cast<const char*>(mapped),130*stride);pixels.close();
 f->readback->Unmap(0,&none);unsigned errors_count=0;
 if(validation)for(uint64_t i=first;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes=0;validation->GetMessage(i,nullptr,&bytes);std::vector<uint8_t> data(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());if(SUCCEEDED(validation->GetMessage(i,message,&bytes)) && message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors_count;REXLOG_ERROR("HDR export GPU: {}",message->pDescription);}}
 REXLOG_INFO("HDR native cache-bound sidecar: 8 draws, 1x/4xMSAA, {} legacy UNORM pixel differences, {} invalid FP16 components, max reference error {}, {} device errors; raw4/2 highlights and alpha blending, unchanged legacy output, strict unsupported/repeated/truncated rejection; no title integration",ldr_differences,bad,max_error,errors_count);
 std::ofstream report(directory/"native-capture-results.json");
 report<<"{\n\"draws\":8,\n\"legacy_pixels_compared\":256,\n\"legacy_pixel_differences\":"<<ldr_differences
  <<",\n\"hdr_components_checked\":1024,\n\"invalid_hdr_components\":"<<bad<<",\n\"max_hdr_error\":"<<max_error
  <<",\n\"device_errors\":"<<errors_count<<",\n\"source\":\"native-translated guest ALU export; synthetic constants, not live EM2 lighting\",\n\"scope\":\"1x/4x MSAA and hardware resolve, owned target/pipeline descriptors, opaque/alpha blend; native cache binding/deferred execution with fixture-owned8x8 native target objects; no live ownership/clear/resolve association or title\"\n}\n";
 const auto bindings=f->registration->stats().bindings;
 const auto clears=f->registration->stats().clears;
 REXLOG_INFO("HDR native framebuffer bindings: {}, expected14; zero-budget typed-null fallback allocation guard passed",bindings);
 REXLOG_INFO("HDR native partial resolve clears: {}, expected15; native resolve clear plus cleared3x4 rectangle and unchanged surrounding HDR checked",clears);
 if(!em2::TestDeferredSceneReplay(cp,f->depth_views.back(),directory/"deferred-replay") || !em2::TestSceneAmbientComposition(cp,directory/"ambient-composition"))return false;
 complete=!material_resolve_bad && material_resolve_report.good() && !material_bad && material_transfers==4 && material_report.good() && !swap_bad && f->swap_queries==f->sampling_modes*4 && f->swap_guards==6 && swap_report.good() && !ldr_differences && !bad && !errors_count && bindings==14 && clears==15 && !transfer_bad && !ownership_bad && !ownership_legacy_differences && ownership_draws==4 && !resolve_bad && !sample_bad && !sampling_bad && sampling_report.good() && resolve_report.good() && ownership_report.good() && transfer_report.good() && report.good() && pixels.good();return complete;
}

#include "hdr_presenter.h"
#include "hdr_output.h"
#include "gpu_constants_internal.h"
#include "native_pixel_fixture.h"
#include "hdr_scene_cache.h"
#include "hdr_scene_target.h"
#include "hdr_scene_resolve.h"
#include "material_capture.h"
#include <rex/cvar.h>
REXCVAR_DECLARE(bool,em2_apply_scene_lighting);
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/d3d12/shared_memory.h>
#include <rex/graphics/d3d12/texture_cache.h>
#include <rex/graphics/d3d12/render_target_cache.h>
#include <rex/graphics/trace_writer.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <d3d12sdklayers.h>
#include <array>
#include <cstring>
#include <fstream>
#include <vector>
#include <cmath>
namespace {
using CP=rex::graphics::d3d12::D3D12CommandProcessor;
using Base=rex::graphics::CommandProcessor;
using P=rex::ui::d3d12::D3D12Presenter;
using G=rex::graphics::GraphicsSystem;
using E=em2::Em2CommandProcessor;
using RT=rex::graphics::d3d12::D3D12RenderTargetCache;
using RTBase=rex::graphics::RenderTargetCache;
using namespace em2::pixel_test;
template<class Tag,auto Member>struct Access{friend auto MemberOf(Tag){return Member;}};
template<class Tag,class Type>struct TypeAccess{friend auto TypeOf(Tag){return std::type_identity<Type>{};}};
struct HostTag{friend auto TypeOf(HostTag);};template struct TypeAccess<HostTag,RT::D3D12RenderTarget>;
struct KeyTag{friend auto TypeOf(KeyTag);};template struct TypeAccess<KeyTag,RTBase::RenderTargetKey>;
using Host=decltype(TypeOf(HostTag{}))::type;
using Key=decltype(TypeOf(KeyTag{}))::type;
struct TargetsTag{friend auto MemberOf(TargetsTag);};template struct Access<TargetsTag,&RTBase::render_targets_>;
struct OwnTag{friend auto MemberOf(OwnTag);};template struct Access<OwnTag,&RTBase::ChangeOwnership>;
struct SourceGammaTag{friend auto MemberOf(SourceGammaTag);};template struct Access<SourceGammaTag,&RT::gamma_render_target_as_srgb_>;
struct TextureTag{friend auto MemberOf(TextureTag);};template struct Access<TextureTag,&CP::texture_cache_>;
struct BeginTag{friend auto MemberOf(BeginTag);};template struct Access<BeginTag,&CP::BeginSubmission>;
struct GraphicsTag{friend auto MemberOf(GraphicsTag);};template struct Access<GraphicsTag,&Base::graphics_system_>;
struct MemoryTag{friend auto MemberOf(MemoryTag);};template struct Access<MemoryTag,&Base::memory_>;
struct PresenterTag{friend auto MemberOf(PresenterTag);};template struct Access<PresenterTag,&G::presenter_>;
struct SharedTag{friend auto MemberOf(SharedTag);};template struct Access<SharedTag,&CP::shared_memory_>;
struct IssueTag{friend auto MemberOf(IssueTag);};template struct Access<IssueTag,&E::IssueSwap>;
struct HdrTag{friend auto MemberOf(HdrTag);};template struct Access<HdrTag,&E::hdr_swap_>;
struct GammaTag{friend auto MemberOf(GammaTag);};template struct Access<GammaTag,&Base::gamma_ramp_rw_component_>;
struct GammaTableTag{friend auto MemberOf(GammaTableTag);};template struct Access<GammaTableTag,&Base::gamma_ramp_256_entry_table>;
struct GammaPwlTag{friend auto MemberOf(GammaPwlTag);};template struct Access<GammaPwlTag,&Base::gamma_ramp_pwl_rgb>;
struct ResourcesTag{friend auto MemberOf(ResourcesTag);};template struct Access<ResourcesTag,&P::guest_output_resources_>;
struct ReadyTag{friend auto MemberOf(ReadyTag);};template struct Access<ReadyTag,&rex::ui::Presenter::guest_output_mailbox_acquired_and_ready_>;
constexpr unsigned side=8,stride=256*side;
struct Probe:Fixture {
 E& cp;G& graphics;
 std::unique_ptr<rex::ui::Presenter> original;
 std::array<std::unique_ptr<rex::ui::Presenter>,2> presenters;
 std::array<ComPtr<ID3D12Resource>,7> mailboxes;
 std::unique_ptr<rex::graphics::RegisterFile> source_registers;
 std::unique_ptr<rex::graphics::TraceWriter> source_trace;
 std::unique_ptr<RT> source_cache;
 std::unique_ptr<em2::HdrSceneCacheRegistration> source_registration;
 std::unique_ptr<Host> source_target;
 std::shared_ptr<em2::HdrSwapState> old_hdr;
 std::array<uint32_t,6> fetch{};
 std::array<rex::graphics::reg::DC_LUT_30_COLOR,256> gamma{};
 std::array<rex::graphics::reg::DC_LUT_PWL_DATA,384> pwl{};
 uint32_t component=0,allocation=0;rex::memory::BaseHeap* heap=nullptr;
 bool installed=false,lost=false;bool old_lighting=REXCVAR_GET(em2_apply_scene_lighting);
 Probe(E& c):cp(c),graphics(*(c.*MemberOf(GraphicsTag{}))){}
 ~Probe(){
  REXCVAR_SET(em2_apply_scene_lighting,old_lighting);
  if(installed){cp.DrainForResourceChange();graphics.*MemberOf(PresenterTag{})=std::move(original);cp.*MemberOf(HdrTag{})=std::move(old_hdr);for(unsigned i=0;i<6;++i)cp.WriteNativeConstant(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0+i,fetch[i]);cp.RestoreGammaRamp(gamma.data(),pwl.data(),component);}
  if(source_cache && source_target)(source_cache.get()->*MemberOf(TargetsTag{})).erase(source_target->key());
  if(heap && allocation)heap->Release(allocation,nullptr);
 }
};
bool PrepareHdrScene(Probe& f,uint32_t physical){
 namespace g=rex::graphics;namespace x=g::xenos;auto& cp=f.cp;auto* memory=cp.*MemberOf(MemoryTag{});auto* shared=(cp.*MemberOf(SharedTag{})).get();auto* texture=(cp.*MemberOf(TextureTag{})).get();auto& provider=cp.GetD3D12Provider();auto* device=provider.GetDevice();
 if(!texture || texture->IsDrawResolutionScaled() || !(cp.*MemberOf(BeginTag{}))(true))return false;
 f.source_registers=std::make_unique<g::RegisterFile>();std::memset(f.source_registers.get(),0,sizeof(g::RegisterFile));f.source_trace=std::make_unique<g::TraceWriter>(memory->physical_membase());f.source_cache=std::make_unique<RT>(*f.source_registers,*memory,*f.source_trace,1,1,cp,false);if(!f.source_cache->Initialize())return false;(f.source_cache.get()->*MemberOf(SourceGammaTag{}))=0;
 if(!em2::RegisterMaterialCapture(f.source_cache.get(),device))return false;f.source_registration=em2::HdrSceneCacheRegistration::Create(*f.source_cache,device,256ull*1024*1024,em2::HdrSceneEncoding::kNativeLinear);if(!f.source_registration)return false;
 D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=96;desc.Height=32;desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
 if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&f.target))))return false;
 auto rtvs=std::make_shared<rex::ui::d3d12::D3D12CpuDescriptorPool>(provider,D3D12_DESCRIPTOR_HEAP_TYPE_RTV,3);auto srvs=std::make_shared<rex::ui::d3d12::D3D12CpuDescriptorPool>(provider,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,3);auto rtv=rtvs->AllocateDescriptor(),srv=srvs->AllocateDescriptor();if(!rtv || !srv)return false;
 device->CreateRenderTargetView(f.target.Get(),nullptr,rtv.GetHandle());D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=desc.Format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;device->CreateShaderResourceView(f.target.Get(),&view,srv.GetHandle());
 Key key;key.pitch_tiles_at_32bpp=1;key.resource_format=0;key.msaa_samples=x::MsaaSamples::k1X;f.source_target=std::make_unique<Host>(key,f.target.Get(),std::move(rtv),decltype(rtv){},decltype(rtv){},std::move(srv),decltype(rtv){},D3D12_RESOURCE_STATE_RENDER_TARGET);(f.source_cache.get()->*MemberOf(TargetsTag{})).emplace(key,f.source_target.get());
 auto& list=cp.GetDeferredCommandList();const float legacy[]={1,1,.5,.25},radiance[]={4,2,.5,.25};list.D3DClearRenderTargetView(f.source_target->descriptor_draw().GetHandle(),legacy,0,nullptr);
 auto binding=em2::AcquireHdrSceneBinding(f.source_cache.get(),f.target.Get(),&list);if(!binding.target || !binding.target->Clear(list,radiance))return false;
 const float vertices[]={-.5f,-.5f,7.5f,-.5f,-.5f,7.5f};std::memcpy(memory->TranslatePhysical(physical+65536),vertices,sizeof(vertices));shared->MemoryInvalidationCallback(physical+65536,4096,true);auto& regs=*f.source_registers;
 x::xe_gpu_vertex_fetch_t vertex{};vertex.type=x::FetchConstantType::kVertex;vertex.size=6;vertex.address=(physical+65536)/4;std::memcpy(&regs.values[g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],&vertex,sizeof(vertex));auto set=[&]<class T>(T value){regs.values[T::register_index]=value.value;};
 g::reg::PA_SC_SCREEN_SCISSOR_BR screen{};screen.br_x=80;screen.br_y=32;set(screen);g::reg::PA_SC_WINDOW_SCISSOR_TL top{};top.window_offset_disable=1;set(top);g::reg::PA_SC_WINDOW_SCISSOR_BR window{};window.br_x=80;window.br_y=32;set(window);g::reg::RB_SURFACE_INFO surface{};surface.surface_pitch=80;surface.msaa_samples=x::MsaaSamples::k1X;set(surface);g::reg::RB_COLOR_INFO color{};color.color_format=x::ColorRenderTargetFormat::k_8_8_8_8;set(color);
 g::reg::RB_COPY_CONTROL control{};control.copy_command=x::CopyCommand::kRaw;control.copy_sample_select=x::CopySampleSelect::k0;set(control);g::reg::RB_COPY_DEST_INFO destination{};destination.copy_dest_format=x::ColorFormat(x::TextureFormat::k_8_8_8_8);set(destination);g::reg::RB_COPY_DEST_PITCH pitch{};pitch.copy_dest_pitch=pitch.copy_dest_height=32;set(pitch);regs.values[g::XE_GPU_REG_RB_COPY_DEST_BASE]=physical;
 (f.source_cache.get()->*MemberOf(OwnTag{}))(key,0,1,nullptr,nullptr);uint32_t written=0,length=0;if(!f.source_cache->Resolve(*memory,*shared,*texture,written,length) || !length)return false;
 rex::thread::global_critical_region region;auto lock=region.Acquire();x::xe_gpu_texture_fetch_t fetch;std::memcpy(&fetch,cp.ConstantRegisterValues()+g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0,sizeof(fetch));return bool(em2::FindHdrSwapScene(texture,fetch));
}
}
bool em2::TestHdrNativeSwap(E& cp,const std::filesystem::path& directory){
 using namespace em2::pixel_test;namespace g=rex::graphics;namespace x=g::xenos;
 if(!cp.IsGraphicsWorker() || !cp.DrainForResourceChange())return false;
 auto f=std::make_unique<Probe>(cp);auto* device=cp.GetD3D12Provider().GetDevice();auto* queue=cp.GetD3D12Provider().GetDirectQueue();
 ComPtr<ID3D12InfoQueue> validation;device->QueryInterface(IID_PPV_ARGS(&validation));const uint64_t first=validation?validation->GetNumStoredMessagesAllowedByRetrievalFilter():0;
 auto loss=[probe=f.get()](bool,bool){probe->lost=true;};f->presenters[0]=P::Create(loss,cp.GetD3D12Provider());f->presenters[1]=CreateHdrPresenter(loss,cp.GetD3D12Provider());if(!f->presenters[0] || !f->presenters[1])return false;
 auto* memory=cp.*MemberOf(MemoryTag{});f->heap=memory->LookupHeapByType(true,65536);
 if(!f->heap || !f->heap->Alloc(2*65536,65536,rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite,false,&f->allocation))return false;
 const uint32_t physical=f->allocation&0x1fffffff;auto* data=static_cast<uint8_t*>(memory->TranslatePhysical(physical));for(unsigned i=0;i<65536;i+=4){data[i]=64;data[i+1]=128;data[i+2]=192;data[i+3]=255;}
 (cp.*MemberOf(SharedTag{}))->MemoryInvalidationCallback(physical,65536,true);
 if(!f->event || !Buffer(device,7*stride,D3D12_HEAP_TYPE_READBACK,&f->readback) || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f->allocator))) || FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f->allocator.Get(),nullptr,IID_PPV_ARGS(&f->list))) || FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f->fence))))return false;
 std::memcpy(f->fetch.data(),cp.ConstantRegisterValues()+g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0,sizeof(f->fetch));std::memcpy(f->gamma.data(),(cp.*MemberOf(GammaTableTag{}))(),sizeof(f->gamma));std::memcpy(f->pwl.data(),(cp.*MemberOf(GammaPwlTag{}))(),sizeof(f->pwl));f->component=cp.*MemberOf(GammaTag{});
 f->original=std::move(f->graphics.*MemberOf(PresenterTag{}));f->old_hdr=std::move(cp.*MemberOf(HdrTag{}));f->installed=true;
 auto identity=f->gamma;for(unsigned i=0;i<256;++i){const uint32_t v=(i*1023+127)/255;identity[i].color_10_red=identity[i].color_10_green=identity[i].color_10_blue=v;}cp.RestoreGammaRamp(identity.data(),f->pwl.data(),0);
 x::xe_gpu_texture_fetch_t fetch{};fetch.type=x::FetchConstantType::kTexture;fetch.dimension=x::DataDimension::k2DOrStacked;fetch.tiled=1;fetch.pitch=1;fetch.base_address=physical>>12;fetch.format=x::TextureFormat::k_8_8_8_8;fetch.size_2d.width=fetch.size_2d.height=side-1;fetch.swizzle=0x688;
 std::array<uint32_t,6> words;std::memcpy(words.data(),&fetch,sizeof(fetch));for(unsigned i=0;i<6;++i)cp.WriteNativeConstant(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0+i,words[i]);
 const auto start_frame=cp.GetCurrentFrame();unsigned checked_frames=0;
 for(unsigned mode=0;mode<7;++mode){
  REXLOG_INFO("Native swap probe mode{} begin",mode);
  if(mode==3 && !PrepareHdrScene(*f,physical)){REXLOG_ERROR("Native HDR swap scene preparation failed");return false;}
  if(mode==4){fetch.swizzle=0xa88;std::memcpy(words.data(),&fetch,sizeof(fetch));for(unsigned i=0;i<6;++i)cp.WriteNativeConstant(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0+i,words[i]);}
  if(mode>=5)REXCVAR_SET(em2_apply_scene_lighting,true);
  if(mode==6){auto inverse=identity;for(unsigned i=0;i<256;++i){const uint32_t v=1023-(i*1023+127)/255;inverse[i].color_10_red=inverse[i].color_10_green=inverse[i].color_10_blue=v;}cp.RestoreGammaRamp(inverse.data(),f->pwl.data(),0);}
  const unsigned presenter_index=mode>=5?0:mode?1:0;f->graphics.*MemberOf(PresenterTag{})=std::move(f->presenters[presenter_index]);auto* presenter=static_cast<P*>(f->graphics.presenter());
  const float white=mode==2 || mode==4?400:200;if(mode>=2 && !cp.SetHdrSwapSettings({1,white,1000}))return false;
  const auto frame=cp.GetCurrentFrame(),submission=cp.GetCurrentSubmission();(cp.*MemberOf(IssueTag{}))(physical,side,side);
  if(validation)for(uint64_t i=first;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;validation->GetMessage(i,nullptr,&size);std::vector<uint8_t> storage(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(storage.data());if(SUCCEEDED(validation->GetMessage(i,m,&size)) && m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)REXLOG_ERROR("Swap mode{} device validation: {}",mode,m->pDescription);}
  REXLOG_INFO("Native swap probe mode{} returned, device result {}",mode,int32_t(device->GetDeviceRemovedReason()));
  if(cp.GetCurrentFrame()!=frame+1 || cp.GetCurrentSubmission()<=submission || IsHdrLegacyRefresh() || (mode>=2 && mode<5 && HdrPresenterPaperWhite(presenter)!=white))return false;++checked_frames;
  const auto ready=((presenter->*MemberOf(ReadyTag{})).load(std::memory_order_acquire)>>2)&3;if(ready>=3)return false;f->mailboxes[mode]=(presenter->*MemberOf(ResourcesTag{}))[ready].second;
  auto* image=f->mailboxes[mode].Get();if(!image || image->GetDesc().Format!=(mode>=3 && mode<5?kHdrOutputFormat:P::kGuestOutputFormat) || image->GetDesc().Width!=side || image->GetDesc().Height!=side)return false;
  Barrier(f->list.Get(),image,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=image;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=f->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=mode*stride;to.PlacedFootprint.Footprint={mode>=3 && mode<5?kHdrOutputFormat:P::kGuestOutputFormat,side,side,1,256};f->list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(f->list.Get(),image,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  if(FAILED(f->list->Close()))return false;ID3D12CommandList* lists[]={f->list.Get()};queue->ExecuteCommandLists(1,lists);
  if(FAILED(queue->Signal(f->fence.Get(),mode+1)) || FAILED(f->fence->SetEventOnCompletion(mode+1,f->event)) || WaitForSingleObject(f->event,15000)!=WAIT_OBJECT_0){f.release();return false;}
  f->presenters[presenter_index]=std::move(f->graphics.*MemberOf(PresenterTag{}));if(mode<6 && (FAILED(f->allocator->Reset()) || FAILED(f->list->Reset(f->allocator.Get(),nullptr))))return false;
 }
 if(!cp.DrainForResourceChange())return false;const uint8_t* pixels=nullptr;D3D12_RANGE read{0,7*stride};if(FAILED(f->readback->Map(0,&read,reinterpret_cast<void**>(const_cast<uint8_t**>(&pixels)))))return false;
 unsigned differences=0,nonzero=0;for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x){uint32_t baseline;std::memcpy(&baseline,pixels+y*256+x*4,4);nonzero+=(baseline&0x3fffffff)!=0;for(unsigned mode=1;mode<3;++mode)differences+=std::memcmp(pixels+y*256+x*4,pixels+mode*stride+y*256+x*4,4)!=0;}
 unsigned hdr_bad=0,highlights=0;double hdr_error=0;
 for(unsigned mode=3;mode<5;++mode)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)for(unsigned c=0;c<4;++c){const double white=mode==3?200:400,magnitude=4*white,room=1000-white,delta=magnitude-white,mapped=white+room*(delta/(room+delta));const double radiance[]={4,2,.5};const double expected=c==3?1:radiance[c]*white*(mapped/magnitude)/80;uint16_t bits;std::memcpy(&bits,pixels+mode*stride+y*256+x*8+c*2,2);const unsigned exponent=(bits>>10)&31,mantissa=bits&1023;const double actual=(bits&32768?-1:1)*(exponent==31?INFINITY:exponent?std::ldexp(double(1024+mantissa),int(exponent)-25):std::ldexp(double(mantissa),-24));const double error=std::abs(actual-expected);hdr_error=std::max(hdr_error,error);hdr_bad+=!std::isfinite(actual) || error>.0007*std::max(1.,std::abs(expected))+1e-5;if(c<3)highlights+=actual>1;}
 unsigned sdr_bad=0;for(unsigned mode=5;mode<7;++mode)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x){uint32_t packed;std::memcpy(&packed,pixels+mode*stride+y*256+x*4,4);const uint32_t blue=(128*1023+127)/255;const uint32_t expected=mode==5?(1023u|(1023u<<10)|(blue<<20)|(3u<<30)):((1023-blue)<<20)|(3u<<30);sdr_bad+=packed!=expected;}
 std::filesystem::create_directories(directory);std::ofstream raw(directory/"native-swap-readback.bin",std::ios::binary);raw.write(reinterpret_cast<const char*>(pixels),7*stride);D3D12_RANGE none{0,0};f->readback->Unmap(0,&none);
 unsigned errors=0;if(validation)for(uint64_t i=first;i<validation->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;validation->GetMessage(i,nullptr,&size);std::vector<uint8_t> bytes(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());if(SUCCEEDED(validation->GetMessage(i,m,&size)) && m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;REXLOG_ERROR("HDR native swap: {}",m->pDescription);}}
 std::ofstream report(directory/"native-swap-results.json");report<<"{\n\"sdr_modified_pixels\":128,\n\"sdr_invalid_pixels\":"<<sdr_bad<<",\n\"hdr_components\":512,\n\"invalid_hdr_components\":"<<hdr_bad<<",\n\"max_hdr_error\":"<<hdr_error<<",\n\"highlight_components\":"<<highlights<<",\n\"real_swap_calls\":"<<checked_frames<<",\n\"frame_delta\":"<<cp.GetCurrentFrame()-start_frame<<",\n\"fallback_pixel_comparisons\":128,\n\"differences\":"<<differences<<",\n\"nonzero_baseline_pixels\":"<<nonzero<<",\n\"device_errors\":"<<errors<<",\n\"scope\":\"actual virtual IssueSwap/native gamma/public native RefreshGuestOutput and submission fences; SDR baseline, native gamma fallback, actual native authoritative scene Resolve and HDR gamma bypass at200/400nits; public refresh/submission/mailbox; no surface/Present/title; synthetic fixture source; modified native-linear scene also reaches SDR through native table gamma at identity and inverted ramps, exact 10-bit pixels and unsigned HDR clipping\"\n}\n";
 REXLOG_INFO("Native full swap: {} calls/{} frames, SDR/HDR-fallback pixel differences {}, nonzero {}, device errors {}; no surface/Present/title",checked_frames,cp.GetCurrentFrame()-start_frame,differences,nonzero,errors);
 REXLOG_INFO("Native positive HDR swap:512 components, invalid {}, max error {}, highlights {}; authored scene fixture4/2/.5 before legacy8-bit packing",hdr_bad,hdr_error,highlights);
 return !f->lost && !hdr_bad && highlights && !sdr_bad && checked_frames==7 && !differences && nonzero==64 && !errors && raw.good() && report.good();
}

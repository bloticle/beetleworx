#include "material_capture.h"
#include "material_ambient_export.h"
#include "hdr_scene_target.h"
#include "hdr_scene_resolve.h"
#include "hdr_scene_cache.h"
#include "../private/material_shader_data.h"
#include "../private/scene_shader_data.h"
#include <rex/graphics/d3d12/pipeline_cache.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/render_target_cache.h>
#include <rex/graphics/d3d12/deferred_command_list.h>
#include <rex/logging.h>
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <thread>
#include <unordered_set>
namespace em2 {
struct MaterialCaptureState{
    std::thread::id owner=std::this_thread::get_id();
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> nulls;
    std::atomic<uint64_t> translations=0,replacements=0,failures=0,bindings=0,fallbacks=0,prepares=0,pending=0,transfers=0,clear_pipelines=0,unsafe_draws=0,surface_transfers=0;
    uint64_t token=0;
    bool draw=false,surface_draw=false,surfaces=false,linear_surfaces=false,surface_disabled=false,capture_disabled=false;std::unordered_set<ID3D12Resource*> surface_invalid;std::unordered_set<ID3D12Resource*> invalid;std::atomic<uint64_t> used=0;uint64_t budget=0;
    struct Entry{std::shared_ptr<HdrSceneTarget> target,surface;};
    std::unordered_map<ID3D12Resource*,Entry> targets;
    std::mutex programs_mutex;
    struct Program{std::vector<uint8_t> replacement;std::shared_ptr<const std::vector<uint8_t>> original;bool surface=false;};
    std::vector<Program> programs;
    std::vector<std::vector<uint8_t>> surface_preserving;
    std::shared_ptr<MaterialCaptureTestGate> test_gate;
};
}
namespace {
using Cache=rex::graphics::d3d12::D3D12RenderTargetCache;
using Translator=rex::graphics::DxbcShaderTranslator;
std::mutex mutex;
std::unordered_map<const Cache*,std::shared_ptr<em2::MaterialCaptureState>> states;
std::unordered_set<const Cache*> started;
std::atomic<bool> active=false;
std::atomic<uint64_t> next_token=1;
constexpr GUID capture_guid{0x6b0565df,0x3cab,0x4877,{0xa1,0x71,0x8f,0x7c,0x02,0x21,0x98,0x44}};
constexpr GUID surface_guid{0x178574b9,0x21e6,0x41d8,{0xb6,0x1c,0x0a,0x51,0x73,0xc9,0x62,0x5d}};
constexpr GUID inert_guid{0x7292cd1c,0x725a,0x4ba6,{0xae,0x31,0x6f,0x89,0xa7,0x4d,0x19,0x6a}};
thread_local std::shared_ptr<em2::MaterialCaptureState> current;
std::shared_ptr<em2::MaterialCaptureState> Find(const Cache* cache){if(!active.load(std::memory_order_acquire))return {};std::lock_guard lock(mutex);auto it=states.find(cache);return it==states.end()?nullptr:it->second;}
std::shared_ptr<em2::MaterialCaptureState> BeginTranslation(const Cache* cache){std::lock_guard lock(mutex);started.insert(cache);auto it=states.find(cache);return it==states.end()?nullptr:it->second;}
std::span<const uint32_t> Replacement(const rex::graphics::Shader* shader,bool surfaces){
    if(!shader || shader->type()!=rex::graphics::xenos::ShaderType::kPixel)return {};
    const auto& code=shader->ucode_data();
    for(const std::span<const uint32_t> original:{std::span<const uint32_t>(em2::material_shader_data::Gen3_Char_Oswald),std::span<const uint32_t>(em2::material_shader_data::Gen3_Char_Mickey)})
        if(code.size()==original.size() && std::equal(code.begin(),code.end(),original.begin()))return em2::material_shader_data::full_suffix_difference;
    const auto& world=em2::material_shader_data::Gen3_StaticEnv_DL;
    if(code.size()==world.size() && std::equal(code.begin(),code.end(),world.begin()))return em2::material_shader_data::world_baked_difference;
    const auto& base=em2::material_shader_data::Gen3_StaticEnv;
    if(code.size()==base.size() && std::equal(code.begin(),code.end(),base.begin()))return surfaces?std::span<const uint32_t>(em2::material_shader_data::world_base_combined):std::span<const uint32_t>(em2::material_shader_data::world_base_baked_difference);
    return {};
}
template<class Tag,auto Member>struct Access{friend auto MemberOf(Tag){return Member;}};
#define FIELD(tag,field) struct tag{friend auto MemberOf(tag);};template struct Access<tag,&Translator::field>
FIELD(Vendor,vendor_id_);FIELD(Bindless,bindless_resources_used_);FIELD(Rov,edram_rov_used_);FIELD(Gamma,gamma_render_target_as_srgb_);FIELD(Msaa,msaa_2x_supported_);FIELD(ScaleX,draw_resolution_scale_x_);FIELD(ScaleY,draw_resolution_scale_y_);
#undef FIELD
using CP=rex::graphics::d3d12::D3D12CommandProcessor;
template<class Tag,class T>struct TypeAccess{friend auto TypeOf(Tag){return std::type_identity<T>{};}};
struct PipelineType{friend auto TypeOf(PipelineType);};template struct TypeAccess<PipelineType,rex::graphics::d3d12::PipelineCache::Pipeline>;
struct TargetType{friend auto TypeOf(TargetType);};template struct TypeAccess<TargetType,rex::graphics::RenderTargetCache::RenderTarget>;
struct HostType{friend auto TypeOf(HostType);};template struct TypeAccess<HostType,Cache::D3D12RenderTarget>;
using Host=decltype(TypeOf(HostType{}))::type;
using NativePipeline=decltype(TypeOf(PipelineType{}))::type;using Target=decltype(TypeOf(TargetType{}))::type;
struct CpCache{friend auto MemberOf(CpCache);};template struct Access<CpCache,&CP::render_target_cache_>;
struct CpPipelines{friend auto MemberOf(CpPipelines);};template struct Access<CpPipelines,&CP::pipeline_cache_>;
struct GuestPipeline{friend auto MemberOf(GuestPipeline);};template struct Access<GuestPipeline,&CP::current_guest_pipeline_>;
struct CurrentTargets{friend auto MemberOf(CurrentTargets);};template struct Access<CurrentTargets,&Cache::current_command_list_render_targets_>;
struct FramebufferValid{friend auto MemberOf(FramebufferValid);};template struct Access<FramebufferValid,&Cache::are_current_command_list_render_targets_valid_>;
struct BindTargets{friend auto MemberOf(BindTargets);};template struct Access<BindTargets,&Cache::SetCommandListRenderTargets>;
struct Binary{friend auto MemberOf(Binary);};template struct Access<Binary,&rex::graphics::Shader::Translation::translated_binary_>;
bool Compatible(const rex::graphics::DxbcShader& a,const rex::graphics::DxbcShader& b){
    for(unsigned c=0;c<256;++c)if(a.constant_register_map().GetPackedFloatConstantIndex(c)!=b.constant_register_map().GetPackedFloatConstantIndex(c))return false;
    const auto& at=a.GetTextureBindingsAfterTranslation();const auto& bt=b.GetTextureBindingsAfterTranslation();if(at.size()!=bt.size() || std::memcmp(at.data(),bt.data(),at.size()*sizeof(at[0])))return false;
    const auto& as=a.GetSamplerBindingsAfterTranslation();const auto& bs=b.GetSamplerBindingsAfterTranslation();if(as.size()!=bs.size())return false;
    for(unsigned i=0;i<as.size();++i){const auto& x=as[i];const auto& y=bs[i];if(x.bindless_descriptor_index!=y.bindless_descriptor_index || x.fetch_constant!=y.fetch_constant || x.mag_filter!=y.mag_filter || x.min_filter!=y.min_filter || x.mip_filter!=y.mip_filter || x.aniso_filter!=y.aniso_filter)return false;}
    return true;
}
}
bool em2::RegisterMaterialCapture(const Cache* cache,ID3D12Device* device,uint64_t budget,bool surfaces,bool linear_surfaces){
    if(!cache || !device || (linear_surfaces && !surfaces))return false;auto state=std::make_shared<MaterialCaptureState>();state->device=device;state->budget=budget;state->surfaces=surfaces;state->linear_surfaces=linear_surfaces;state->token=next_token.fetch_add(1);
    const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,2,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&state->nulls))))return false;
    auto handle=state->nulls->GetCPUDescriptorHandleForHeapStart();for(unsigned i=0;i<2;++i){D3D12_RENDER_TARGET_VIEW_DESC view{};view.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;view.ViewDimension=i?D3D12_RTV_DIMENSION_TEXTURE2DMS:D3D12_RTV_DIMENSION_TEXTURE2D;device->CreateRenderTargetView(nullptr,&view,handle);handle.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);}
    std::lock_guard lock(mutex);if(started.contains(cache))return false;const bool added=states.emplace(cache,std::move(state)).second;if(added)active.store(true,std::memory_order_release);return added;
}
void em2::ShutdownMaterialCapture(const Cache* cache){ShutdownMaterialSceneResolves(cache);std::lock_guard lock(mutex);states.erase(cache);started.erase(cache);active.store(!states.empty(),std::memory_order_release);}
em2::MaterialCaptureStats em2::GetMaterialCaptureStats(const Cache* cache){auto s=Find(cache);if(!s)return {};return {s->translations.load(),s->replacements.load(),s->failures.load(),s->bindings.load(),s->used.load(),s->fallbacks.load(),s->prepares.load(),s->pending.load(),s->transfers.load(),s->clear_pipelines.load(),s->unsafe_draws.load(),s->surface_transfers.load()};}
bool em2::SetMaterialCaptureDraw(Cache* cache,ID3D12PipelineState* pipeline){auto s=Find(cache);if(!s)return false;if(s->owner!=std::this_thread::get_id())throw std::logic_error("Material binding owner mismatch");uint64_t token=0;UINT size=sizeof(token);const bool draw=pipeline && SUCCEEDED(pipeline->GetPrivateData(capture_guid,&size,&token)) && size==sizeof(token) && token==s->token;size=sizeof(token);const bool surface=s->surfaces && pipeline && SUCCEEDED(pipeline->GetPrivateData(surface_guid,&size,&token)) && size==sizeof(token) && token==s->token;const bool changed=s->draw!=draw || s->surface_draw!=surface;s->draw=draw;s->surface_draw=surface;if(changed)cache->InvalidateCommandListRenderTargets();return changed;}
void em2::BeforeMaterialCaptureDraw(CP& cp){
    auto* cache=(cp.*MemberOf(CpCache{})).get();auto state=Find(cache);if(!state || cache->GetPath()!=rex::graphics::RenderTargetCache::Path::kHostRenderTargets)return;
    // Internal transfer/replay draws install external pipelines. Never replace
    // their explicit destination bindings with a guest framebuffer.
    const auto* pipeline=static_cast<const NativePipeline*>(cp.*MemberOf(GuestPipeline{}));if(!pipeline)return;
    auto* pipelines=(cp.*MemberOf(CpPipelines{})).get();
    // Use the SDK's own completion/drain protocol before reading its plain PSO
    // pointer. Cache hits keep recording without waiting; this path is opt-in.
    if(pipelines && pipelines->IsCreatingPipelines()){
        ++state->pending;std::shared_ptr<MaterialCaptureTestGate> gate;{std::lock_guard lock(state->programs_mutex);gate=state->test_gate;}if(gate)gate->waiting.release();
        pipelines->EndSubmission();if(pipelines->IsCreatingPipelines())return;
    }
    if(!pipeline->state)return;
    if(state->owner!=std::this_thread::get_id())throw std::logic_error("Material pre-draw owner mismatch");
    ++state->prepares;
    const bool changed=SetMaterialCaptureDraw(cache,pipeline->state);
    // The pinned SDK stores these five pointer-sized framebuffer slots by
    // memcpy despite a pointer-to-pointer declaration in its installed header.
    Target* targets[5]{};static_assert(sizeof(targets)==sizeof(cache->*MemberOf(CurrentTargets{})));
    std::memcpy(targets,cache->*MemberOf(CurrentTargets{}),sizeof(targets));
    // Private exports describe colour0 only. A surface previously used as
    // colour0 can later be written through another MRT slot with the same
    // native resource key. Those writes cannot preserve its old sidecars/view.
    for(unsigned i=2;i<5;++i)if(targets[i]){auto* resource=static_cast<Host*>(targets[i])->resource();InvalidateMaterialCaptureDestination(cache,resource);InvalidateHdrSceneDestination(cache,resource);}
    if(targets[1]){uint64_t token=0;UINT size=sizeof(token);const bool inert=SUCCEEDED(pipeline->state->GetPrivateData(inert_guid,&size,&token)) && size==sizeof(token) && token==state->token;if(!inert){auto* source=static_cast<Host*>(targets[1])->resource();InvalidateHdrSceneView(cache,source);if(state->surfaces && !state->surface_draw)InvalidateMaterialSurface(cache,source);}}
    if(!state->draw && targets[1]){uint64_t token=0;UINT size=sizeof(token);const bool inert=SUCCEEDED(pipeline->state->GetPrivateData(inert_guid,&size,&token)) && size==sizeof(token) && token==state->token;if(!inert){++state->unsafe_draws;InvalidateMaterialCaptureDestination(cache,static_cast<Host*>(targets[1])->resource());if(GetHdrSceneEncoding(cache)==HdrSceneEncoding::kNativeLinear)InvalidateHdrSceneDestination(cache,static_cast<Host*>(targets[1])->resource());}}
    if(changed || !(cache->*MemberOf(FramebufferValid{})))(cache->*MemberOf(BindTargets{}))(targets);
}
namespace {
em2::MaterialCaptureBinding AcquireMaterialTarget(const std::shared_ptr<em2::MaterialCaptureState>& s,ID3D12Resource* source,rex::graphics::d3d12::DeferredCommandList& list){
    if(!s || !source)return {};if(s->owner!=std::this_thread::get_id())throw std::logic_error("Material binding owner mismatch");
    const auto desc=source->GetDesc();em2::MaterialCaptureBinding result{true,s->nulls->GetCPUDescriptorHandleForHeapStart(),{}};if(desc.SampleDesc.Count>1)result.rtv.ptr+=s->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);if(s->capture_disabled || s->invalid.contains(source))return result;
    auto it=s->targets.find(source);if(it==s->targets.end()){
        auto auxiliary=desc;auxiliary.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;auxiliary.Alignment=0;auxiliary.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;auxiliary.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        const auto a=s->device->GetResourceAllocationInfo(0,1,&desc),b=s->device->GetResourceAllocationInfo(0,1,&auxiliary);
        if(a.SizeInBytes==UINT64_MAX || b.SizeInBytes==UINT64_MAX || a.SizeInBytes>s->budget-s->used || b.SizeInBytes>s->budget-s->used-a.SizeInBytes)return result;
        std::shared_ptr<em2::HdrSceneTarget> target=em2::HdrSceneTarget::Create(source);if(!target)return result;
        // Pin all ownership before recording the clear or binding a descriptor.
        it=s->targets.emplace(source,em2::MaterialCaptureState::Entry{std::move(target)}).first;s->used+=a.SizeInBytes+b.SizeInBytes;
        const float zero[4]{};if(!it->second.target->Clear(list,zero))throw std::runtime_error("Material initial clear failed");
    }
    result.target=it->second.target;result.rtv=result.target->rtv();return result;
}
} // namespace
em2::MaterialCaptureBinding em2::AcquireMaterialCapture(const Cache* cache,ID3D12Resource* source,rex::graphics::d3d12::DeferredCommandList& list){auto s=Find(cache);if(!s || !s->draw || !source)return {};++s->bindings;return AcquireMaterialTarget(s,source,list);}
bool em2::HasMaterialCapture(const Cache* cache){return bool(Find(cache));}
void em2::InvalidateMaterialCaptureDestination(const Cache* cache,ID3D12Resource* destination){auto s=Find(cache);if(!s || !destination)return;InvalidateMaterialSurface(cache,destination);if(s->owner!=std::this_thread::get_id())throw std::logic_error("Material invalidation owner mismatch");const bool was_invalid=s->capture_disabled || s->invalid.contains(destination);if(s->invalid.size()>=4096)s->capture_disabled=true;else s->invalid.insert(destination);if(!was_invalid)const_cast<Cache*>(cache)->InvalidateCommandListRenderTargets();}
em2::MaterialCaptureTransfer em2::PrepareMaterialCaptureTransfer(const Cache* cache,ID3D12Resource* source,ID3D12Resource* destination,rex::graphics::d3d12::DeferredCommandList& list){
    auto s=Find(cache);if(!s)return {};if(s->owner!=std::this_thread::get_id())throw std::logic_error("Material transfer owner mismatch");
    auto it=s->targets.find(source);if(source==destination || s->capture_disabled || s->invalid.contains(source) || it==s->targets.end()){InvalidateMaterialCaptureDestination(cache,destination);return {};}
    auto src=it->second.target;auto dst=AcquireMaterialTarget(s,destination,list);if(!dst.target){InvalidateMaterialCaptureDestination(cache,destination);return {};}
    return {std::move(src),std::move(dst.target)};
}
void em2::CountMaterialCaptureTransfer(const Cache* cache){auto s=Find(cache);if(s)++s->transfers;}
void em2::ClearMaterialCapture(const Cache* cache,ID3D12Resource* source,const D3D12_RECT& rect,rex::graphics::d3d12::DeferredCommandList& list){
    auto s=Find(cache);if(!s)return;if(s->owner!=std::this_thread::get_id())throw std::logic_error("Material clear owner mismatch");
    const auto desc=source?source->GetDesc():D3D12_RESOURCE_DESC{};
    const bool full=source && rect.left<=0 && rect.top<=0 && uint64_t(std::max<LONG>(0,rect.right))>=desc.Width && uint32_t(std::max<LONG>(0,rect.bottom))>=desc.Height;
    if(full){const bool material=s->invalid.erase(source)!=0,surface=s->surface_invalid.erase(source)!=0;if(material || surface)const_cast<Cache*>(cache)->InvalidateCommandListRenderTargets();}
    auto it=s->targets.find(source);if(it==s->targets.end())return;
    const float zero[4]{};if(!it->second.target->Clear(list,zero,{&rect,1}))throw std::runtime_error("Material clear failed");
    if(it->second.surface){const float absent[4]={0,0,0,-1};if(!it->second.surface->Clear(list,absent,{&rect,1}))throw std::runtime_error("Surface clear failed");}
}
em2::MaterialCaptureBinding em2::FindMaterialCaptureSource(const Cache* cache,ID3D12Resource* source){auto s=Find(cache);if(!s)return {};if(s->owner!=std::this_thread::get_id())throw std::logic_error("Material resolve owner mismatch");auto it=s->targets.find(source);if(s->capture_disabled || s->invalid.contains(source) || it==s->targets.end())return {};return {true,it->second.target->rtv(),it->second.target};}
bool em2::ReserveMaterialCaptureMemory(const Cache* cache,uint64_t bytes){auto s=Find(cache);if(!s)return false;if(s->owner!=std::this_thread::get_id())throw std::logic_error("Material reservation owner mismatch");if(s->capture_disabled || bytes>s->budget-s->used)return false;s->used+=bytes;return true;}
void em2::ReleaseMaterialCaptureMemory(const Cache* cache,uint64_t bytes){auto s=Find(cache);if(!s)return;if(s->owner!=std::this_thread::get_id() || bytes>s->used)throw std::logic_error("Material reservation release mismatch");s->used-=bytes;}
em2::MaterialPipelineScope::MaterialPipelineScope(const Cache* cache):previous_(std::move(current)){current=Find(cache);}
em2::MaterialPipelineScope::~MaterialPipelineScope(){current=std::move(previous_);}
std::shared_ptr<const std::vector<uint8_t>> em2::CurrentMaterialOriginal(const D3D12_SHADER_BYTECODE& code,bool* surface){if(surface)*surface=false;if(!current)return {};std::lock_guard lock(current->programs_mutex);for(const auto& program:current->programs)if(program.replacement.size()==code.BytecodeLength && !std::memcmp(program.replacement.data(),code.pShaderBytecode,program.replacement.size())){if(surface)*surface=program.surface;return program.original;}return {};}
void em2::SetMaterialCaptureTestGate(const Cache* cache,std::shared_ptr<MaterialCaptureTestGate> gate){auto state=Find(cache);if(!state)return;std::lock_guard lock(state->programs_mutex);state->test_gate=std::move(gate);}
void em2::WaitMaterialCaptureTestGate(){if(!current)return;std::shared_ptr<MaterialCaptureTestGate> gate;{std::lock_guard lock(current->programs_mutex);gate=current->test_gate;}if(gate){gate->entered.release();gate->release.acquire();}}
void em2::CountMaterialFallback(){if(current)++current->fallbacks;}
bool em2::CurrentMaterialCapture(){return bool(current);}
bool em2::CurrentMaterialSurfaceCapture(){return current && current->surfaces;}
bool em2::CurrentLinearMaterialSurfaceCapture(){return current && current->surfaces && current->linear_surfaces;}
bool em2::CurrentMaterialSurfacePreserving(const D3D12_SHADER_BYTECODE& code){
    if(!current || !current->surfaces || !code.pShaderBytecode || !code.BytecodeLength)return false;
    std::lock_guard lock(current->programs_mutex);
    for(const auto& program:current->surface_preserving)if(program.size()==code.BytecodeLength && !std::memcmp(program.data(),code.pShaderBytecode,program.size()))return true;
    return false;
}
void em2::CountMaterialClearPipeline(){if(current)++current->clear_pipelines;}
bool em2::StampCurrentMaterialInertPipeline(ID3D12PipelineState* pipeline){return current && pipeline && SUCCEEDED(pipeline->SetPrivateData(inert_guid,sizeof(current->token),&current->token));}
bool em2::StampCurrentMaterialPipeline(ID3D12PipelineState* pipeline){return current && pipeline && SUCCEEDED(pipeline->SetPrivateData(capture_guid,sizeof(current->token),&current->token));}

extern "C" bool EM2OriginalTranslateMaterial(rex::graphics::d3d12::PipelineCache*,Translator*,rex::graphics::d3d12::D3D12Shader::D3D12Translation*,IDxbcConverter*,IDxcUtils*,IDxcCompiler*);
bool rex::graphics::d3d12::PipelineCache::TranslateAnalyzedShader(Translator& native,D3D12Shader::D3D12Translation& translation,IDxbcConverter* converter,IDxcUtils* utils,IDxcCompiler* compiler){
    auto state=BeginTranslation(&render_target_cache_);
    const bool success=EM2OriginalTranslateMaterial(this,&native,&translation,converter,utils,compiler);if(!success)return false;
    if(!state)return true;++state->translations;
    if(state->surfaces && !(native.*MemberOf(Rov{})) && translation.shader().type()==xenos::ShaderType::kPixel){
        const auto& code=translation.shader().ucode_data();const auto& fog=em2::scene_shader_data::ScreenSpace_DepthFog_0;
        if(code.size()==fog.size() && std::equal(code.begin(),code.end(),fog.begin()))try{
            const auto& binary=translation.translated_binary();std::lock_guard lock(state->programs_mutex);
            if(!binary.empty() && state->surface_preserving.size()<256)state->surface_preserving.push_back(binary);
        }catch(const std::exception& e){++state->failures;REXLOG_WARN("Surface-preserving shader registration unavailable: {}",e.what());}
    }
    const auto words=Replacement(&translation.shader(),state->surfaces);if(words.empty() || native.*MemberOf(Rov{}))return true;
    const bool surface=state->surfaces && words.data()==em2::material_shader_data::world_base_combined.data();
    try{
        Translator translator(native.*MemberOf(Vendor{}),native.*MemberOf(Bindless{}),false,native.*MemberOf(Gamma{}),native.*MemberOf(Msaa{}),native.*MemberOf(ScaleX{}),native.*MemberOf(ScaleY{}));
        DxbcShader candidate(xenos::ShaderType::kPixel,translation.shader().ucode_data_hash(),words.data(),words.size(),std::endian::native);rex::string::StringBuffer text;candidate.AnalyzeUcode(text);auto* result=candidate.GetOrCreateTranslation(translation.modification());
        if(!translator.TranslateAnalyzedShader(*result) || !result->is_valid() || !Compatible(static_cast<const DxbcShader&>(translation.shader()),candidate) || (surface?em2::ExportMaterialLightingSurface(result->translated_binary()):em2::ExportMaterialAmbient(result->translated_binary())).empty()){++state->failures;return true;}
        std::vector<uint8_t> replacement=result->translated_binary();
        {std::lock_guard lock(state->programs_mutex);if(state->programs.size()>=256){++state->failures;return true;}state->programs.push_back({replacement,std::make_shared<const std::vector<uint8_t>>(translation.translated_binary()),surface});}
        (translation.*MemberOf(Binary{})).swap(replacement);translation.set_host_disassembly({});++state->replacements;
    }catch(const std::exception& e){++state->failures;REXLOG_WARN("Material preparation translation rejected: {}",e.what());}
    return true;
}

extern "C" void* EM2OriginalDeferredWriteCommand(rex::graphics::d3d12::DeferredCommandList*,uint32_t,size_t);
void* rex::graphics::d3d12::DeferredCommandList::WriteCommand(Command command,size_t bytes){
    if(active.load(std::memory_order_acquire) && (command==Command::kD3DDrawInstanced || command==Command::kD3DDrawIndexedInstanced)){
        em2::BeforeMaterialCaptureDraw(const_cast<CP&>(command_processor_));
    }
    return EM2OriginalDeferredWriteCommand(this,uint32_t(command),bytes);
}

em2::SceneViewTarget em2::CaptureSceneViewTarget(CP& cp){
    auto* cache=(cp.*MemberOf(CpCache{})).get();
    if(!cache || cache->GetPath()!=rex::graphics::RenderTargetCache::Path::kHostRenderTargets || GetHdrSceneEncoding(cache)!=HdrSceneEncoding::kNativeLinear || !(cache->*MemberOf(FramebufferValid{})))return {};
    Target* targets[5]{};std::memcpy(targets,cache->*MemberOf(CurrentTargets{}),sizeof(targets));if(!targets[1])return {};
    auto* resource=static_cast<Host*>(targets[1])->resource();bool srgb=false;if(!FindHdrSceneSource(cache,resource,srgb).target)return {};
    return {cache,resource};
}

bool em2::HasMaterialSurfaceCapture(const Cache* cache){auto s=Find(cache);return s && s->surfaces;}
bool em2::HasLinearMaterialSurfaceCapture(const Cache* cache){auto s=Find(cache);return s && s->surfaces && s->linear_surfaces;}
bool em2::StampCurrentMaterialSurfacePipeline(ID3D12PipelineState* pipeline){return current && current->surfaces && pipeline && SUCCEEDED(pipeline->SetPrivateData(surface_guid,sizeof(current->token),&current->token));}
void em2::InvalidateMaterialSurface(const Cache* cache,ID3D12Resource* source){
    auto s=Find(cache);if(!s || !s->surfaces || !source)return;
    if(s->owner!=std::this_thread::get_id())throw std::logic_error("Surface invalidation owner mismatch");
    const bool was=s->surface_disabled || s->surface_invalid.contains(source);
    if(s->surface_invalid.size()>=4096)s->surface_disabled=true;else s->surface_invalid.insert(source);
    if(!was)const_cast<Cache*>(cache)->InvalidateCommandListRenderTargets();
}
namespace {
em2::MaterialCaptureBinding AcquireSurfaceTarget(const std::shared_ptr<em2::MaterialCaptureState>& s,ID3D12Resource* source,rex::graphics::d3d12::DeferredCommandList& list){
    using namespace em2;if(!s || !s->surfaces || !source)return {};
    auto material=AcquireMaterialTarget(s,source,list);if(!material.target)return material;
    MaterialCaptureBinding result{true,s->nulls->GetCPUDescriptorHandleForHeapStart(),{}};
    if(source->GetDesc().SampleDesc.Count>1)result.rtv.ptr+=s->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if(s->surface_disabled || s->surface_invalid.contains(source))return result;
    auto& entry=s->targets.at(source);
    if(!entry.surface){
        const auto desc=material.target->texture()->GetDesc();const auto allocation=s->device->GetResourceAllocationInfo(0,1,&desc);
        if(allocation.SizeInBytes==UINT64_MAX || allocation.SizeInBytes>s->budget-s->used)return result;
        std::shared_ptr<HdrSceneTarget> target=HdrSceneTarget::Create(source);if(!target)return result;
        entry.surface=std::move(target);s->used+=allocation.SizeInBytes;
        const float absent[4]={0,0,0,-1};if(!entry.surface->Clear(list,absent))throw std::runtime_error("Surface initial clear failed");
    }
    result.target=entry.surface;result.rtv=result.target->rtv();return result;
}
} // namespace
em2::MaterialCaptureBinding em2::AcquireMaterialSurface(const Cache* cache,ID3D12Resource* source,rex::graphics::d3d12::DeferredCommandList& list){
    auto s=Find(cache);if(!s || !s->draw || !s->surface_draw)return {};return AcquireSurfaceTarget(s,source,list);
}
em2::MaterialCaptureTransfer em2::PrepareMaterialSurfaceTransfer(const Cache* cache,ID3D12Resource* source,ID3D12Resource* destination,rex::graphics::d3d12::DeferredCommandList& list){
    auto s=Find(cache);if(!s || !s->surfaces)return {};
    auto src=FindMaterialSurfaceSource(cache,source);
    if(source==destination || !src.target){InvalidateMaterialSurface(cache,destination);return {};}
    auto dst=AcquireSurfaceTarget(s,destination,list);
    if(!dst.target){InvalidateMaterialSurface(cache,destination);return {};}
    return {std::move(src.target),std::move(dst.target)};
}
void em2::CountMaterialSurfaceTransfer(const Cache* cache){auto s=Find(cache);if(s)++s->surface_transfers;}
em2::MaterialCaptureBinding em2::FindMaterialSurfaceSource(const Cache* cache,ID3D12Resource* source){
    auto s=Find(cache);if(!s || !s->surfaces || s->capture_disabled || s->surface_disabled || s->invalid.contains(source) || s->surface_invalid.contains(source))return {};
    if(s->owner!=std::this_thread::get_id())throw std::logic_error("Surface lookup owner mismatch");
    const auto it=s->targets.find(source);if(it==s->targets.end() || !it->second.surface)return {};
    return {true,it->second.surface->rtv(),it->second.surface};
}

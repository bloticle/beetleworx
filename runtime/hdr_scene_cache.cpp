#include "hdr_scene_cache.h"
#include "material_capture.h"
#include "hdr_scene_target.h"
#include "hdr_scene_resolve.h"
#include <rex/graphics/d3d12/pipeline_cache.h>
#include <rex/graphics/d3d12/render_target_cache.h>
#include <atomic>
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <windows.h>
#include <stdexcept>
#include <cmath>
#include <cstring>
#include <immintrin.h>
namespace em2 {
struct HdrSceneCacheState {
    DWORD owner_thread=GetCurrentThreadId();
    HdrSceneEncoding encoding=HdrSceneEncoding::kCompatibility;
    bool have_binding_policy=false,binding_srgb=false,binding_native_gamma=false;
    std::atomic<uint64_t> builds{0},worker_builds{0};
    std::atomic<uint64_t> bindings{0};
    std::atomic<uint64_t> clears{0},encoding_changes{0};
    std::atomic<uint64_t> transfers{0},native_linear_builds{0};
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> null_rtvs;
    struct Clear {float raw[4];D3D12_RECT rectangle;};
    struct Entry {std::shared_ptr<HdrSceneTarget> target;bool initialized=false,srgb=false,native_gamma=false,disabled=false;std::vector<Clear> pending;std::shared_ptr<const SceneViewIdentity> view;};
    std::unordered_map<ID3D12Resource*,Entry> targets;
    std::unordered_set<ID3D12Resource*> invalid;
    bool capture_disabled=false;
    uint64_t budget=0,used=0;
};
}
namespace {
using Cache=rex::graphics::d3d12::D3D12RenderTargetCache;
std::mutex mutex;
std::atomic<size_t> linear_cache_count=0;
std::unordered_map<const Cache*,std::shared_ptr<em2::HdrSceneCacheState>> registered;
std::unordered_set<const Cache*> started;
thread_local std::shared_ptr<em2::HdrSceneCacheState> current;
void ApplyClear(em2::HdrSceneCacheState::Entry& entry,const em2::HdrSceneCacheState::Clear& clear,rex::graphics::d3d12::DeferredCommandList& list){
    const auto csr=_mm_getcsr();struct FP{unsigned v;~FP(){_mm_setcsr(v);}}fp{csr};
    float value[4];std::memcpy(value,clear.raw,sizeof(value));
    if(entry.srgb)for(unsigned c=0;c<3;++c)value[c]=value[c]<=.04045f?value[c]/12.92f:std::pow((value[c]+.055f)/1.055f,2.4f);
    else if(entry.native_gamma)for(unsigned c=0;c<3;++c)value[c]=rex::graphics::xenos::PWLGammaToLinear(value[c]);
    if(!entry.target->Clear(list,value,{&clear.rectangle,1}))throw std::runtime_error("HDR native clear rejected");
}
struct PipelineScope {
    std::shared_ptr<em2::HdrSceneCacheState> previous=std::move(current);
    explicit PipelineScope(const Cache* cache){std::lock_guard lock(mutex);started.insert(cache);auto it=registered.find(cache);current=it==registered.end()?nullptr:it->second;}
    ~PipelineScope(){current=std::move(previous);}
};
}
std::unique_ptr<em2::HdrSceneCacheRegistration> em2::HdrSceneCacheRegistration::Create(const Cache& cache,ID3D12Device* device,uint64_t budget,HdrSceneEncoding encoding){
    if(!device || (encoding!=HdrSceneEncoding::kCompatibility && encoding!=HdrSceneEncoding::kNativeLinear) || (encoding==HdrSceneEncoding::kNativeLinear && (!HasMaterialCapture(&cache) || cache.GetPath()!=rex::graphics::RenderTargetCache::Path::kHostRenderTargets)))return {};
    std::lock_guard lock(mutex);if(started.contains(&cache) || registered.contains(&cache))return {};
    auto result=std::unique_ptr<HdrSceneCacheRegistration>(new HdrSceneCacheRegistration);
    result->state_=std::make_shared<HdrSceneCacheState>();auto& s=*result->state_;s.device=device;s.budget=budget;s.encoding=encoding;
    const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,2,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&s.null_rtvs))))return {};
    auto handle=s.null_rtvs->GetCPUDescriptorHandleForHeapStart();
    for(unsigned i=0;i<2;++i){D3D12_RENDER_TARGET_VIEW_DESC view{};view.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;view.ViewDimension=i?D3D12_RTV_DIMENSION_TEXTURE2DMS:D3D12_RTV_DIMENSION_TEXTURE2D;device->CreateRenderTargetView(nullptr,&view,handle);handle.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);}
    registered.emplace(&cache,result->state_);if(encoding==HdrSceneEncoding::kNativeLinear)linear_cache_count.fetch_add(1,std::memory_order_release);result->cache_=&cache;return result;
}
em2::HdrSceneBinding em2::AcquireHdrSceneBinding(const Cache* cache,ID3D12Resource* legacy,rex::graphics::d3d12::DeferredCommandList* initialize,bool srgb,bool native_gamma){
    if(!legacy)return {};
    std::shared_ptr<HdrSceneCacheState> state;
    {std::lock_guard lock(mutex);auto it=registered.find(cache);if(it==registered.end())return {};state=it->second;started.insert(cache);}
    const auto desc=legacy->GetDesc();
    if(desc.Format!=DXGI_FORMAT_R8G8B8A8_TYPELESS && desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)return {};
    auto& s=*state;if(GetCurrentThreadId()!=s.owner_thread)throw std::logic_error("HDR binding must run on its cache owner thread");
    if(initialize)++s.bindings;
    HdrSceneBinding result;result.encoding=s.encoding;result.active=true;result.rtv=s.null_rtvs->GetCPUDescriptorHandleForHeapStart();
    if(desc.SampleDesc.Count>1)result.rtv.ptr+=s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if(s.capture_disabled || s.invalid.contains(legacy))return result;
    auto it=s.targets.find(legacy);
    if(it==s.targets.end()){
        auto hdr=desc;hdr.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;hdr.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;hdr.Alignment=0;hdr.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
        const auto a=s.device->GetResourceAllocationInfo(0,1,&desc),b=s.device->GetResourceAllocationInfo(0,1,&hdr);
        if(a.SizeInBytes==UINT64_MAX || b.SizeInBytes==UINT64_MAX || a.SizeInBytes>s.budget-s.used || b.SizeInBytes>s.budget-s.used-a.SizeInBytes)return result;
        auto target=HdrSceneTarget::Create(legacy);if(!target)return result;
        s.used+=a.SizeInBytes+b.SizeInBytes;it=s.targets.emplace(legacy,HdrSceneCacheState::Entry{std::move(target)}).first;
    }
    if(it->second.disabled)return result;
    const auto fallback=result.rtv;
    result.target=it->second.target;result.rtv=result.target->rtv();
    if(initialize){auto& entry=it->second;
        native_gamma=native_gamma && s.encoding==HdrSceneEncoding::kNativeLinear;
        if(entry.initialized && (entry.srgb!=srgb || entry.native_gamma!=native_gamma)){++s.encoding_changes;entry.disabled=true;entry.pending.clear();entry.view.reset();return {true,fallback,{},s.encoding};}
        entry.srgb=srgb;entry.native_gamma=native_gamma;
        if(!entry.initialized){const float black[4]{};if(!result.target->Clear(*initialize,black))throw std::runtime_error("HDR initial clear failed");entry.initialized=true;}
        for(const auto& clear:entry.pending)ApplyClear(entry,clear,*initialize);entry.pending.clear();
    }
    if(s.encoding==HdrSceneEncoding::kNativeLinear){const auto& entry=it->second;result.guest_transfer=entry.srgb?HdrGuestTransfer::kSrgb:entry.native_gamma?HdrGuestTransfer::kXenosPwl:HdrGuestTransfer::kRaw;}
    return result;
}
void em2::MirrorHdrSceneClear(const Cache* cache,ID3D12Resource* legacy,const float (&raw)[4],const D3D12_RECT& rectangle,rex::graphics::d3d12::DeferredCommandList& list){
    std::shared_ptr<HdrSceneCacheState> state;{std::lock_guard lock(mutex);auto it=registered.find(cache);if(it==registered.end() || !legacy)return;state=it->second;}
    if(GetCurrentThreadId()!=state->owner_thread)throw std::logic_error("HDR clear owner mismatch");
    const auto desc=legacy->GetDesc();const bool full=rectangle.left<=0 && rectangle.top<=0 && uint64_t(std::max<LONG>(0,rectangle.right))>=desc.Width && uint32_t(std::max<LONG>(0,rectangle.bottom))>=desc.Height;
    auto old=state->targets.find(legacy);if(old!=state->targets.end())old->second.view.reset();if(full && (state->invalid.contains(legacy) || (old!=state->targets.end() && old->second.disabled))){state->invalid.erase(legacy);if(old!=state->targets.end()){old->second.initialized=false;old->second.disabled=false;old->second.pending.clear();}const_cast<Cache*>(cache)->InvalidateCommandListRenderTargets();}
    auto binding=AcquireHdrSceneBinding(cache,legacy);if(!binding.target)return;
    auto& entry=state->targets.at(legacy);HdrSceneCacheState::Clear clear{};std::memcpy(clear.raw,raw,sizeof(clear.raw));clear.rectangle=rectangle;
    if(entry.initialized)ApplyClear(entry,clear,list);
    else {if(entry.pending.size()>=128){entry.disabled=true;entry.pending.clear();return;}entry.pending.push_back(clear);}
    ++state->clears;
}
em2::HdrSceneCacheRegistration::~HdrSceneCacheRegistration(){
    if(!cache_)return;
    std::lock_guard lock(mutex);auto it=registered.find(cache_);
    // Once compilation starts, mode belongs to the cache until its native
    // shutdown. Dropping an observer handle must not split worker generations.
    if(!started.contains(cache_) && it!=registered.end() && it->second==state_){if(state_->encoding==HdrSceneEncoding::kNativeLinear)linear_cache_count.fetch_sub(1,std::memory_order_release);registered.erase(it);}
}
em2::HdrSceneTransfer em2::PrepareHdrSceneTransfer(const Cache* cache,ID3D12Resource* source,ID3D12Resource* destination,rex::graphics::d3d12::DeferredCommandList& list){
    std::shared_ptr<HdrSceneCacheState> state;{std::lock_guard lock(mutex);auto it=registered.find(cache);if(it==registered.end())return {};state=it->second;}
    InvalidateHdrSceneView(cache,destination);
    auto it=state->targets.find(source);
    if(source==destination || state->capture_disabled || state->invalid.contains(source) || it==state->targets.end() || !it->second.initialized || it->second.disabled){InvalidateHdrSceneDestination(cache,destination);return {};}
    const bool srgb=it->second.srgb,native_gamma=it->second.native_gamma;auto src=it->second.target;
    auto dst=AcquireHdrSceneBinding(cache,destination,&list,srgb,native_gamma);if(!dst.target){InvalidateHdrSceneDestination(cache,destination);return {};}
    return {std::move(src),std::move(dst.target)};
}
void em2::InvalidateHdrSceneDestination(const Cache* cache,ID3D12Resource* destination){
    std::shared_ptr<HdrSceneCacheState> state;{std::lock_guard lock(mutex);auto it=registered.find(cache);if(it==registered.end())return;state=it->second;}
    if(GetCurrentThreadId()!=state->owner_thread)throw std::logic_error("HDR invalidation owner mismatch");const bool was_invalid=state->capture_disabled || state->invalid.contains(destination);
    if(state->invalid.size()>=4096)state->capture_disabled=true;else state->invalid.insert(destination);
    if(!was_invalid)const_cast<Cache*>(cache)->InvalidateCommandListRenderTargets();
    auto it=state->targets.find(destination);if(it!=state->targets.end()){it->second.disabled=true;it->second.pending.clear();it->second.view.reset();}
}
void em2::CountHdrSceneTransfer(const Cache* cache){std::lock_guard lock(mutex);auto it=registered.find(cache);if(it!=registered.end())++it->second->transfers;}
bool em2::HasHdrSceneCache(const Cache* cache){std::lock_guard lock(mutex);return registered.contains(cache);}
bool em2::HdrSceneCacheRegistration::KeepUntilCacheShutdown(){
    std::lock_guard lock(mutex);auto it=registered.find(cache_);
    if(it==registered.end() || it->second!=state_ || GetCurrentThreadId()!=state_->owner_thread)return false;
    // The same lock used when compilation starts prevents later retagging;
    // native cache shutdown releases the registration in either case.
    started.insert(cache_);return true;
}
em2::HdrSceneCacheStats em2::HdrSceneCacheRegistration::stats()const{return {state_->builds.load(),state_->worker_builds.load(),state_->bindings.load(),state_->clears.load(),state_->encoding_changes.load(),state_->transfers.load(),state_->native_linear_builds.load()};}
void em2::ShutdownHdrSceneCache(const Cache* cache){ShutdownHdrSceneResolves(cache);std::lock_guard lock(mutex);auto it=registered.find(cache);if(it!=registered.end()){if(it->second->encoding==HdrSceneEncoding::kNativeLinear)linear_cache_count.fetch_sub(1,std::memory_order_release);registered.erase(it);}started.erase(cache);}
em2::HdrSceneBinding em2::FindHdrSceneSource(const Cache* cache,ID3D12Resource* legacy,bool& srgb){
    std::lock_guard lock(mutex);auto found=registered.find(cache);if(found==registered.end())return {};
    auto& s=*found->second;if(GetCurrentThreadId()!=s.owner_thread)throw std::logic_error("HDR resolve must run on its cache owner thread");
    auto it=s.targets.find(legacy);if(s.capture_disabled || s.invalid.contains(legacy) || it==s.targets.end() || !it->second.initialized || it->second.disabled)return {};
    srgb=it->second.srgb;const auto transfer=s.encoding==HdrSceneEncoding::kNativeLinear?(it->second.srgb?HdrGuestTransfer::kSrgb:it->second.native_gamma?HdrGuestTransfer::kXenosPwl:HdrGuestTransfer::kRaw):HdrGuestTransfer::kRaw;return {true,it->second.target->rtv(),it->second.target,s.encoding,transfer};
}
bool em2::ReserveHdrSceneMemory(const Cache* cache,uint64_t bytes){
    std::lock_guard lock(mutex);auto it=registered.find(cache);if(it==registered.end())return false;
    auto& s=*it->second;if(s.capture_disabled || bytes>s.budget-s.used)return false;s.used+=bytes;return true;
}
void em2::ReleaseHdrSceneMemory(const Cache* cache,uint64_t bytes){
    std::lock_guard lock(mutex);auto it=registered.find(cache);if(it!=registered.end()){auto& used=it->second->used;if(bytes>used)throw std::logic_error("HDR memory reservation underflow");used-=bytes;}
}
em2::HdrSceneEncoding em2::GetHdrSceneEncoding(const Cache* cache){std::lock_guard lock(mutex);auto it=registered.find(cache);return it==registered.end()?HdrSceneEncoding::kCompatibility:it->second->encoding;}
em2::HdrSceneEncoding em2::CurrentHdrSceneEncoding(){return current?current->encoding:HdrSceneEncoding::kCompatibility;}
bool em2::UpdateHdrSceneBindingPolicy(const Cache* cache,bool srgb,bool native_gamma){if(!linear_cache_count.load(std::memory_order_acquire))return false;std::lock_guard lock(mutex);auto it=registered.find(cache);if(it==registered.end() || it->second->encoding!=HdrSceneEncoding::kNativeLinear)return false;auto& s=*it->second;if(GetCurrentThreadId()!=s.owner_thread)throw std::logic_error("HDR binding policy owner mismatch");const bool changed=!s.have_binding_policy || s.binding_srgb!=srgb || s.binding_native_gamma!=native_gamma;s.have_binding_policy=true;s.binding_srgb=srgb;s.binding_native_gamma=native_gamma;return changed;}
bool em2::CurrentHdrScenePipeline(){return bool(current);}
void em2::CountHdrScenePipelineBuild(bool native_linear){if(current){++current->builds;if(native_linear)++current->native_linear_builds;if(GetCurrentThreadId()!=current->owner_thread)++current->worker_builds;}}
extern "C" ID3D12PipelineState* EM2OriginalCreateD3D12Pipeline(rex::graphics::d3d12::PipelineCache*,const void*);
ID3D12PipelineState* rex::graphics::d3d12::PipelineCache::CreateD3D12Pipeline(const PipelineRuntimeDescription& description){
    PipelineScope scope(&render_target_cache_);em2::MaterialPipelineScope material_scope(&render_target_cache_);return EM2OriginalCreateD3D12Pipeline(this,&description);
}

bool em2::BindHdrSceneView(const Cache* cache,ID3D12Resource* legacy,std::shared_ptr<const SceneViewIdentity> view){
    if(!legacy || !view || !view->width || !view->height || view->width>16384 || view->height>16384)return false;
    std::lock_guard lock(mutex);auto found=registered.find(cache);if(found==registered.end())return false;auto& s=*found->second;
    if(GetCurrentThreadId()!=s.owner_thread)throw std::logic_error("Scene view binding owner mismatch");
    if(s.encoding!=HdrSceneEncoding::kNativeLinear || s.capture_disabled || s.invalid.contains(legacy))return false;
    auto it=s.targets.find(legacy);if(it==s.targets.end() || !it->second.initialized || it->second.disabled)return false;
    const auto d=legacy->GetDesc();if(d.SampleDesc.Count!=1 || view->width>d.Width || view->height>d.Height)return false;
    it->second.view=std::move(view);return true;
}
void em2::InvalidateHdrSceneView(const Cache* cache,ID3D12Resource* legacy){
    if(!linear_cache_count.load(std::memory_order_acquire))return;
    std::lock_guard lock(mutex);auto found=registered.find(cache);if(found==registered.end())return;auto& s=*found->second;
    if(GetCurrentThreadId()!=s.owner_thread)throw std::logic_error("Scene view invalidation owner mismatch");
    auto it=s.targets.find(legacy);if(it!=s.targets.end())it->second.view.reset();
}
std::shared_ptr<const em2::SceneViewIdentity> em2::FindHdrSceneView(const Cache* cache,ID3D12Resource* legacy){
    std::lock_guard lock(mutex);auto found=registered.find(cache);if(found==registered.end())return {};auto& s=*found->second;
    if(GetCurrentThreadId()!=s.owner_thread)throw std::logic_error("Scene view lookup owner mismatch");
    if(s.encoding!=HdrSceneEncoding::kNativeLinear || s.capture_disabled || s.invalid.contains(legacy))return {};
    auto it=s.targets.find(legacy);return it!=s.targets.end() && it->second.initialized && !it->second.disabled?it->second.view:nullptr;
}

#include "gpu_constants_internal.h"
#include "transfer_benchmark.h"
#include "hdr_output.h"
#include "hdr_presenter.h"
#include "hdr_scene_resolve.h"
#include "hdr_guest_view.h"
#include "scene_lighting.h"
#include <rex/graphics/d3d12/texture_cache.h>
#include <rex/graphics/graphics_system.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <algorithm>
#include <cmath>
#include <vector>
namespace {
using CP=rex::graphics::d3d12::D3D12CommandProcessor;
template<class Tag,auto Member>struct Access{friend auto MemberOf(Tag){return Member;}};
struct TextureTag{friend auto MemberOf(TextureTag);};template struct Access<TextureTag,&CP::texture_cache_>;
struct BeginTag{friend auto MemberOf(BeginTag);};template struct Access<BeginTag,&CP::BeginSubmission>;
struct EndTag{friend auto MemberOf(EndTag);};template struct Access<EndTag,&CP::EndSubmission>;
}
struct em2::HdrSwapState {
    HdrOutputPass pass;
    HdrOutputSettings settings;
    struct Entry {std::unique_ptr<HdrOutputBindings> bindings;uint64_t submission;};
    std::vector<Entry> entries;
    HdrGuestViewPass sdr_pass;
    struct SdrEntry {std::shared_ptr<const HdrResolvedScene> owner;std::shared_ptr<HdrGuestViewBindings> bindings;ID3D12Resource* source;uint64_t revision=0,submission=0,bytes=0;};
    std::vector<SdrEntry> sdr_entries;
    bool sdr_failed=false;
    bool initialized=false;
};
bool em2::Em2CommandProcessor::ReleaseHdrSwapBindings(){
    if(!IsGraphicsWorker() || !DrainForResourceChange())return false;
    if(hdr_swap_){hdr_swap_->entries.clear();hdr_swap_->sdr_entries.clear();hdr_swap_->sdr_failed=false;}return true;
}
ID3D12Resource* em2::Em2CommandProcessor::RequestSceneSdrSwapTexture(const std::shared_ptr<const HdrResolvedScene>& scene){
    if(!IsGraphicsWorker() || !SceneLightingEnabled() || !scene || !scene->valid() || scene->encoding()!=HdrSceneEncoding::kNativeLinear || !scene->guest_texture())return nullptr;
    try{
        if(!hdr_swap_)hdr_swap_=std::make_shared<HdrSwapState>();auto& state=*hdr_swap_;if(state.sdr_failed)return nullptr;
        auto* device=GetD3D12Provider().GetDevice();auto* source=scene->guest_texture();
        std::erase_if(state.sdr_entries,[&](const auto& entry){return entry.submission<=GetCompletedSubmission() && entry.source!=source;});
        auto found=std::find_if(state.sdr_entries.begin(),state.sdr_entries.end(),[&](const auto& entry){return entry.source==source;});
        if(found==state.sdr_entries.end()){
            if(state.sdr_entries.size()>=8 || !state.sdr_pass.Initialize(device))return nullptr;
            auto desc=source->GetDesc();desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;desc.Alignment=0;desc.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
            const auto bytes=device->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;uint64_t used=0;for(const auto& entry:state.sdr_entries)used+=entry.bytes;
            if(!bytes || bytes>64ull*1024*1024 || used+bytes>128ull*1024*1024)return nullptr;
            auto binding=HdrGuestViewBindings::Create(source,DXGI_FORMAT_R8G8B8A8_UNORM);if(!binding)return nullptr;
            state.sdr_entries.push_back({scene,std::move(binding),source,0,GetCurrentSubmission(),bytes});found=state.sdr_entries.end()-1;
        }
        // Own before recording and keep through the native fence. Quantizing to
        // RGBA8 preserves the native gamma shader's unsigned LUT input range.
        found->submission=GetCurrentSubmission();
        if(found->revision!=scene->revision()){
            if(!state.sdr_pass.Record(*this,*found->bindings,HdrGuestTransfer::kRaw))return nullptr;
            found->revision=scene->revision();
        }
        return found->bindings->result();
    }catch(...){if(hdr_swap_)hdr_swap_->sdr_failed=true;return nullptr;}
}
bool em2::Em2CommandProcessor::SetHdrSwapSettings(const HdrOutputSettings& s){
    if(!IsGraphicsWorker() || !std::isfinite(s.exposure) || s.exposure<1.f/256 || s.exposure>256 || !std::isfinite(s.paper_white_nits) || s.paper_white_nits<=0 || s.paper_white_nits>1000 || !std::isfinite(s.peak_nits) || s.peak_nits<s.paper_white_nits || s.peak_nits>10000 || !std::isfinite(s.bloom_strength) || s.bloom_strength<0 || s.bloom_strength>16)return false;
    if(!hdr_swap_)hdr_swap_=std::make_shared<HdrSwapState>();hdr_swap_->settings=s;
    SetHdrPresenterPaperWhite(static_cast<rex::ui::d3d12::D3D12Presenter*>(graphics_system_->presenter()),s.paper_white_nits);SetHdrPresenterPeak(static_cast<rex::ui::d3d12::D3D12Presenter*>(graphics_system_->presenter()),s.peak_nits);return true;
}
void em2::Em2CommandProcessor::IssueSwap(uint32_t ptr,uint32_t width,uint32_t height){
    TransferBenchmarkAtSwap();
    scene_draw_inputs_.reset();
    scene_replay_.reset();
    scene_depth_view_.reset();
    using Presenter=rex::ui::d3d12::D3D12Presenter;
    auto* presenter=static_cast<Presenter*>(graphics_system_->presenter());
    if(hdr_swap_ && !SceneLightingEnabled())std::erase_if(hdr_swap_->sdr_entries,[&](const auto& entry){return entry.submission<=GetCompletedSubmission();});
    if(!IsHdrPresenter(presenter)){D3D12CommandProcessor::IssueSwap(ptr,width,height);return;}
    // Preserve native submission/fence accounting, including end-frame work.
    // A legacy fallback reuses this already-open submission.
    if(!(this->*MemberOf(BeginTag{}))(true))return;
    if(hdr_swap_){SetHdrPresenterPaperWhite(presenter,hdr_swap_->settings.paper_white_nits);SetHdrPresenterPeak(presenter,hdr_swap_->settings.peak_nits);}
    rex::thread::global_critical_region region;auto lock=region.Acquire();
    auto* cache=(this->*MemberOf(TextureTag{})).get();
    auto scene=FindHdrSwapScene(cache,register_file_->GetTextureFetch(0));
    if(hdr_swap_){auto& entries=hdr_swap_->entries;std::erase_if(entries,[&](const auto& entry){return entry.submission<=GetCompletedSubmission() && (!scene || entry.bindings->scene()!=scene->texture());});}
    if(!scene){HdrLegacyRefreshScope legacy;D3D12CommandProcessor::IssueSwap(ptr,width,height);return;}
    if(!hdr_swap_)hdr_swap_=std::make_shared<HdrSwapState>();auto& state=*hdr_swap_;
    if(!state.initialized)state.initialized=state.pass.Initialize(GetD3D12Provider().GetDevice());
    if(!state.initialized){HdrLegacyRefreshScope legacy;D3D12CommandProcessor::IssueSwap(ptr,width,height);return;}
    SetHdrPresenterPaperWhite(presenter,state.settings.paper_white_nits);SetHdrPresenterPeak(presenter,state.settings.peak_nits);
    const auto extent=scene->texture()->GetDesc();
    presenter->RefreshGuestOutput(uint32_t(extent.Width),extent.Height,1280,720,[&](rex::ui::Presenter::GuestOutputRefreshContext& base){
        auto& context=static_cast<Presenter::D3D12GuestOutputRefreshContext&>(base);
        auto* output=context.resource_uav_capable();
        auto found=std::find_if(state.entries.begin(),state.entries.end(),[&](const auto& entry){return entry.bindings->scene()==scene->texture() && entry.bindings->output()==output;});
        if(found==state.entries.end()){
            // Surface/format changes can replace mailbox resources while the
            // scene stays identical. Evict completed pairs before the bound
            // is reached; never evict a pair still referenced by the GPU.
            if(state.entries.size()>=8)std::erase_if(state.entries,[&](const auto& entry){return entry.submission<=GetCompletedSubmission();});
            if(state.entries.size()>=32)return false;
            auto bindings=state.pass.CreateBindings(scene->texture(),output);if(!bindings)return false;
            state.entries.push_back({std::move(bindings),GetCurrentSubmission()});found=state.entries.end()-1;
        }
        // Pin views/resources through the native fence, including dispatch
        // failures that may have queued resource-state transitions.
        found->submission=GetCurrentSubmission();context.SetIs8bpc(false);
        PushTransitionBarrier(output,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const bool recorded=state.pass.Record(*this,*found->bindings,state.settings);
        PushTransitionBarrier(output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);SubmitBarriers();
        if(!recorded)return false;
        (this->*MemberOf(EndTag{}))(true);return true;
    });
    // Close even when the refresher declines or an allocation fails.
    (this->*MemberOf(EndTag{}))(true);
}

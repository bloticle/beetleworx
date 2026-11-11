#include "scene_lighting.h"
#include "gpu_constants_internal.h"
#include "hdr_scene_cache.h"
#include "material_capture.h"
#include <rex/cvar.h>
#include <algorithm>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_provider.h>
namespace {
template<class Tag,auto Member>struct LightingAccess{friend auto MemberOf(Tag){return Member;}};
struct LightingCache{friend auto MemberOf(LightingCache);};template struct LightingAccess<LightingCache,&rex::graphics::d3d12::D3D12CommandProcessor::render_target_cache_>;
}
REXCVAR_DEFINE_BOOL(em2_apply_scene_lighting,false,"Debug","Experimental matched native-linear material AO composition into HDR scene resolves; requires native-linear material cache");
REXCVAR_DEFINE_BOOL(em2_apply_scene_indirect,false,"Debug","Experimental screen-space single-bounce diffuse lighting; requires verified linear material surfaces");
REXCVAR_DEFINE_BOOL(em2_prepare_scene_lighting,false,"Debug","Experimental live geometry/AO preparation only; produces GPU buffers without changing displayed lighting");
bool em2::SceneLightingEnabled(){return REXCVAR_GET(em2_apply_scene_lighting);}
bool em2::SceneIndirectEnabled(){return SceneLightingEnabled() && REXCVAR_GET(em2_apply_scene_indirect);}
std::shared_ptr<const em2::SceneLightingFrame> em2::SceneLightingState::Record(rex::graphics::d3d12::D3D12CommandProcessor& cp,const SceneReplayInputs& replay,std::shared_ptr<const SceneDepthView> depth){
    Collect(cp.GetCompletedSubmission());if(pending_.size()>=32 || !IsSceneReplayCurrent(cp,replay))return {};
    auto* device=cp.GetD3D12Provider().GetDevice();if(!raster_.Initialize(device) || !occlusion_.Initialize(device))return {};
    auto frame=std::make_shared<SceneLightingFrame>();frame->submission=replay.submission;
    frame->raster=raster_.CreateBindings(cp,replay,std::move(depth),latest_?latest_->raster:nullptr);if(!frame->raster)return {};
    const auto source=CaptureSceneViewTarget(cp);const auto geometry=frame->raster->geometry()->positions()->GetDesc();
    SceneViewIdentity metadata;metadata.frame=replay.frame;metadata.submission=replay.submission;metadata.draw=replay.scene.draw;metadata.width=uint32_t(geometry.Width);metadata.height=geometry.Height;metadata.inverse_view_projection=replay.scene.inverse_view_projection;metadata.inverse_view=replay.scene.inverse_view;metadata.depth_fetch=replay.scene.depth_fetch;metadata.viewport=replay.scene.raster->viewport;metadata.scissor=replay.scene.raster->scissor;
    frame->view=std::make_shared<const SceneViewIdentity>(metadata);
    frame->occlusion=occlusion_.CreateBindings(frame->raster->geometry(),latest_?latest_->occlusion:nullptr,frame->view);if(!frame->occlusion)return {};
    // All owners/retirement allocations precede the first GPU command. A
    // subsequent failure cannot release resources referenced by queued work.
    pending_.push_back(frame);
    if(!raster_.Record(cp,*frame->raster,replay))return {};
    frame->geometry_recorded=true;
    if(!occlusion_.Record(cp,*frame->occlusion,{2,4,.01f,1}))return {};
    frame->occlusion_recorded=true;if(source.cache)BindHdrSceneView(source.cache,source.legacy,frame->view);latest_=frame;return frame;
}
std::shared_ptr<const em2::SceneAmbientBindings> em2::SceneLightingState::Compose(rex::graphics::d3d12::D3D12CommandProcessor& cp,std::shared_ptr<const HdrResolvedScene> colour,std::shared_ptr<const HdrResolvedScene> ambient,SceneAmbientEncoding encoding,const SceneAmbientSettings& settings,std::shared_ptr<const HdrResolvedScene> surface,SceneIndirectEncoding diffuse_encoding,const SceneIndirectSettings& indirect_settings){
    if(!latest_ || std::find(pending_.begin(),pending_.end(),latest_)==pending_.end() || !latest_->occlusion_recorded || latest_->submission!=cp.GetCurrentSubmission() || latest_->compositions.size()>=8 || !composition_.Initialize(cp.GetD3D12Provider().GetDevice()))return {};
    std::shared_ptr<SceneIndirectBindings> bounce;
    if(surface){
        if(latest_->indirect.size()>=8 || !indirect_.Initialize(cp.GetD3D12Provider().GetDevice()))return {};
        bounce=indirect_.CreateBindings(colour,std::move(surface),latest_->occlusion,diffuse_encoding,indirect_latest_);if(!bounce)return {};
        // Retain before recording: even a partial command-allocation failure
        // must keep source/output descriptors and resources alive to the fence.
        latest_->indirect.push_back(bounce);if(!indirect_.Record(cp,*bounce,indirect_settings))return {};indirect_latest_=bounce;
    }
    auto previous=latest_->compositions.empty()?composition_latest_:latest_->compositions.back();auto binding=composition_.CreateBindings(std::move(colour),std::move(ambient),latest_->occlusion,encoding,previous,bounce);if(!binding)return {};
    latest_->compositions.push_back(binding);if(!composition_.Record(cp,*binding,settings))return {};composition_latest_=binding;return binding;
}
void em2::SceneLightingState::Collect(uint64_t completed){
    while(!pending_.empty() && pending_.front()->submission<=completed)pending_.pop_front();
}
void em2::Em2CommandProcessor::UpdateSceneLighting(){
    const bool enabled=REXCVAR_GET(em2_prepare_scene_lighting)||REXCVAR_GET(em2_apply_scene_lighting);
    if(scene_lighting_)scene_lighting_->Collect(GetCompletedSubmission());
    if(!enabled){
        if(scene_lighting_)scene_lighting_->Disable();
        if(scene_lighting_failed_ && (!scene_lighting_ || !scene_lighting_->pending())){scene_lighting_.reset();scene_lighting_failed_=false;scene_lighting_reported_=false;}
        return;
    }
    // After a command-allocation exception, stop issuing private passes. Keep
    // any partially recorded frame pinned until its native fence completes.
    if(scene_lighting_failed_)return;
    if(!scene_replay_ || !scene_depth_view_)return;
    try{
        if(!scene_lighting_)scene_lighting_=std::make_shared<SceneLightingState>();
        const auto frame=scene_lighting_->Record(*this,*scene_replay_,scene_depth_view_);
        if(frame && !scene_lighting_reported_){REXLOG_INFO("Live scene geometry/AO prepared at native frame{}, submission{}; matched resolve composition requested={}",GetCurrentFrame(),GetCurrentSubmission(),REXCVAR_GET(em2_apply_scene_lighting));scene_lighting_reported_=true;}
    }catch(const std::exception& e){if(!scene_lighting_failed_){REXLOG_WARN("Scene geometry/AO preparation unavailable: {}",e.what());scene_lighting_failed_=true;}}
}

bool em2::SceneLightingState::ComposeResolved(rex::graphics::d3d12::D3D12RenderTargetCache* cache,rex::graphics::d3d12::D3D12CommandProcessor& cp,const rex::graphics::draw_util::ResolveInfo& info,const SceneAmbientSettings& settings){
    auto colour=FindHdrSceneResolve(cache,info),ambient=FindMaterialSceneResolve(cache,info);if(!colour || !ambient || colour->lighting_applied())return false;
    std::shared_ptr<const HdrResolvedScene> surface;
    const bool indirect=SceneIndirectEnabled();
    if(indirect && HasLinearMaterialSurfaceCapture(cache))surface=FindMaterialSurfaceResolve(cache,info);
    // Unknown encodings or stale views may still have valid ambient inputs.
    // Select AO before recording any GI work; a partially recorded pass is
    // never retried as a different composition.
    if(surface && (!surface->valid() || !surface->view() || surface->view()!=colour->view() || !latest_ || surface->view()!=latest_->view || surface->frame()!=colour->frame() || surface->submission()!=colour->submission()))surface.reset();
    auto composition=Compose(cp,colour,ambient,SceneAmbientEncoding::kLinear,settings,surface,surface?SceneIndirectEncoding::kLinearDiffuse:SceneIndirectEncoding::kUnknown);
    if(!composition || !PublishSceneAmbientResult(cache,cp,colour,composition))return false;
    ++publications_;if(surface)++indirect_publications_;else if(indirect)++indirect_fallbacks_;return true;
}
void em2::Em2CommandProcessor::ComposeResolvedSceneLighting(rex::graphics::d3d12::D3D12RenderTargetCache* cache,const rex::graphics::draw_util::ResolveInfo& info){
    if(!scene_lighting_ || scene_lighting_failed_ || cache!=(this->*MemberOf(LightingCache{})).get())return;
    try{
        const auto previous_indirect=scene_lighting_->indirect_publications(),previous_fallbacks=scene_lighting_->indirect_fallbacks();
        if(scene_lighting_->ComposeResolved(cache,*this,info,{})){
            if(scene_lighting_->publications()==1)REXLOG_INFO("Matched geometry/material AO published into native HDR scene at frame{}, submission{}",GetCurrentFrame(),GetCurrentSubmission());
            if(!previous_indirect && scene_lighting_->indirect_publications()==1)REXLOG_INFO("Verified linear material diffuse bounce published into native scene at frame{}, submission{}; screen-space coverage only",GetCurrentFrame(),GetCurrentSubmission());
            if(!previous_fallbacks && scene_lighting_->indirect_fallbacks()==1)REXLOG_INFO("Indirect lighting has no matched linear surface input for this resolve; retaining material AO");
        }
    }
    catch(const std::exception& e){scene_lighting_failed_=true;REXLOG_WARN("Scene lighting publication unavailable: {}",e.what());}
}
void em2::ComposeNativeSceneLighting(rex::graphics::d3d12::D3D12RenderTargetCache* cache,rex::graphics::d3d12::D3D12CommandProcessor& cp,const rex::graphics::draw_util::ResolveInfo& info){
    if(!REXCVAR_GET(em2_apply_scene_lighting))return;
    if(auto* game=dynamic_cast<Em2CommandProcessor*>(&cp))game->ComposeResolvedSceneLighting(cache,info);
}

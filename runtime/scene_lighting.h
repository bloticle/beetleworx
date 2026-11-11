#pragma once
#include "scene_raster_pass.h"
#include "scene_occlusion.h"
#include "scene_ambient.h"
#include "scene_indirect.h"
#include <deque>
namespace em2 {
struct SceneLightingFrame {
    uint64_t submission=0;
    std::shared_ptr<const SceneViewIdentity> view;
    std::shared_ptr<SceneRasterBindings> raster;
    std::shared_ptr<SceneOcclusionBindings> occlusion;
    std::vector<std::shared_ptr<SceneAmbientBindings>> compositions;
    std::vector<std::shared_ptr<SceneIndirectBindings>> indirect;
    bool geometry_recorded=false,occlusion_recorded=false;
};
// GPU-worker owner. Record retains the complete frame BEFORE emitting any
// commands. Collect accepts only a completed native submission fence value.
// Keep this owner alive until all submissions using it have drained; AO root/
// pipeline objects live here. Disabling releases reuse refs, not in-flight work.
class SceneLightingState {
public:
    std::shared_ptr<const SceneLightingFrame> Record(rex::graphics::d3d12::D3D12CommandProcessor&,const SceneReplayInputs&,std::shared_ptr<const SceneDepthView>);
    // Caller holds memory lock and proves linear colour/ambient use the raster
    // frame's pixel grid. Own the binding before emitting composition commands.
    std::shared_ptr<const SceneAmbientBindings> Compose(rex::graphics::d3d12::D3D12CommandProcessor&,std::shared_ptr<const HdrResolvedScene>,std::shared_ptr<const HdrResolvedScene>,SceneAmbientEncoding,const SceneAmbientSettings&,std::shared_ptr<const HdrResolvedScene> surface={},SceneIndirectEncoding diffuse_encoding=SceneIndirectEncoding::kUnknown,const SceneIndirectSettings& indirect_settings={});
    bool ComposeResolved(rex::graphics::d3d12::D3D12RenderTargetCache*,rex::graphics::d3d12::D3D12CommandProcessor&,const rex::graphics::draw_util::ResolveInfo&,const SceneAmbientSettings&);
    uint64_t publications()const{return publications_;}
    uint64_t indirect_publications()const{return indirect_publications_;}
    uint64_t indirect_fallbacks()const{return indirect_fallbacks_;}
    void Collect(uint64_t completed);
    void Disable(){latest_.reset();composition_latest_.reset();indirect_latest_.reset();}
    size_t pending()const{return pending_.size();}
    uint32_t program_builds()const{return raster_.program_builds();}
    const std::shared_ptr<SceneLightingFrame>& latest()const{return latest_;}
private:
    uint64_t publications_=0,indirect_publications_=0,indirect_fallbacks_=0;
    SceneRasterGeometryPass raster_;
    SceneOcclusionPass occlusion_;
    SceneAmbientPass composition_;
    SceneIndirectPass indirect_;
    std::shared_ptr<const SceneIndirectBindings> indirect_latest_;
    std::shared_ptr<const SceneAmbientBindings> composition_latest_;
    std::shared_ptr<SceneLightingFrame> latest_;
    std::deque<std::shared_ptr<SceneLightingFrame>> pending_;
};
}

namespace em2 {
bool SceneLightingEnabled();
bool SceneIndirectEnabled();
// Called after native HDR, ambient and surface resolve layers commit, while the memory lock is
// held, before resolve clears. Default-off experimental consumption path.
void ComposeNativeSceneLighting(rex::graphics::d3d12::D3D12RenderTargetCache*,rex::graphics::d3d12::D3D12CommandProcessor&,const rex::graphics::draw_util::ResolveInfo&);
}

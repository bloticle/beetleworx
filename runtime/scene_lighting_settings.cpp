#include "scene_lighting_settings.h"
#include "scene_lighting.h"
#include "renderer_graph.h"
#include "renderer_storage.h"
#include "gpu_constants_internal.h"
#include "gpu_sync.h"
#include "hdr_presenter.h"
#include "hdr_scene_cache.h"
#include "material_capture.h"
#include <rex/graphics/graphics_system.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <mutex>
#include <immintrin.h>
REXCVAR_DECLARE(bool,em2_apply_scene_lighting);
REXCVAR_DECLARE(bool,em2_apply_scene_indirect);
namespace {
using CP=rex::graphics::d3d12::D3D12CommandProcessor;
template<class Tag,auto Member>struct Access{friend auto MemberOf(Tag){return Member;}};
struct GraphicsTag{friend auto MemberOf(GraphicsTag);};template struct Access<GraphicsTag,&rex::graphics::CommandProcessor::graphics_system_>;
struct TargetsTag{friend auto MemberOf(TargetsTag);};template struct Access<TargetsTag,&CP::render_target_cache_>;
struct RemovedTag{friend auto MemberOf(RemovedTag);};template struct Access<RemovedTag,&CP::device_removed_>;
using Status=em2::SceneLightingSettingStatus;
class Requests {
    std::mutex mutex_;uint64_t revision_=0;Status status_=Status::Idle;
public:
    void Queue(uint64_t r){std::lock_guard lock(mutex_);if(r<=revision_)return;revision_=r;status_=Status::Queued;}
    void Finish(uint64_t r,Status s){std::lock_guard lock(mutex_);if(r==revision_)status_=s;}
    Status Get(){std::lock_guard lock(mutex_);return status_;}
}requests;
}
void em2::QueueSceneLightingSetting(uint64_t revision){requests.Queue(revision);}
em2::SceneLightingSettingStatus em2::SceneLightingSettingsStatus(){return requests.Get();}
em2::SceneLightingSettingStatus em2::ApplyRendererSceneLighting(Em2CommandProcessor& cp,int choice){
    const auto csr=_mm_getcsr();struct Fp{unsigned value;~Fp(){_mm_setcsr(value);}}fp{csr};
    if(!cp.IsGraphicsWorker() || choice<0 || choice>2)return Status::Failed;
    const bool enabled=choice!=0,indirect=choice==2;
    auto* graphics=cp.*MemberOf(GraphicsTag{});auto* current=(cp.*MemberOf(TargetsTag{})).get();
    if(!graphics || !current)return Status::Failed;
    if(enabled && current->GetPath()!=rex::graphics::RenderTargetCache::Path::kHostRenderTargets)return Status::Unsupported;
    const bool hdr=IsHdrPresenter(static_cast<rex::ui::d3d12::D3D12Presenter*>(graphics->presenter()));
    const auto encoding=enabled?HdrSceneEncoding::kNativeLinear:HdrSceneEncoding::kCompatibility;
    if(SceneLightingEnabled()==enabled && SceneIndirectEnabled()==indirect && HasHdrSceneCache(current)==(hdr || enabled) && HasMaterialCapture(current)==enabled && HasMaterialSurfaceCapture(current)==indirect && HasLinearMaterialSurfaceCapture(current)==indirect && GetHdrSceneEncoding(current)==encoding)return Status::Applied;
    GraphicsResourceCheckpoint checkpoint(cp);if(!checkpoint.valid())return Status::Busy;
    struct Work{RendererGraph stage,retired;RendererStorageSnapshot storage;PipelineShaderSnapshot shaders;ResolveRangeLedger ledger;};
    std::unique_ptr<Work> work;Status result=Status::Failed;
    try{
        work=std::make_unique<Work>();
        if(CaptureRendererStorage(cp,checkpoint,work->storage) && CapturePipelineShaders(cp,work->shaders)){
            work->shaders.hdr_scene=hdr || enabled;work->shaders.material_capture=enabled;work->shaders.hdr_encoding=encoding;
            work->shaders.material_surfaces=indirect;work->shaders.material_surface_linear=indirect;
            if(!enabled){work->shaders.material_surfaces=false;work->shaders.material_surface_linear=false;}
            if(BuildRendererGraph(cp,checkpoint,work->shaders,current->draw_resolution_scale_x(),current->draw_resolution_scale_y(),work->stage) &&
               RestoreRendererGraph(cp,checkpoint,std::move(work->storage),work->stage,work->ledger) &&
               PublishRendererGraph(cp,checkpoint,work->stage,work->ledger,work->retired)){
                REXCVAR_SET(em2_apply_scene_lighting,enabled);REXCVAR_SET(em2_apply_scene_indirect,indirect);result=Status::Applied;
            }
        }
    }catch(...){result=Status::Failed;}
    if(!cp.DrainForResourceChange()){
        work.release();cp.*MemberOf(RemovedTag{})=true;graphics->OnHostGpuLossFromAnyThread(false);result=Status::GpuFailure;
    }
    REXLOG_INFO("Live scene lighting mode={} completed status={}; display presenter retained",choice,int(result));return result;
}
void em2::ApplySceneLightingSetting(Em2CommandProcessor& cp,PCSettingRequest request){
    if(!cp.IsGraphicsWorker() || request.index!=kPCSceneLighting || !IsCurrentPCSettingRequest(request))return;
    const int previous=SceneIndirectEnabled()?2:SceneLightingEnabled()?1:0;const auto result=ApplyRendererSceneLighting(cp,request.choice);
    if(result!=Status::Applied)RevertPCSettingIfUnchanged(request,previous);
    requests.Finish(request.revision,result);
}
bool em2::TestSceneLightingRequestOrdering(){
    Requests state;state.Queue(1);state.Queue(2);state.Finish(1,Status::Failed);if(state.Get()!=Status::Queued)return false;
    state.Finish(2,Status::Applied);state.Queue(2);if(state.Get()!=Status::Applied)return false;
    state.Queue(3);state.Finish(2,Status::Applied);if(state.Get()!=Status::Queued)return false;
    state.Finish(3,Status::Unsupported);return state.Get()==Status::Unsupported;
}

#include "renderer_colour_mode.h"
#include "renderer_graph.h"
#include "renderer_storage.h"
#include "gpu_sync.h"
#include "gpu_constants_internal.h"
#include "hdr_presenter.h"
#include "hdr_scene_cache.h"
#include "material_capture.h"
#include "scene_lighting.h"
#include <rex/graphics/graphics_system.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <immintrin.h>
namespace {
using CP=rex::graphics::d3d12::D3D12CommandProcessor;
using Base=rex::graphics::CommandProcessor;
using G=rex::graphics::GraphicsSystem;
using P=rex::ui::d3d12::D3D12Presenter;
template<class Tag,auto Member>struct Access{friend auto MemberOf(Tag){return Member;}};
struct GraphicsTag{friend auto MemberOf(GraphicsTag);};template struct Access<GraphicsTag,&Base::graphics_system_>;
struct RenderTag{friend auto MemberOf(RenderTag);};template struct Access<RenderTag,&CP::render_target_cache_>;
struct RemovedTag{friend auto MemberOf(RemovedTag);};template struct Access<RemovedTag,&CP::device_removed_>;
struct PresenterTag{friend auto MemberOf(PresenterTag);};template struct Access<PresenterTag,&G::presenter_>;
struct LossTag{friend auto MemberOf(LossTag);};template struct Access<LossTag,&rex::ui::Presenter::host_gpu_loss_callback_>;
}
void em2::ExchangeGraphicsPresenter(G& graphics,std::unique_ptr<rex::ui::Presenter>& replacement){
    // The caller owns UI-thread attachment and has paused native GPU producers.
    (graphics.*MemberOf(PresenterTag{})).swap(replacement);
}
em2::LiveColourModeStatus em2::ApplyRendererColourMode(Em2CommandProcessor& cp,bool hdr,const PresenterExchange& exchange){
    using Status=LiveColourModeStatus;const auto csr=_mm_getcsr();struct Fp{unsigned csr;~Fp(){_mm_setcsr(csr);}}fp{csr};
    if(!cp.IsGraphicsWorker() || !exchange)return Status::RestoreFailed;
    auto* graphics=cp.*MemberOf(GraphicsTag{});auto* current=(cp.*MemberOf(RenderTag{})).get();if(!graphics || !current)return Status::RestoreFailed;
    auto* old_presenter=static_cast<P*>(graphics->presenter());
    const bool lighting=SceneLightingEnabled(),indirect=SceneIndirectEnabled();const auto encoding=lighting?HdrSceneEncoding::kNativeLinear:HdrSceneEncoding::kCompatibility;
    if((hdr || lighting) && current->GetPath()!=rex::graphics::RenderTargetCache::Path::kHostRenderTargets)return Status::Unsupported;
    if(HasHdrSceneCache(current)==(hdr || lighting) && HasMaterialCapture(current)==lighting && HasMaterialSurfaceCapture(current)==indirect && HasLinearMaterialSurfaceCapture(current)==indirect && GetHdrSceneEncoding(current)==encoding && IsHdrPresenter(old_presenter)==hdr)return Status::Applied;
    GraphicsResourceCheckpoint checkpoint(cp);if(!checkpoint.valid())return Status::Busy;
    struct Work {RendererGraph stage,retired;RendererStorageSnapshot storage;PipelineShaderSnapshot shaders;ResolveRangeLedger ledger;std::unique_ptr<rex::ui::Presenter> presenter;};
    std::unique_ptr<Work> work;Status result=Status::CaptureFailed;bool exchanged=false,fatal=false;rex::ui::Presenter* prepared=nullptr;
    try {
        work=std::make_unique<Work>();
        if(CaptureRendererStorage(cp,checkpoint,work->storage) && CapturePipelineShaders(cp,work->shaders)){
            work->shaders.hdr_scene=hdr || lighting;work->shaders.material_capture=lighting;work->shaders.material_surfaces=indirect;work->shaders.material_surface_linear=indirect;work->shaders.hdr_encoding=encoding;result=Status::BuildFailed;
            if(!lighting){work->shaders.material_surfaces=false;work->shaders.material_surface_linear=false;}
            if(BuildRendererGraph(cp,checkpoint,work->shaders,current->draw_resolution_scale_x(),current->draw_resolution_scale_y(),work->stage)){
                result=Status::RestoreFailed;
                if(RestoreRendererGraph(cp,checkpoint,std::move(work->storage),work->stage,work->ledger)){
                    const auto loss=old_presenter?old_presenter->*MemberOf(LossTag{}):rex::ui::Presenter::HostGpuLossCallback{};
                    work->presenter=hdr?CreateHdrPresenter(loss,cp.GetD3D12Provider()):P::Create(loss,cp.GetD3D12Provider());result=Status::PresenterFailed;
                    if(work->presenter && cp.DrainForResourceChange()){
                        prepared=work->presenter.get();
                        bool accepted=false;try{accepted=exchange(work->presenter);}catch(...){fatal=true;}
                        const bool consistent=graphics->presenter()==(accepted?prepared:old_presenter) && work->presenter.get()==(accepted?old_presenter:prepared);
                        if(!consistent)fatal=true;
                        if(accepted && consistent){exchanged=true;result=Status::RestoreFailed;
                            if(PublishRendererGraph(cp,checkpoint,work->stage,work->ledger,work->retired))result=Status::Applied;
                        }
                    }
                }
            }
        }
    }catch(...){result=Status::RestoreFailed;}
    // On failed publication the old graph is still active. Restore its matching
    // presenter before allowing producer execution to resume.
    if(exchanged && result!=Status::Applied){try{fatal=!exchange(work->presenter) || graphics->presenter()!=old_presenter || work->presenter.get()!=prepared;}catch(...){fatal=true;}}
    if(!cp.ReleaseHdrSwapBindings())fatal=true;
    if(fatal){
        // A failed fence or host rollback cannot safely retire either graph.
        work.release();cp.*MemberOf(RemovedTag{})=true;graphics->OnHostGpuLossFromAnyThread(false);result=Status::GpuFailure;
    }
    REXLOG_INFO("Live renderer colour mode HDR={} completed status={}; host owns surface/UI attachment",hdr,int(result));return result;
}

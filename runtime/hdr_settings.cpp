#include "hdr_settings.h"
#include "hdr_output.h"
#include "hdr_presenter.h"
#include "gpu_constants_internal.h"
#include <rex/graphics/graphics_system.h>
#include <mutex>
#include <rex/logging.h>
namespace {
class Status {
 std::mutex mutex_;uint64_t revision_=0;em2::HdrSettingStatus value_=em2::HdrSettingStatus::Idle;
public:
 void Queue(uint64_t r){std::lock_guard lock(mutex_);if(r<=revision_)return;revision_=r;value_=em2::HdrSettingStatus::Queued;}
 void Finish(uint64_t r,em2::HdrSettingStatus v){std::lock_guard lock(mutex_);if(r!=revision_)return;value_=v;}
 em2::HdrSettingStatus Get(){std::lock_guard lock(mutex_);return value_;}
}status;
template<class Tag,auto Member>struct Access{friend auto MemberOf(Tag){return Member;}};
struct GraphicsTag{friend auto MemberOf(GraphicsTag);};template struct Access<GraphicsTag,&rex::graphics::CommandProcessor::graphics_system_>;
}
void em2::QueueHdrSettingRequest(uint64_t r){status.Queue(r);}
void em2::FinishHdrSettingRequest(uint64_t r,HdrSettingStatus value){status.Finish(r,value);}
em2::HdrSettingStatus em2::HdrSettingsStatus(){return status.Get();}
void em2::ApplyHdrSettingRequest(Em2CommandProcessor& cp,PCSettingRequest request,const PresenterExchange& host){
 if(!cp.IsGraphicsWorker() || !IsCurrentPCSettingRequest(request))return;
 if(!cp.SetHdrSwapSettings(PCHdrOutputSettings())){if(request.index==kPCHdrMode)FinishHdrSettingRequest(request.revision,HdrSettingStatus::Failed);return;}
 if(request.index!=kPCHdrMode)return;
 auto* graphics=cp.*MemberOf(GraphicsTag{});const int previous=IsHdrPresenter(static_cast<rex::ui::d3d12::D3D12Presenter*>(graphics->presenter()))?1:0;
 bool exchanged=false;auto exchange=[&](std::unique_ptr<rex::ui::Presenter>& candidate){if(!exchanged && !IsCurrentPCSettingRequest(request))return false;const bool accepted=host(candidate);if(accepted)exchanged=true;return accepted;};
 const auto result=ApplyRendererColourMode(cp,request.choice!=0,exchange);
 if(result!=LiveColourModeStatus::Applied)RevertPCSettingIfUnchanged(request,previous);
 HdrSettingStatus value=HdrSettingStatus::Failed;
 if(result==LiveColourModeStatus::Applied)value=HdrSettingStatus::Applied;
 else if(result==LiveColourModeStatus::Busy)value=HdrSettingStatus::Busy;
 else if(result==LiveColourModeStatus::Unsupported)value=HdrSettingStatus::Unsupported;
 else if(result==LiveColourModeStatus::PresenterFailed)value=HdrSettingStatus::Unavailable;
 FinishHdrSettingRequest(request.revision,value);
}
bool em2::TestHdrSettingRequestOrdering(){
 Status local;local.Queue(1);local.Queue(2);local.Finish(1,HdrSettingStatus::Failed);if(local.Get()!=HdrSettingStatus::Queued)return false;local.Finish(2,HdrSettingStatus::Applied);local.Queue(2);if(local.Get()!=HdrSettingStatus::Applied)return false;local.Queue(3);local.Finish(2,HdrSettingStatus::Applied);if(local.Get()!=HdrSettingStatus::Queued)return false;local.Finish(3,HdrSettingStatus::Unavailable);if(local.Get()!=HdrSettingStatus::Unavailable)return false;
 REXLOG_INFO("HDR setting status passed: latest revision owns completion, stale failure and duplicate queue ignored");return true;
}

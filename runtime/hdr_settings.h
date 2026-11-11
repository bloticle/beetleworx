#pragma once
#include "pc_settings.h"
#include "renderer_colour_mode.h"
namespace em2 {
enum class HdrSettingStatus {Idle,Queued,Applied,Busy,Unsupported,Failed,Unavailable};
void QueueHdrSettingRequest(uint64_t revision);
void FinishHdrSettingRequest(uint64_t revision,HdrSettingStatus);
HdrSettingStatus HdrSettingsStatus();
// Command worker; host callback marshals attachment to UI. Only the initial
// exchange rejects stale requests. Rollback must run despite a newer request.
void ApplyHdrSettingRequest(Em2CommandProcessor&,PCSettingRequest,const PresenterExchange&);
bool TestHdrSettingRequestOrdering();
}

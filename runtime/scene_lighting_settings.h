#pragma once
#include "pc_settings.h"
namespace em2 {
class Em2CommandProcessor;
enum class SceneLightingSettingStatus {Idle,Queued,Applied,Busy,Unsupported,Failed,GpuFailure};
void QueueSceneLightingSetting(uint64_t revision);
SceneLightingSettingStatus SceneLightingSettingsStatus();
// GPU worker, outside guest packets. Rebuilds immutable material/HDR caches
// under a resource checkpoint and preserves the current display presenter.
// 0 original, 1 material AO, 2 AO + screen-space diffuse bounce.
SceneLightingSettingStatus ApplyRendererSceneLighting(Em2CommandProcessor&,int choice);
void ApplySceneLightingSetting(Em2CommandProcessor&,PCSettingRequest);
bool TestSceneLightingRequestOrdering();
}

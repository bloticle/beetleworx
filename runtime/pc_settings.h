#pragma once
#include <filesystem>
#include <functional>
#include <cstdint>

namespace rex::ui {class Presenter;}
namespace em2 {
constexpr int kPCOptionBase = 1000;
constexpr int kPCOptionCount = 40;
constexpr int kPCRestoredGremlinNames=38;
constexpr int kPCOriginalInkDrips=39;
constexpr int kPCCenterOnMouse=29,kPCCenterOnAim=30,kPCConfineMouse=31,kPCHideMouse=32;
constexpr int kPCMousePriority=33,kPCMouseTakeover=34,kPCCenterOnCapture=35,kPCMouseGainX=36,kPCMouseGainY=37;
constexpr int kPCDistanceShadows=28;
constexpr int kPCSceneLighting=25;
constexpr int kPCScreenUpscaler=26,kPCUpscaleSharpness=27;
void ApplyPCPresentationSettings(rex::ui::Presenter&);
constexpr int kPCHdrMode=21,kPCHdrPaperWhite=22,kPCHdrPeak=23,kPCHdrExposure=24;
struct HdrOutputSettings;
HdrOutputSettings PCHdrOutputSettings();
void LoadPCSettings(const std::filesystem::path& directory);
int PCSetting(int index);
const char* PCSettingName(int index);
int PCSettingChoiceCount(int index);
int PCSettingDefault(int index);
int ChangePCSetting(int index, int choice);
struct PCSettingRequest {int index=-1,choice=-1;uint64_t revision=0;};
PCSettingRequest SnapshotPCSetting(int index);
bool IsCurrentPCSettingRequest(PCSettingRequest request);
// Async apply failure: undo this exact request without notifying again. A newer
// selection of the same choice is a different request and must survive.
bool RevertPCSettingIfUnchanged(PCSettingRequest request,int actual);
int FrameLimit();
float GameplayViewWidth();
int CameraResponse();
bool MouseCameraEnabled();
struct MouseCursorOptions {
    bool center_switch=true,center_aim=true,confine=true,hide=true,always_mouse=false,center_capture=true;
    unsigned takeover_counts=2;
    float gain_x=1,gain_y=1;
};
MouseCursorOptions GetMouseCursorOptions();
// Wait before the native update samples input. Presentations outside an outer
// game frame (for example loading/movie paths) retain the fallback limiter.
class GuestFramePacingScope {
public:
    GuestFramePacingScope();
    ~GuestFramePacingScope();
    GuestFramePacingScope(const GuestFramePacingScope&)=delete;
    GuestFramePacingScope& operator=(const GuestFramePacingScope&)=delete;
};
void PaceGuestPresentation();
void ApplyPCStartupSettings();
// Invoke on the command-processor thread; shader replacement occurs before draw.
void ApplyPCLiveGPUSetting(int index);
unsigned NativeLightingSettings();
void WriteGameConfigOverlay(const std::filesystem::path& original,
                            const std::filesystem::path& destination);
void SetPCSettingsChangedCallback(std::function<void(PCSettingRequest)> callback);
void SetPCSupportActionCallback(std::function<void(int,int)> callback);
bool NativePCSupportOption(int id,int command,int& result);
bool TestPCSettings(const std::filesystem::path& temporary_directory,
                    const std::filesystem::path& original_config);
}

#pragma once
#include "online.h"
#include "online_campaigns.h"
namespace em2::net {
enum class NativeSessionState {None,WaitingForWorld,Joining,Attached,Failed};
struct NativeSessionStatus {
    NativeSessionState state=NativeSessionState::None;std::string error;
    bool continue_requested=false,profile_requested=false,save_notice_requested=false;InputWait wait=InputWait::None;
    // Immutable owner-thread failure context copied for support/UI readers.
    std::string diagnostic_report;
};
struct NativeSessionTiming {bool fixed_step=false,halted=false;};
// Startup-thread call before the guest starts, or through the guarded live
// menu preparation API below. On rejection, ownership stays with the caller.
// Optional prelaunch polling has sole ownership until main
// guest binding joins it; after handoff only the simulation thread polls.
bool AdoptNativeOnlineSession(std::unique_ptr<Session>& ready,unsigned local_device=0);
// Live title-menu owner only. Suppresses auto-sign-in, auto-Continue and input
// timeline attachment until the original SaveManager reload/Resume completes.
bool BeginNativeOnlineMenuPreparation(std::unique_ptr<Session>& ready);
bool FinishNativeOnlineMenuPreparation();
// Owner-thread only, after audited original archive decode/summary validation.
// Clients return an empty success; only the actual ready host can publish.
HostCampaignRecord RecordNativeOnlineHostCampaign(const std::filesystem::path& normal_user_data,
    const std::filesystem::path& private_user_data);
void FailNativeOnlineMenuPreparation(const std::string& error);
NativeSessionStatus NativeOnlineStatus();
// Single-flight selection through the original native controller-join menu handler.
bool ClaimNativeOnlineProfile(unsigned& device);
void FinishNativeOnlineProfile(bool accepted);
// Atomic authorization used only by the authored existing-save autosave notice.
bool ClaimNativeOnlineSaveNotice();
// Single-flight native Continue request; a rejected native transition may retry.
bool ClaimNativeOnlineContinue();
void FinishNativeOnlineContinue(bool accepted);
// Timing is fixed from adoption, before world/co-op readiness. A failed owner
// stays halted until shutdown/resynchronization even if no gate was attached.
NativeSessionTiming NativeOnlineTiming();
// Fast policy snapshot for UI/worker/simulation code. Includes retained startup
// and failed sessions before an input gate exists, plus standalone QA gates.
bool SynchronizedSessionActive() noexcept;
// Retained legacy Session/InputGate ownership only; a Room bridge must not
// accidentally acquire the same native controls or RNG lease.
bool LegacyNativeSessionActive() noexcept;
// Native owner thread only, including retained startup before input attachment.
// This is display ownership, never the number of simulated human actors.
std::optional<unsigned> NativeOnlineDisplaySlot();
// Bind at the main guest entry, before its first synchronous asset load.
bool BindNativeOnlineThread();
// Startup thread only, before launching any guest threads. A temporary sole
// transport owner keeps the ready connection alive through runtime/GPU setup.
// Main guest binding stops and joins it before claiming simulation ownership.
bool StartNativeOnlinePrelaunchPolling();
// Outer frames poll only. Advance world preparation only at the boundary
// after the previous native update has been finalized.
bool UpdateNativeOnlineSession(uint8_t* guest_base,bool advance_world=false);
// Called only after the original level loader and asset/script commits succeed.
// Startup can acquire the input timeline at the next completed update instead
// of advancing scripts independently while waiting for a gameplay camera.
void NotifyNativeOnlineLevelLoaded(std::string_view destination);
bool NativeOnlineLocalMenuPauseRequested();
// Nonblocking transport service from the native loading-frame callback. Only
// the established main/simulation owner may poll.
// Does not test world readiness, attach controls, or consume gameplay ticks.
bool PollNativeOnlineLoading();
// Shutdown-thread call only after every guest thread has stopped.
void ReleaseNativeOnlineSession();
bool TestNativeOnlineSession();
}

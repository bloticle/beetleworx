#pragma once
#include "online_runtime.h"
#include "online_room_bootstrap.h"
namespace em2::net {
// Retained native startup owner. Adoption does not assert a loaded world or
// producer readiness, and never creates another executable/native runtime.
bool AdoptNativeRoom(std::unique_ptr<Room>& frozen_loading,unsigned local_device=0);
bool NativeRoomActive()noexcept;
NativeSessionStatus NativeRoomStatus();
NativeSessionTiming NativeRoomTiming();
std::optional<unsigned> NativeRoomDisplaySlot();
bool BindNativeRoomThread();
bool StartNativeRoomPrelaunchPolling();
bool PollNativeRoomLoading();
bool UpdateNativeRoomSession(uint8_t* guest_base,bool advance_world=false);
void NotifyNativeRoomLevelLoaded(std::string_view destination);
void ReleaseNativeRoomSession();
// UI shutdown must drain the owner-thread report before the SDK forcibly
// stops guest threads. No new history dump may enter after this returns.
void CloseNativeRoomDiagnostics();
void CancelNativeRoomLoads()noexcept;
bool NativeRoomLoadsCancelled()noexcept;
std::optional<std::array<uint8_t,32>> NativeRoomLoadedWorldToken();
bool RoomPrepareWorldRandom(std::string_view destination);
bool RoomPreparePlayerRandom(uint32_t slot,uint32_t mode);
bool RoomClaimProfile(unsigned& local_device);
void RoomFinishProfile(bool accepted);
bool RoomClaimContinue();
void RoomFinishContinue(bool accepted);
bool RoomClaimSaveNotice();
// Owner thread, at a completed boundary only. No probe means NOT ready. May be
// supplied before gate attachment, or delegated to its boundary-checked setter.
bool SetNativeRoomReadinessProbe(RoomBootstrapGate::ReadinessProbe);
// Explicit development gameplay owner; requires the retained frozen native
// Room, native thread and real runtime input system. No implicit demo opt-in.
bool EnableNativeRoomGameplay(unsigned physical_user);
bool BeginNativeRoomMenuPreparation(std::unique_ptr<Room>&);
bool FinishNativeRoomMenuPreparation();
void FailNativeRoomMenuPreparation(const std::string&);
HostCampaignRecord RecordNativeRoomHostCampaign(const std::filesystem::path& normal,const std::filesystem::path& session);
// Explicit isolated QA only: read completed native leave-boundary eligibility.
// Never releases actors/controls, resumes simulation, changes saves or loads UI.
void ProbeNativeRoomLeaveBoundary(PPCContext&,uint8_t*,bool completed_outer_boundary);
class NativeRoomActors;
std::unique_ptr<NativeRoomActors> TakeNativeRoomActorsForTitleLeave(PPCContext&,uint8_t*);
bool TestNativeRoomSession();
}

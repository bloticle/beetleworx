#pragma once
#include "online_room.h"
#include <atomic>
#include <memory>
struct PPCContext;
namespace rex::input {class InputSystem;}
namespace em2::net {
struct OnlineNameActor;
struct OwnedActorInput;
class RoomBootstrapGate;
class NativeRoomActors;
enum class RoomGameplayAdmission {Inactive,Waiting,Ready,Stopped,WrongThread};
// Two retail humans plus native world-owned extra humans. Owns no
// transport/controls/RNG lease: the retained
// native Room and its raw gate outlive this adapter through gameplay cutover.
// Configure is explicit; ordinary/demo launches never opt in implicitly.
class NativeRoomGameplay {
public:
    NativeRoomGameplay();
    ~NativeRoomGameplay();
    NativeRoomGameplay(const NativeRoomGameplay&)=delete;
    NativeRoomGameplay& operator=(const NativeRoomGameplay&)=delete;
    bool Configure(Room&,RoomBootstrapGate&,rex::input::InputSystem&,unsigned physical_user);
    void Detach();
    // Mutating actor admission at an agreed completed native update. Kept
    // separate from Readiness, which must not construct/admit world entities.
    bool InitializeActors(PPCContext&,uint8_t*);
    uint16_t extra_actor_mask()const;
    // Count only actual admitted humans during this owned native update. Zero means unavailable.
    uint32_t NativeDeathParticipantCount(PPCContext&,uint8_t*)const;
    bool OwnedActorForPeer(uint64_t room_peer,PPCContext&,uint8_t*,OwnedActorInput&)const;
    uint32_t LocalCameraController()const;
    uint32_t LocalHudActor(unsigned profile,PPCContext&,uint8_t*)const;
    bool OwnsCameraMode(uint32_t,uint8_t*)const;
    bool NameTagActors(std::vector<OnlineNameActor>&,uint64_t& local_peer,uint8_t*,std::array<uint8_t,16>* session=nullptr)const;
    bool RenderBindingsAvailable(uint8_t*)const;
    bool NativeLoadPoint(std::string_view,LoadPhase,bool,PPCContext&,uint8_t*);
    bool transition_pending()const;
    bool CompleteWorldTransition(PPCContext&,uint8_t*);
    void UpdateExtraCameras(PPCContext&,uint8_t*);
    // Movie-only owner-thread lease for original Bink playback scheduling.
    // Between native updates the media clock holds the last completed tick.
    bool ReadMovieClock(uint32_t movie_manager,PPCContext&,uint8_t*,uint64_t& tick)const;
    bool Readiness(PPCContext&,uint8_t*);
    RoomGameplayAdmission Prepare(PPCContext&,uint8_t*,RoomFrame&);
    bool Commit(const RoomFrame&,PPCContext&,uint8_t*);
    // Called only AFTER StartTick has established the agreed native tick.
    bool Deliver(PPCContext&,uint8_t*);
    // Called only AFTER original native post-update and a real observation.
    bool Complete(const std::array<uint8_t,32>& digest,PPCContext&,uint8_t*);
    void ObservationFailed();
    bool active()const;
    bool halted()const;
    bool waiting()const;
    bool completion_pending()const;
    // Only player damage in the admitted guest's agreed native update.
    // Props/NPCs retain dispatch until their event/lifecycle replication exists.
    // replaying a host correction is authorized separately by its native scope.
    bool SuppressLocalDamage(uint32_t target_handle)const;
    // Private integration probe: record causal root broadcasts in its scope.
    // Returns true when the caller must quarantine nested capture until return.
    bool RecordDamageEvent(uint32_t event,uint32_t listener,uint32_t caller,uint8_t*);
    // Private target-selection authority. Human brains remain outside this
    // scope; guest native setters execute at the original selector call site.
    bool ReplayAiTargetDecision(PPCContext&,uint8_t*);
    std::optional<size_t> BeginAiTargetDecision(uint32_t brain,uint8_t*);
    bool FinishAiTargetDecision(size_t ticket,uint32_t brain,uint32_t setter_calls,uint32_t setter_caller,uint32_t target,uint8_t*);
    bool OverridePrivateAiTarget(uint32_t brain,uint32_t& target,uint8_t*);
    // Private full-spawner transport/capture comparison. Does not replay or
    // suppress births; addresses are used only to bind stable player IDs.
    bool RecordSpawnBatch(SpawnBatch,uint32_t owner,uint32_t anchor,uint8_t*);
    // Private host eligibility: true authorizes a null result only at the
    // audited caller, without entering the native parent or changing counters.
    bool SuppressSpawnBatch(const PPCContext&,uint8_t*,unsigned depth);
    bool passive_input_pending()const;
    uint64_t committed_tick()const;
    // Owner-thread read only, outside the native update. Eligibility proves
    // completed input/physics ownership, not storage idle or renderer fences.
    const char* LeaveBoundaryBlocker(uint8_t*)const;
    // Original local physical source only, on the stopped world's owner thread.
    bool SampleRecoveryButtons(uint16_t&)const;
    std::unique_ptr<NativeRoomActors> PrepareTitleUnload(PPCContext&,uint8_t*);
    std::optional<uint64_t> last_native_completed_tick()const;
    const std::string& error()const;
private:
    bool RearmInputContext(uint64_t source_tick,PPCContext&,uint8_t*);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
NativeRoomGameplay* CurrentNativeRoomGameplay()noexcept;
bool NativeRoomGameplayReadiness(PPCContext&,uint8_t*);
bool NativeRoomGameplayHalted()noexcept;
bool NativeRoomGameplayWaiting()noexcept;
bool NativeRoomRenderBindingsAvailable(uint8_t*);
uint32_t NativeRoomLocalCameraController();
bool SuppressNativeRoomCameraLook(PPCContext&,uint8_t*);
bool NativeRoomOwnsCameraMode(uint32_t,uint8_t*);
// Real HUD polling for extra humans after late admission; false until the
// current world's local actor is ready. Never changes canonical health.
bool ReadNativeRoomHudLife(unsigned profile,PPCContext&,uint8_t*,double&);
}

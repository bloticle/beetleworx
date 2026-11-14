#pragma once
#include "online_room.h"
#include <atomic>
#include <memory>
struct PPCContext;
namespace rex::input {class InputSystem;}
namespace em2::net {
struct OnlineNameActor;
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
    uint32_t LocalCameraController()const;
    uint32_t LocalHudActor(unsigned profile,PPCContext&,uint8_t*)const;
    bool OwnsCameraMode(uint32_t,uint8_t*)const;
    bool NameTagActors(std::vector<OnlineNameActor>&,uint64_t& local_peer,uint8_t*,std::array<uint8_t,16>* session=nullptr)const;
    bool RenderBindingsAvailable(uint8_t*)const;
    bool NativeLoadPoint(std::string_view,LoadPhase,bool,PPCContext&,uint8_t*);
    bool transition_pending()const;
    bool CompleteWorldTransition(PPCContext&,uint8_t*);
    void UpdateExtraCameras(PPCContext&,uint8_t*);
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
    uint64_t committed_tick()const;
    // Owner-thread read only, outside the native update. Eligibility proves
    // completed input/physics ownership, not storage idle or renderer fences.
    const char* LeaveBoundaryBlocker(uint8_t*)const;
    std::unique_ptr<NativeRoomActors> PrepareTitleUnload(PPCContext&,uint8_t*);
    std::optional<uint64_t> last_native_completed_tick()const;
    const std::string& error()const;
private:
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
}
